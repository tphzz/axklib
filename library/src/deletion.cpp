#include "axklib/deletion.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "axklib/bytes.hpp"
#include "axklib/deletion_manifest.hpp"
#include "axklib/semantic.hpp"

namespace {

constexpr std::size_t maximum_deletion_objects = 1024U;

axk::Error deletion_error(std::string message) {
    return axk::make_error(axk::ErrorCode::transaction_rejected, axk::ErrorCategory::transaction, std::move(message));
}

bool supported_type(axk::ObjectType type) {
    return type == axk::ObjectType::prog || type == axk::ObjectType::sbac || type == axk::ObjectType::sbnk ||
           type == axk::ObjectType::smpl || type == axk::ObjectType::sequ;
}

std::string_view object_type_label(axk::ObjectType type) {
    switch (type) {
    case axk::ObjectType::prog:
        return "Program";
    case axk::ObjectType::sbac:
        return "Sample Bank";
    case axk::ObjectType::sbnk:
        return "Sample";
    case axk::ObjectType::smpl:
        return "Wave Data";
    case axk::ObjectType::sequ:
        return "Sequence";
    default:
        return "Object";
    }
}

int object_type_order(axk::ObjectType type) {
    switch (type) {
    case axk::ObjectType::prog:
        return 0;
    case axk::ObjectType::sbac:
        return 1;
    case axk::ObjectType::sbnk:
        return 2;
    case axk::ObjectType::smpl:
        return 3;
    case axk::ObjectType::sequ:
        return 4;
    default:
        return 5;
    }
}

bool member_relationship(std::string_view type) {
    return type == "SBNK_LEFT_MEMBER_TO_SMPL" || type == "SBNK_RIGHT_MEMBER_TO_SMPL";
}

bool relevant_incoming(axk::ObjectType type, std::string_view relationship) {
    if (type == axk::ObjectType::sbac)
        return relationship == "PROG_ASSIGNMENT_TO_SBAC";
    if (type == axk::ObjectType::sbnk)
        return relationship == "SBAC_SLOT_TO_SBNK" || relationship == "PROG_ASSIGNMENT_TO_SBNK";
    if (type == axk::ObjectType::smpl)
        return member_relationship(relationship);
    return false;
}

bool cleanup_relationship(axk::ObjectType source, std::string_view relationship) {
    if (source == axk::ObjectType::prog)
        return relationship == "PROG_ASSIGNMENT_TO_SBAC" || relationship == "PROG_ASSIGNMENT_TO_SBNK";
    if (source == axk::ObjectType::sbac)
        return relationship == "SBAC_SLOT_TO_SBNK";
    if (source == axk::ObjectType::sbnk)
        return member_relationship(relationship);
    return false;
}

std::optional<std::uint8_t> program_number(const axk::ObjectSnapshot &object) {
    unsigned value{};
    const auto &name = object.object.header.name;
    const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), value);
    if (error != std::errc{} || end != name.data() + name.size() || value == 0U || value > 128U)
        return std::nullopt;
    return static_cast<std::uint8_t>(value);
}

std::string display_name(const axk::ObjectSnapshot &object) {
    if (const auto *program = std::get_if<axk::CurrentProg>(&object.object.payload)) {
        return std::format("{}: {}", object.object.header.name, program->program_name);
    }
    return object.object.header.name;
}

std::uint64_t record_cluster_count(const axk::Container &container, const axk::ObjectSnapshot &object) {
    const auto partition = std::ranges::find(container.partitions(), object.partition, &axk::Partition::index);
    if (partition == container.partitions().end())
        return 0U;
    const auto record = std::ranges::find(partition->records, object.sfs_id, &axk::IndexRecord::sfs_id);
    if (record == partition->records.end())
        return 0U;
    std::uint64_t result = record->continuation_clusters.size();
    for (const auto &extent : record->extents)
        result += extent.cluster_count;
    return result;
}

std::uint64_t record_size(const axk::Container &container, const axk::ObjectSnapshot &object) {
    const auto partition = std::ranges::find(container.partitions(), object.partition, &axk::Partition::index);
    if (partition == container.partitions().end())
        return 0U;
    const auto record = std::ranges::find(partition->records, object.sfs_id, &axk::IndexRecord::sfs_id);
    return record == partition->records.end() ? 0U : record->data_size;
}

