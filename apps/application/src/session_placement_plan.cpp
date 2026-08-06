#include "session_placement_plan.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "axklib/catalog.hpp"
#include "axklib/media.hpp"
#include "axklib/relationship.hpp"

namespace axk::app::detail {
namespace {

Error plan_error(std::string code, std::string message) { return {std::move(code), std::move(message)}; }

bool repairable_object_type(std::string_view type) {
    constexpr std::array types{std::string_view{"SMPL"}, std::string_view{"SBNK"}, std::string_view{"SBAC"},
                               std::string_view{"PROG"}, std::string_view{"SEQU"}, std::string_view{"PRF3"}};
    return std::ranges::contains(types, type);
}

bool valid_volume_name(std::string_view name) {
    return !name.empty() && name.size() <= 16U && name.front() != ' ' && name.back() != ' ' &&
           std::ranges::all_of(name, [](unsigned char character) { return character >= 0x20U && character <= 0x7eU; });
}

std::string unique_recovery_name(const std::set<std::string> &volume_names) {
    if (!volume_names.contains("Recovered"))
        return "Recovered";
    for (std::size_t suffix = 2U; suffix <= 999U; ++suffix) {
        const auto candidate = std::format("Recovered {}", suffix);
        if (candidate.size() <= 16U && !volume_names.contains(candidate))
            return candidate;
    }
    return {};
}

void append_destination(std::map<std::string, PlacementRepairDestination> &destinations, std::string volume_name,
                        bool creates_volume, std::span<const ObjectSnapshot *const> objects) {
    auto destination = destinations
                           .try_emplace(volume_name, PlacementRepairDestination{.volume_name = std::move(volume_name),
                                                                                .creates_volume = creates_volume,
                                                                                .object_sfs_ids = {},
                                                                                .object_type_counts = {}})
                           .first;
    destination->second.creates_volume = destination->second.creates_volume || creates_volume;
    for (const auto *object : objects) {
        destination->second.object_sfs_ids.push_back(object->sfs_id);
        ++destination->second.object_type_counts[object->object.header.raw_type];
    }
}

void append_blocker(std::map<std::string, PlacementRepairBlocker> &blockers, std::string code, std::string message,
                    std::size_t object_count) {
    auto blocker =
        blockers.try_emplace(code, PlacementRepairBlocker{.code = std::move(code), .message = std::move(message)})
            .first;
    blocker->second.object_count += object_count;
}

} // namespace

std::size_t PlacementRepairPlan::repair_object_count() const noexcept {
    std::size_t result{};
    for (const auto &destination : destinations)
        result += destination.object_sfs_ids.size();
    return result;
}

Result<PlacementRepairPlan> plan_placement_repair(const ImageSessionRead &session, PlacementRepairScope scope,
                                                  std::optional<std::string> recovery_volume_name) {
    const auto *container = session.media == nullptr ? nullptr : std::get_if<Container>(&session.media->storage());
    if (container == nullptr ||
        !std::ranges::contains(container->partitions(), scope.partition_index,
                               [](const Partition &partition) { return partition.index.value; })) {
        return std::unexpected(plan_error("placement_scope_invalid", "partition does not exist"));
    }

    std::set<std::string> volume_names;
    for (const auto &[id, volume] : session.volume_scopes_by_id) {
        if (volume.partition_index == scope.partition_index)
            volume_names.insert(volume.display_name);
    }
    if (scope.volume_name && !volume_names.contains(*scope.volume_name)) {
        return std::unexpected(plan_error("placement_scope_invalid", "volume does not exist in the partition"));
    }
    if (recovery_volume_name &&
        (!valid_volume_name(*recovery_volume_name) || volume_names.contains(*recovery_volume_name))) {
        return std::unexpected(plan_error(
            "invalid_request", "recoveryVolumeName must be a new 1..16 character printable ASCII volume name"));
    }

    ObjectCatalog catalog;
    catalog.issues = session.catalog_issues;
    std::vector<const ObjectSnapshot *> objects;
    std::unordered_map<std::string, std::size_t> indices;
    for (const auto *object : session.catalog_objects) {
        catalog.objects.push_back(*object);
        if (object->partition.value != scope.partition_index)
            continue;
        indices.emplace(object->key, objects.size());
        objects.push_back(object);
    }
    const auto graph = build_relationship_graph(catalog);
    std::vector<std::vector<std::size_t>> adjacency(objects.size());
    for (const auto &relationship : graph.relationships) {
        if (relationship.quality != RelationshipQuality::known || !relationship.target_key)
            continue;
        const auto source = indices.find(relationship.source_key);
        const auto target = indices.find(*relationship.target_key);
        if (source == indices.end() || target == indices.end())
            continue;
        adjacency[source->second].push_back(target->second);
        adjacency[target->second].push_back(source->second);
    }

    std::map<std::string, PlacementRepairDestination> destinations;
    std::map<std::string, PlacementRepairBlocker> blockers;
    std::vector<const ObjectSnapshot *> ownerless;
    std::vector<bool> visited(objects.size());
    for (std::size_t start = 0U; start < objects.size(); ++start) {
        if (visited[start])
            continue;
        std::vector<std::size_t> pending{start};
        visited[start] = true;
        std::vector<const ObjectSnapshot *> missing;
        std::set<std::string> anchors;
        std::size_t ambiguous_count{};
        std::size_t unsupported_count{};
        while (!pending.empty()) {
            const auto index = pending.back();
            pending.pop_back();
            const auto *object = objects[index];
            if (object->placement_resolution == PlacementResolution::exact && object->placement) {
                anchors.insert(object->placement->volume_name);
            } else if (object->placement_resolution == PlacementResolution::missing && !object->placement &&
                       object->placement_candidates.empty()) {
                missing.push_back(object);
                if (!repairable_object_type(object->object.header.raw_type))
                    ++unsupported_count;
            } else {
                ++ambiguous_count;
            }
            for (const auto adjacent : adjacency[index]) {
                if (!visited[adjacent]) {
                    visited[adjacent] = true;
                    pending.push_back(adjacent);
                }
            }
        }

        const auto relevant = !scope.volume_name || anchors.contains(*scope.volume_name);
        const auto damaged_count = missing.size() + ambiguous_count;
        if (!relevant || damaged_count == 0U)
            continue;
        if (ambiguous_count != 0U) {
            append_blocker(blockers, "PLACEMENT_AMBIGUOUS",
                           "An object component has ambiguous placement candidates and was left unchanged",
                           damaged_count);
            continue;
        }
        if (unsupported_count != 0U) {
            append_blocker(blockers, "PLACEMENT_OBJECT_TYPE_UNSUPPORTED",
                           "An object component contains a type that placement repair cannot write", damaged_count);
            continue;
        }
        if (anchors.size() > 1U) {
            append_blocker(blockers, "PLACEMENT_MULTIPLE_VOLUME_OWNERS",
                           "Known relationships connect the missing objects to more than one volume", damaged_count);
            continue;
        }
        if (anchors.empty()) {
            ownerless.insert(ownerless.end(), missing.begin(), missing.end());
            continue;
        }
        append_destination(destinations, *anchors.begin(), false, missing);
    }

    if (!ownerless.empty()) {
        auto recovery_name = recovery_volume_name ? *recovery_volume_name : unique_recovery_name(volume_names);
        if (recovery_name.empty()) {
            return std::unexpected(
                plan_error("placement_repair_unavailable", "no unique recovery volume name is available"));
        }
        append_destination(destinations, std::move(recovery_name), true, ownerless);
    }

    PlacementRepairPlan result{
        .scope = std::move(scope), .destinations = {}, .blockers = {}, .blocked_object_count = 0U};
    for (auto &[name, destination] : destinations) {
        std::ranges::sort(destination.object_sfs_ids, {}, &SfsId::value);
        result.destinations.push_back(std::move(destination));
    }
    for (auto &[code, blocker] : blockers) {
        result.blocked_object_count += blocker.object_count;
        result.blockers.push_back(std::move(blocker));
    }
    return result;
}

} // namespace axk::app::detail
