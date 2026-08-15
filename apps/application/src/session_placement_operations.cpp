#include "axklib/application/session_placement_operations.hpp"

#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/alteration.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/catalog.hpp"
#include "axklib/relationship.hpp"
#include "axklib/sfs.hpp"
#include "session_placement_plan.hpp"

namespace {

using Json = nlohmann::json;

struct PlacementRequest {
    std::string image_id;
    std::uint64_t revision{};
    axk::app::detail::PlacementRepairScope scope;
    std::optional<std::string> recovery_volume_name;
};

axk::app::Error operation_error(std::string code, std::string message) { return {std::move(code), std::move(message)}; }

axk::app::Result<std::tuple<std::string, std::uint64_t, std::uint8_t, std::string>>
parse_volume_deletion_request(const Json &input) {
    try {
        const auto image_id = input.at("imageId").get<std::string>();
        const auto revision = input.at("expectedRevision").get<std::uint64_t>();
        const auto partition_value = input.at("partitionIndex").get<std::uint64_t>();
        const auto volume_name = input.at("volumeName").get<std::string>();
        if (image_id.empty() || revision == 0U || partition_value > 7U || volume_name.empty()) {
            return std::unexpected(operation_error(
                "invalid_request", "imageId, expectedRevision, partitionIndex, and volumeName are required"));
        }
        return std::tuple{image_id, revision, static_cast<std::uint8_t>(partition_value), volume_name};
    } catch (const Json::exception &) {
        return std::unexpected(operation_error(
            "invalid_request", "imageId, expectedRevision, partitionIndex, and volumeName are required"));
    }
}

axk::app::Result<PlacementRequest> parse_placement_request(const Json &input) {
    try {
        PlacementRequest result;
        result.image_id = input.at("imageId").get<std::string>();
        result.revision = input.at("expectedRevision").get<std::uint64_t>();
        const auto &scope = input.at("scope");
        const auto kind = scope.at("kind").get<std::string>();
        const auto partition_value = scope.at("partitionIndex").get<std::uint64_t>();
        if (result.image_id.empty() || result.revision == 0U || partition_value > 7U ||
            (kind != "PARTITION" && kind != "VOLUME")) {
            return std::unexpected(operation_error("invalid_request", "placement repair scope is invalid"));
        }
        result.scope.partition_index = static_cast<std::uint8_t>(partition_value);
        if (kind == "VOLUME") {
            result.scope.volume_name = scope.at("volumeName").get<std::string>();
            if (result.scope.volume_name->empty())
                return std::unexpected(operation_error("invalid_request", "volumeName is required for VOLUME scope"));
        } else if (scope.contains("volumeName")) {
            return std::unexpected(operation_error("invalid_request", "PARTITION scope must not include volumeName"));
        }
        if (input.contains("recoveryVolumeName"))
            result.recovery_volume_name = input.at("recoveryVolumeName").get<std::string>();
        if (result.scope.volume_name && result.recovery_volume_name) {
            return std::unexpected(
                operation_error("invalid_request", "recoveryVolumeName is only valid for PARTITION scope"));
        }
        return result;
    } catch (const Json::exception &) {
        return std::unexpected(operation_error(
            "invalid_request", "imageId, expectedRevision, and a discriminated placement scope are required"));
    }
}

axk::app::Result<std::set<axk::SfsId>> physical_volume_closure(const axk::Partition &partition,
                                                               const axk::DirectoryEntry &volume) {
    std::set<axk::SfsId> result;
    std::vector<axk::SfsId> pending{axk::SfsId{volume.link_id.value}};
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        if (result.contains(id))
            continue;
        const auto item = std::ranges::find(partition.records, id, &axk::IndexRecord::sfs_id);
        if (item == partition.records.end()) {
            return std::unexpected(
                operation_error("volume_closure_invalid", "volume closure references a missing SFS record"));
        }
        result.insert(id);
        if (item->payload_kind != axk::PayloadKind::directory)
            continue;
        for (const auto &child : item->directory_entries) {
            if (child.name != "." && child.name != "..")
                pending.push_back(axk::SfsId{child.link_id.value});
        }
    }
    for (const auto &item : partition.records) {
        if (result.contains(item.sfs_id) || item.sfs_id.value == 1U ||
            item.payload_kind != axk::PayloadKind::directory) {
            continue;
        }
        for (const auto &child : item.directory_entries) {
            if (child.name != "." && child.name != ".." && result.contains(axk::SfsId{child.link_id.value})) {
                return std::unexpected(
                    operation_error("volume_closure_invalid", "a directory outside the volume references its closure"));
            }
        }
    }
    return result;
}