void add_notice(std::vector<axk::ObjectDeletionNotice> &notices, std::string code, std::string message,
                std::vector<std::string> keys) {
    if (std::ranges::none_of(notices,
                             [&](const auto &notice) { return notice.code == code && notice.object_keys == keys; })) {
        notices.push_back({std::move(code), std::move(message), std::move(keys)});
    }
}

axk::ObjectDeletionImpact make_impact(const axk::Container &container, const axk::ObjectSnapshot &object,
                                      axk::ObjectDeletionRole role, axk::ObjectDeletionStatus status,
                                      std::string reason) {
    axk::ObjectDeletionImpact result;
    result.object_key = object.key;
    result.object_type = object.object.header.type;
    result.object_name = display_name(object);
    result.partition = object.partition;
    if (object.placement) {
        result.partition_name = object.placement->partition_name;
        result.volume_name = object.placement->volume_name;
    }
    result.role = role;
    result.status = status;
    result.stored_size_bytes = record_size(container, object);
    result.freed_clusters = record_cluster_count(container, object);
    result.reason = std::move(reason);
    return result;
}

struct DeletionIndex {
    const axk::Container &container;
    const axk::ObjectCatalog &catalog;
    const axk::RelationshipGraph &graph;
    std::map<std::string, const axk::ObjectSnapshot *> objects;
    std::map<std::string, std::vector<const axk::Relationship *>> incoming;
    std::map<std::string, std::vector<const axk::Relationship *>> outgoing;
    std::set<std::uint8_t> inconsistent_partitions;
    std::map<std::tuple<std::uint8_t, std::string, std::uint8_t>, std::string> programs;
    std::map<std::string, axk::WaveformStatus> waveform_statuses;

    DeletionIndex(const axk::Container &source_container, const axk::ObjectCatalog &source_catalog,
                  const axk::RelationshipGraph &source_graph)
        : container(source_container), catalog(source_catalog), graph(source_graph) {
        for (const auto &object : catalog.objects) {
            objects.emplace(object.key, &object);
            if (object.object.header.type == axk::ObjectType::prog && object.placement) {
                if (const auto number = program_number(object)) {
                    programs.emplace(std::tuple{object.partition.value, object.placement->volume_name, *number},
                                     object.key);
                }
            }
        }
        for (const auto &relationship : graph.relationships) {
            outgoing[relationship.source_key].push_back(&relationship);
            if (relationship.type.starts_with("PROG_ASSIGNMENT_TO_") &&
                !axk::is_effective_program_assignment(relationship)) {
                continue;
            }
            if (relationship.target_key)
                incoming[*relationship.target_key].push_back(&relationship);
            for (const auto &candidate : relationship.candidate_keys) {
                if (!relationship.target_key || candidate != *relationship.target_key)
                    incoming[candidate].push_back(&relationship);
            }
        }
        for (const auto &issue : catalog.issues)
            inconsistent_partitions.insert(issue.partition.value);
        for (const auto &partition : container.partitions()) {
            if (!allocation_is_safe_for_mutation(partition.allocation)) {
                inconsistent_partitions.insert(partition.index.value);
            }
        }
        const auto orphan_report = axk::analyze_waveform_orphans(container, catalog, graph);
        for (const auto &row : orphan_report.rows)
            waveform_statuses.emplace(row.object_key, row.status);
    }

    [[nodiscard]] const axk::ObjectSnapshot *find(std::string_view key) const {
        const auto found = objects.find(std::string{key});
        return found == objects.end() ? nullptr : found->second;
    }

    [[nodiscard]] bool same_scope(const axk::ObjectSnapshot &left, const axk::ObjectSnapshot &right) const {
        return left.scope_key == right.scope_key && left.placement && right.placement &&
               left.placement->volume_name == right.placement->volume_name && left.partition == right.partition;
    }

    [[nodiscard]] std::optional<std::string> program_key(const axk::ObjectSnapshot &sample, std::uint8_t number) const {
        if (!sample.placement)
            return std::nullopt;
        const auto found = programs.find(std::tuple{sample.partition.value, sample.placement->volume_name, number});
        return found == programs.end() ? std::nullopt : std::optional<std::string>{found->second};
    }
};