axk::app::Result<std::set<axk::SfsId>> resolve_volume_closure(const axk::app::ImageSessionRead &session,
                                                              std::uint8_t partition_index,
                                                              std::string_view volume_name) {
    if (axk::is_partition_support_root_entry(volume_name)) {
        return std::unexpected(
            operation_error("volume_scope_invalid", "PRF3 is a reserved partition support directory"));
    }
    const auto *container = session.media == nullptr ? nullptr : std::get_if<axk::Container>(&session.media->storage());
    if (container == nullptr) {
        return std::unexpected(
            operation_error("volume_scope_invalid", "volume deletion requires an SFS image partition"));
    }
    const auto partition = std::ranges::find(container->partitions(), partition_index,
                                             [](const axk::Partition &item) { return item.index.value; });
    if (partition == container->partitions().end())
        return std::unexpected(operation_error("volume_scope_invalid", "partition does not exist"));
    const auto root = std::ranges::find(partition->records, axk::SfsId{1U}, &axk::IndexRecord::sfs_id);
    if (root == partition->records.end() || root->payload_kind != axk::PayloadKind::directory) {
        return std::unexpected(operation_error("volume_closure_invalid", "partition root directory is unavailable"));
    }
    std::vector<const axk::DirectoryEntry *> matches;
    for (const auto &entry : root->directory_entries) {
        if (entry.name == volume_name)
            matches.push_back(&entry);
    }
    if (matches.size() != 1U) {
        return std::unexpected(
            operation_error("volume_scope_invalid", "volume name is not unique in the selected partition"));
    }
    return physical_volume_closure(*partition, *matches.front());
}

axk::app::Result<Json> inspect_volume_deletion(axk::app::ImageSessionManager &images, std::string_view owner_id,
                                               std::string_view image_id, std::uint64_t revision,
                                               std::uint8_t partition_index, std::string_view volume_name) {
    auto session = images.begin_read(image_id, owner_id, revision);
    if (!session)
        return std::unexpected(session.error());

    auto closure = resolve_volume_closure(*session, partition_index, volume_name);
    if (!closure)
        return std::unexpected(closure.error());

    axk::ObjectCatalog catalog;
    catalog.issues = session->catalog_issues;
    std::map<std::string, axk::SfsId> object_ids;
    for (const auto *object : session->catalog_objects) {
        catalog.objects.push_back(*object);
        if (object->partition.value == partition_index)
            object_ids.emplace(object->key, object->sfs_id);
    }
    const auto graph = axk::build_relationship_graph(catalog);
    const auto crossing_count = std::ranges::count_if(graph.relationships, [&](const axk::Relationship &relationship) {
        if (relationship.quality != axk::RelationshipQuality::known || !relationship.target_key)
            return false;
        const auto source = object_ids.find(relationship.source_key);
        const auto target = object_ids.find(*relationship.target_key);
        return source != object_ids.end() && target != object_ids.end() &&
               closure->contains(source->second) != closure->contains(target->second);
    });
    Json blockers = Json::array();
    if (crossing_count != 0U) {
        blockers.push_back({{"code", "KNOWN_RELATIONSHIP_CROSSES_VOLUME"},
                            {"message", "A known object relationship crosses the volume closure"},
                            {"count", crossing_count}});
    }
    return Json{{"imageId", image_id},
                {"revision", revision},
                {"partitionIndex", partition_index},
                {"volumeName", volume_name},
                {"canDelete", crossing_count == 0U},
                {"crossingRelationshipCount", crossing_count},
                {"blockers", std::move(blockers)}};
}

Json scope_json(const axk::app::detail::PlacementRepairScope &scope) {
    if (scope.volume_name) {
        return {{"kind", "VOLUME"}, {"partitionIndex", scope.partition_index}, {"volumeName", *scope.volume_name}};
    }
    return {{"kind", "PARTITION"}, {"partitionIndex", scope.partition_index}};
}

Json inspection_json(std::string_view image_id, std::uint64_t revision,
                     const axk::app::detail::PlacementRepairPlan &plan) {
    Json destinations = Json::array();
    std::optional<std::string> recovery_volume_name;
    for (const auto &destination : plan.destinations) {
        destinations.push_back({{"volumeName", destination.volume_name},
                                {"createsVolume", destination.creates_volume},
                                {"objectCount", destination.object_sfs_ids.size()},
                                {"objectTypeCounts", destination.object_type_counts}});
        if (destination.creates_volume)
            recovery_volume_name = destination.volume_name;
    }
    Json blockers = Json::array();
    for (const auto &blocker : plan.blockers) {
        blockers.push_back({{"code", blocker.code}, {"message", blocker.message}, {"count", blocker.object_count}});
    }
    Json result{{"imageId", image_id},
                {"revision", revision},
                {"scope", scope_json(plan.scope)},
                {"canRepair", plan.can_repair()},
                {"repairObjectCount", plan.repair_object_count()},
                {"blockedObjectCount", plan.blocked_object_count},
                {"destinations", std::move(destinations)},
                {"blockers", std::move(blockers)}};
    if (recovery_volume_name)
        result["recoveryVolumeName"] = *recovery_volume_name;
    return result;
}

Json repair_manifest(const axk::app::detail::PlacementRepairPlan &plan) {
    Json operations = Json::array();
    for (const auto &destination : plan.destinations) {
        if (!destination.creates_volume)
            continue;
        operations.push_back({{"id", "create-recovery-volume"},
                              {"type", "insert_volume"},
                              {"partition_index", plan.scope.partition_index},
                              {"volume",
                               {{"name", destination.volume_name},
                                {"waveforms", Json::array()},
                                {"samples", Json::array()},
                                {"sample_banks", Json::array()},
                                {"programs", Json::array()}}}});
    }
    std::size_t repair_index{};
    for (const auto &destination : plan.destinations) {
        Json ids = Json::array();
        for (const auto id : destination.object_sfs_ids)
            ids.push_back(id.value);
        operations.push_back({{"id", std::format("repair-object-placements-{}", ++repair_index)},
                              {"type", "repair_object_placements"},
                              {"partition_index", plan.scope.partition_index},
                              {"volume_name", destination.volume_name},
                              {"object_sfs_ids", std::move(ids)}});
    }
    return {{"schema_version", axk::alteration_manifest_schema_version}, {"operations", std::move(operations)}};
}

} // namespace

axk::app::Result<void> axk::app::bind_session_placement_operations(OperationRegistry &registry,
                                                                   ImageSessionManager &images,
                                                                   OperationRegistry::Handler alter_session) {
    if (!registry.is_implemented("images.volume_deletion.inspect")) {
        auto bound = registry.bind("images.volume_deletion.inspect",
                                   [&images](const Json &input, const OperationContext &context) -> Result<Json> {
                                       auto request = parse_volume_deletion_request(input);
                                       if (!request)
                                           return std::unexpected(request.error());
                                       const auto &[image_id, revision, partition, volume] = *request;
                                       return inspect_volume_deletion(images, context.owner_id, image_id, revision,
                                                                      partition, volume);
                                   });
        if (!bound)
            return bound;
    }
    if (!registry.is_implemented("images.placement.inspect")) {
        auto bound = registry.bind(
            "images.placement.inspect", [&images](const Json &input, const OperationContext &context) -> Result<Json> {
                auto request = parse_placement_request(input);
                if (!request)
                    return std::unexpected(request.error());
                auto session = images.begin_read(request->image_id, context.owner_id, request->revision);
                if (!session)
                    return std::unexpected(session.error());
                auto plan = detail::plan_placement_repair(*session, request->scope, request->recovery_volume_name);
                if (!plan)
                    return std::unexpected(plan.error());
                return inspection_json(request->image_id, request->revision, *plan);
            });
        if (!bound)
            return bound;
    }
    if (!registry.is_implemented("images.placement.repair")) {
        auto bound = registry.bind(
            "images.placement.repair",
            [&images, alter_session = std::move(alter_session)](const Json &input,
                                                                const OperationContext &context) -> Result<Json> {
                auto request = parse_placement_request(input);
                if (!request)
                    return std::unexpected(request.error());
                auto plan = [&]() -> Result<detail::PlacementRepairPlan> {
                    auto session = images.begin_read(request->image_id, context.owner_id, request->revision);
                    if (!session)
                        return std::unexpected(session.error());
                    return detail::plan_placement_repair(*session, request->scope, request->recovery_volume_name);
                }();
                if (!plan)
                    return std::unexpected(plan.error());
                if (!plan->can_repair()) {
                    return std::unexpected(operation_error("placement_repair_unavailable",
                                                           "the selected scope has no safely repairable objects"));
                }
                auto altered = alter_session({{"imageId", request->image_id},
                                              {"expectedRevision", request->revision},
                                              {"manifest", {{"inline", repair_manifest(*plan)}}},
                                              {"inputBindings", Json::array()}},
                                             context);
                if (!altered)
                    return std::unexpected(altered.error());
                return altered;
            });
        if (!bound)
            return bound;
    }
    return {};
}