void evaluate_program(const DeletionIndex &index, const axk::ObjectSnapshot &object,
                      std::vector<axk::ObjectDeletionNotice> &notices) {
    const auto *program = std::get_if<axk::CurrentProg>(&object.object.payload);
    const auto number = program_number(object);
    if (program == nullptr || !number) {
        add_notice(notices, "PROGRAM_UNREADABLE", "Program assignments are unreadable", {object.key});
        return;
    }
    std::set<std::string> assignments;
    std::size_t direct_assignment_count{};
    if (const auto outgoing = index.outgoing.find(object.key); outgoing != index.outgoing.end()) {
        for (const auto *relationship : outgoing->second) {
            if (relationship->type != "PROG_ASSIGNMENT_TO_SBNK" ||
                !axk::is_effective_program_assignment(*relationship)) {
                continue;
            }
            ++direct_assignment_count;
            assignments.insert(*relationship->target_key);
        }
    }
    std::set<std::string> bitmap_samples;
    for (const auto &[key, candidate] : index.objects) {
        if (candidate->object.header.type != axk::ObjectType::sbnk || !index.same_scope(object, *candidate))
            continue;
        const auto *sample = std::get_if<axk::CurrentSbnk>(&candidate->object.payload);
        if (sample != nullptr && std::ranges::contains(sample->linked_program_numbers, *number))
            bitmap_samples.insert(key);
    }
    if (assignments.size() != direct_assignment_count || assignments != bitmap_samples) {
        add_notice(notices, "PROGRAM_LINKS_INCONSISTENT",
                   "Program direct assignments do not match Sample Program links", {object.key});
    }
}

void evaluate_sample_bank(const DeletionIndex &index, const axk::ObjectSnapshot &object,
                          std::vector<axk::ObjectDeletionNotice> &notices) {
    const auto *bank = std::get_if<axk::CurrentSbac>(&object.object.payload);
    if (bank == nullptr || bank->active_slot_count > bank->maximum_slot_count) {
        add_notice(notices, "SAMPLE_BANK_UNREADABLE", "Sample Bank membership is unreadable", {object.key});
        return;
    }
    std::set<std::string> members;
    if (const auto outgoing = index.outgoing.find(object.key); outgoing != index.outgoing.end()) {
        for (const auto *relationship : outgoing->second) {
            if (relationship->type != "SBAC_SLOT_TO_SBNK")
                continue;
            if (relationship->quality != axk::RelationshipQuality::known || !relationship->target_key) {
                add_notice(notices, "SAMPLE_BANK_MEMBER_UNRESOLVED",
                           "Sample Bank contains a member that does not resolve exactly", {object.key});
                continue;
            }
            const auto *sample = index.find(*relationship->target_key);
            if (sample == nullptr || sample->object.header.type != axk::ObjectType::sbnk ||
                !index.same_scope(object, *sample)) {
                add_notice(notices, "SAMPLE_BANK_MEMBER_UNRESOLVED",
                           "Sample Bank member is outside its exact volume scope",
                           {object.key, *relationship->target_key});
                continue;
            }
            members.insert(sample->key);
        }
    }
    if (members.size() != bank->slots.size()) {
        add_notice(notices, "SAMPLE_BANK_MEMBER_UNRESOLVED",
                   "Sample Bank membership cannot be represented as a complete exact deletion closure", {object.key});
    }
    for (const auto &member : members) {
        const auto *sample = index.find(member);
        const auto *decoded = sample == nullptr ? nullptr : std::get_if<axk::CurrentSbnk>(&sample->object.payload);
        if (decoded == nullptr || (decoded->sample_flags & 1U) == 0U) {
            add_notice(notices, "MEMBERSHIP_FLAG_MISSING", "Member Sample is missing its Sample Bank membership flag",
                       {object.key, member});
        }
        if (const auto incoming = index.incoming.find(member); incoming != index.incoming.end()) {
            for (const auto *relationship : incoming->second) {
                if (relationship->type == "SBAC_SLOT_TO_SBNK" && relationship->source_key != object.key) {
                    add_notice(notices, "SHARED_SAMPLE", "Another Sample Bank shares a member Sample",
                               {object.key, member});
                }
            }
        }
    }
}

std::vector<axk::ObjectDeletionNotice> evaluate_object(const DeletionIndex &index, const axk::ObjectSnapshot &object,
                                                       const std::set<std::string> &deleting) {
    std::vector<axk::ObjectDeletionNotice> notices;
    if (object.object.format != axk::ObjectFormat::current || !object.placement) {
        add_notice(
            notices, "OBJECT_PLACEMENT_UNRESOLVED",
            std::format("{} requires current format and exact placement", object_type_label(object.object.header.type)),
            {object.key});
        return notices;
    }
    if (index.inconsistent_partitions.contains(object.partition.value)) {
        add_notice(notices, "SOURCE_INCONSISTENT",
                   "The source partition has unresolved object or allocation diagnostics", {object.key});
    }
    if (object.object.header.type == axk::ObjectType::prog)
        evaluate_program(index, object, notices);
    else if (object.object.header.type == axk::ObjectType::sbac)
        evaluate_sample_bank(index, object, notices);

    bool has_wave_reference{};
    if (const auto incoming = index.incoming.find(object.key); incoming != index.incoming.end()) {
        for (const auto *relationship : incoming->second) {
            if (!relevant_incoming(object.object.header.type, relationship->type))
                continue;
            has_wave_reference = has_wave_reference || object.object.header.type == axk::ObjectType::smpl;
            const auto exact = relationship->quality == axk::RelationshipQuality::known && relationship->target_key &&
                               *relationship->target_key == object.key;
            if (deleting.contains(relationship->source_key))
                continue;
            add_notice(notices, exact ? "OBJECT_REFERENCED" : "RELATIONSHIP_UNRESOLVED",
                       exact ? std::format("{} is referenced by an object that will remain",
                                           object_type_label(object.object.header.type))
                             : std::format("{} ownership is unresolved", object_type_label(object.object.header.type)),
                       {relationship->source_key, object.key});
        }
    }
    if (const auto *sample = std::get_if<axk::CurrentSbnk>(&object.object.payload)) {
        for (const auto number : sample->linked_program_numbers) {
            const auto key = index.program_key(object, number);
            if (!key || !deleting.contains(*key)) {
                add_notice(notices, "OBJECT_REFERENCED", "Sample is referenced by a Program link",
                           key ? std::vector<std::string>{*key, object.key} : std::vector<std::string>{object.key});
            }
        }
    }
    if (object.object.header.type == axk::ObjectType::smpl && !has_wave_reference) {
        const auto row = index.waveform_statuses.find(object.key);
        if (row == index.waveform_statuses.end() || row->second != axk::WaveformStatus::known_unreferenced) {
            add_notice(notices, "WAVE_DATA_NOT_UNREFERENCED",
                       "Wave Data can be deleted only when it is confirmed unreferenced", {object.key});
        }
    }
    return notices;
}

std::set<std::string> stable_deletion_set(const DeletionIndex &index, std::set<std::string> candidates,
                                          const std::set<std::string> &protected_keys = {}) {
    bool changed = true;
    while (changed) {
        changed = false;
        std::vector<std::string> rejected;
        for (const auto &key : candidates) {
            if (protected_keys.contains(key))
                continue;
            const auto *object = index.find(key);
            if (object == nullptr || !evaluate_object(index, *object, candidates).empty())
                rejected.push_back(key);
        }
        for (const auto &key : rejected) {
            changed = candidates.erase(key) != 0U || changed;
        }
    }
    return candidates;
}

std::set<std::string> reachable_cleanup(const DeletionIndex &index, const std::set<std::string> &sources,
                                        const std::set<std::string> &excluded) {
    std::set<std::string> result;
    std::vector<std::string> pending(sources.begin(), sources.end());
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const auto *source = index.find(pending[cursor]);
        if (source == nullptr)
            continue;
        const auto outgoing = index.outgoing.find(source->key);
        if (outgoing == index.outgoing.end())
            continue;
        for (const auto *relationship : outgoing->second) {
            if (!cleanup_relationship(source->object.header.type, relationship->type) ||
                relationship->quality != axk::RelationshipQuality::known || !relationship->target_key) {
                continue;
            }
            const auto *target = index.find(*relationship->target_key);
            if (target == nullptr || !supported_type(target->object.header.type) || excluded.contains(target->key) ||
                !index.same_scope(*source, *target)) {
                continue;
            }
            if (result.insert(target->key).second)
                pending.push_back(target->key);
        }
    }
    return result;
}

std::vector<std::string> prerequisites(const DeletionIndex &index, const axk::ObjectSnapshot &object,
                                       const std::set<std::string> &potential) {
    std::set<std::string> result;
    if (const auto incoming = index.incoming.find(object.key); incoming != index.incoming.end()) {
        for (const auto *relationship : incoming->second) {
            if (relevant_incoming(object.object.header.type, relationship->type) &&
                potential.contains(relationship->source_key)) {
                result.insert(relationship->source_key);
            }
        }
    }
    if (const auto *sample = std::get_if<axk::CurrentSbnk>(&object.object.payload)) {
        for (const auto number : sample->linked_program_numbers) {
            if (const auto key = index.program_key(object, number); key && potential.contains(*key))
                result.insert(*key);
        }
    }
    return {result.begin(), result.end()};
}

std::set<std::string> direct_referrers(const DeletionIndex &index, const axk::ObjectSnapshot &object) {
    std::set<std::string> result;
    if (const auto incoming = index.incoming.find(object.key); incoming != index.incoming.end()) {
        for (const auto *relationship : incoming->second) {
            if (relevant_incoming(object.object.header.type, relationship->type))
                result.insert(relationship->source_key);
        }
    }
    if (const auto *sample = std::get_if<axk::CurrentSbnk>(&object.object.payload)) {
        for (const auto number : sample->linked_program_numbers) {
            if (const auto key = index.program_key(object, number))
                result.insert(*key);
        }
    }
    return result;
}

std::set<std::string> reachable_referrers(const DeletionIndex &index, const std::set<std::string> &targets) {
    std::set<std::string> result;
    std::vector<std::string> pending(targets.begin(), targets.end());
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const auto *object = index.find(pending[cursor]);
        if (object == nullptr)
            continue;
        for (const auto &key : direct_referrers(index, *object)) {
            const auto *referrer = index.find(key);
            if (referrer == nullptr || !supported_type(referrer->object.header.type) || targets.contains(key) ||
                !index.same_scope(*object, *referrer)) {
                continue;
            }
            if (result.insert(key).second)
                pending.push_back(key);
        }
    }
    return result;
}

std::set<std::string> contributing_referrers(const DeletionIndex &index, const std::set<std::string> &eligible_targets,
                                             const std::set<std::string> &available) {
    std::set<std::string> result;
    std::vector<std::string> pending(eligible_targets.begin(), eligible_targets.end());
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const auto *object = index.find(pending[cursor]);
        if (object == nullptr)
            continue;
        for (const auto &key : direct_referrers(index, *object)) {
            if (!available.contains(key))
                continue;
            if (result.insert(key).second)
                pending.push_back(key);
        }
    }
    return result;
}

} // namespace

axk::Result<axk::ObjectDeletionInspection> axk::inspect_object_deletion(const Container &container,
                                                                        const ObjectCatalog &catalog,
                                                                        const RelationshipGraph &graph,
                                                                        const ObjectDeletionSelection &selection) {
    if (selection.target_keys.empty())
        return std::unexpected(deletion_error("deletion requires at least one target"));
    if (selection.target_keys.size() + selection.referrer_keys.size() + selection.cleanup_keys.size() >
        maximum_deletion_objects)
        return std::unexpected(deletion_error("deletion selection exceeds the 1024 object limit"));

    const DeletionIndex index{container, catalog, graph};
    std::set<std::string> requested_targets;
    for (const auto &key : selection.target_keys) {
        const auto *object = index.find(key);
        if (object == nullptr)
            return std::unexpected(deletion_error("deletion target does not exist"));
        if (!supported_type(object->object.header.type))
            return std::unexpected(
                deletion_error("only Program, Sample Bank, Sample, Wave Data, and Sequence objects can be deleted"));
        if (!requested_targets.insert(key).second)
            return std::unexpected(deletion_error("deletion targets must be unique"));
    }
    std::set<std::string> requested_cleanup;
    std::set<std::string> requested_referrers;
    for (const auto &key : selection.referrer_keys) {
        if (requested_targets.contains(key) || !requested_referrers.insert(key).second)
            return std::unexpected(deletion_error("referrer objects must be unique and exclude deletion targets"));
        if (index.find(key) == nullptr)
            return std::unexpected(deletion_error("referrer object does not exist"));
    }
    for (const auto &key : selection.cleanup_keys) {
        if (requested_targets.contains(key) || requested_referrers.contains(key) ||
            !requested_cleanup.insert(key).second)
            return std::unexpected(
                deletion_error("cleanup objects must be unique and exclude deletion targets and referrers"));
        if (index.find(key) == nullptr)
            return std::unexpected(deletion_error("cleanup object does not exist"));
    }

    ObjectDeletionInspection result;
    result.target_keys.assign(requested_targets.begin(), requested_targets.end());

    const auto upstream = reachable_referrers(index, requested_targets);
    for (const auto &key : requested_referrers) {
        if (!upstream.contains(key))
            return std::unexpected(deletion_error("referrer object is not in the target reference closure"));
    }

    auto complete_candidates = requested_targets;
    complete_candidates.insert(upstream.begin(), upstream.end());
    const auto complete_stable = stable_deletion_set(index, complete_candidates);
    std::set<std::string> completely_eligible_targets;
    std::ranges::copy_if(requested_targets,
                         std::inserter(completely_eligible_targets, completely_eligible_targets.end()),
                         [&](const auto &key) { return complete_stable.contains(key); });
    const auto optional_referrers = contributing_referrers(index, completely_eligible_targets, complete_stable);

    auto explicit_candidates = requested_targets;
    explicit_candidates.insert(requested_referrers.begin(), requested_referrers.end());
    const auto explicit_stable = stable_deletion_set(index, explicit_candidates);
    std::set<std::string> eligible_targets;
    std::ranges::copy_if(requested_targets, std::inserter(eligible_targets, eligible_targets.end()),
                         [&](const auto &key) { return explicit_stable.contains(key); });
    const auto selected_referrers = contributing_referrers(index, eligible_targets, explicit_stable);
    auto selected = eligible_targets;
    selected.insert(selected_referrers.begin(), selected_referrers.end());

    for (const auto &key : requested_targets) {
        const auto *object = index.find(key);
        const auto eligible = eligible_targets.contains(key);
        const auto resolvable = completely_eligible_targets.contains(key);
        auto impact = make_impact(container, *object, ObjectDeletionRole::target,
                                  eligible ? ObjectDeletionStatus::required : ObjectDeletionStatus::blocked,
                                  eligible     ? "Selected object"
                                  : resolvable ? "References must be resolved"
                                               : "Deletion constraints are not satisfied");
        impact.requested = true;
        result.impacts.push_back(std::move(impact));
        if (!eligible) {
            auto blockers = evaluate_object(index, *object, explicit_stable);
            if (blockers.empty()) {
                add_notice(blockers, "DEPENDENCY_BLOCKED", "A selected referencing object cannot be deleted safely",
                           {key});
            }
            result.blockers.insert(result.blockers.end(), std::make_move_iterator(blockers.begin()),
                                   std::make_move_iterator(blockers.end()));
        }
    }

    for (const auto &key : upstream) {
        const auto *object = index.find(key);
        const auto optional = optional_referrers.contains(key);
        auto impact = make_impact(container, *object, ObjectDeletionRole::referrer,
                                  optional ? ObjectDeletionStatus::optional : ObjectDeletionStatus::preserved,
                                  optional ? std::format("Delete this {} to resolve an incoming reference",
                                                         object_type_label(object->object.header.type))
                                           : std::format("This {} cannot safely resolve the selected target",
                                                         object_type_label(object->object.header.type)));
        impact.requested = requested_referrers.contains(key);
        if (optional)
            impact.prerequisite_keys = prerequisites(index, *object, complete_stable);
        result.impacts.push_back(std::move(impact));
    }

    auto cleanup_sources = requested_targets;
    cleanup_sources.insert(selected_referrers.begin(), selected_referrers.end());
    auto cleanup_excluded = requested_targets;
    cleanup_excluded.insert(upstream.begin(), upstream.end());
    const auto reachable = reachable_cleanup(index, cleanup_sources, cleanup_excluded);
    auto potential = completely_eligible_targets;
    potential.insert(optional_referrers.begin(), optional_referrers.end());
    const auto protected_keys = potential;
    potential.insert(reachable.begin(), reachable.end());
    potential = stable_deletion_set(index, std::move(potential), protected_keys);
    const std::set<std::string> optional_keys = [&] {
        auto value = potential;
        for (const auto &key : protected_keys)
            value.erase(key);
        return value;
    }();
    for (const auto &key : reachable) {
        const auto *object = index.find(key);
        const auto optional = optional_keys.contains(key);
        auto impact = make_impact(container, *object, ObjectDeletionRole::dependency,
                                  optional ? ObjectDeletionStatus::optional : ObjectDeletionStatus::preserved,
                                  optional ? std::format("{} becomes unreferenced when its parents are deleted",
                                                         object_type_label(object->object.header.type))
                                           : std::format("{} remains because another object refers to it",
                                                         object_type_label(object->object.header.type)));
        if (optional)
            impact.prerequisite_keys = prerequisites(index, *object, potential);
        impact.requested = requested_cleanup.contains(key);
        result.impacts.push_back(std::move(impact));
    }

    for (const auto &key : requested_cleanup) {
        if (!optional_keys.contains(key))
            return std::unexpected(deletion_error("cleanup object is not an optional dependency of this deletion"));
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &key : requested_cleanup) {
            if (selected.contains(key))
                continue;
            const auto impact = std::ranges::find(result.impacts, key, &ObjectDeletionImpact::object_key);
            if (impact != result.impacts.end() &&
                std::ranges::all_of(impact->prerequisite_keys,
                                    [&](const auto &prerequisite) { return selected.contains(prerequisite); })) {
                selected.insert(key);
                changed = true;
            }
        }
    }
    for (auto &impact : result.impacts)
        impact.selected = selected.contains(impact.object_key);
    result.selected_keys.assign(selected.begin(), selected.end());
    result.can_apply = !eligible_targets.empty();

    std::ranges::sort(result.impacts, {}, [](const auto &impact) {
        return std::tuple{impact.status == ObjectDeletionStatus::blocked ? 1 : 0,
                          impact.role == ObjectDeletionRole::target     ? 0
                          : impact.role == ObjectDeletionRole::referrer ? 1
                                                                        : 2,
                          impact.partition.value,
                          impact.volume_name,
                          object_type_order(impact.object_type),
                          impact.object_name,
                          impact.object_key};
    });

    std::set<std::string> relevant_keys;
    for (const auto &impact : result.impacts)
        relevant_keys.insert(impact.object_key);
    for (const auto &relationship : graph.relationships) {
        const auto target_key = relationship.target_key.value_or(
            relationship.candidate_keys.size() == 1U ? relationship.candidate_keys.front() : "");
        if (!relevant_keys.contains(relationship.source_key) &&
            (target_key.empty() || !relevant_keys.contains(target_key))) {
            continue;
        }
        const auto source_selected = selected.contains(relationship.source_key);
        const auto target_selected = !target_key.empty() && selected.contains(target_key);
        const auto effect = source_selected   ? ObjectDeletionReferenceEffect::removed
                            : target_selected ? ObjectDeletionReferenceEffect::blocking
                                              : ObjectDeletionReferenceEffect::preserved;
        result.references.push_back(
            {relationship.source_key, target_key, relationship.type, relationship.quality, effect});
    }
    std::ranges::sort(result.references, {}, [](const auto &reference) {
        return std::tuple{reference.source_key, reference.type, reference.target_key};
    });

    for (const auto &impact : result.impacts) {
        if (impact.object_type == ObjectType::smpl && impact.status == ObjectDeletionStatus::optional &&
            !impact.selected &&
            std::ranges::all_of(impact.prerequisite_keys, [&](const auto &key) { return selected.contains(key); })) {
            add_notice(result.warnings, "WAVE_DATA_WILL_BE_UNREFERENCED",
                       "Selected Sample deletion will leave valid unreferenced Wave Data", {impact.object_key});
        }
    }

    const auto manifest = detail::build_object_deletion_manifest(container, catalog, result.impacts);
    if (!manifest)
        return std::unexpected(manifest.error());
    result.manifest = manifest->manifest;
    result.estimated_freed_bytes = manifest->estimated_freed_bytes;
    result.estimated_freed_clusters = manifest->estimated_freed_clusters;
    return result;
}
