#include "axklib/deletion.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <tuple>
#include <utility>

#include "axklib/bytes.hpp"
#include "axklib/semantic.hpp"

namespace {

axk::Error deletion_error(std::string message) {
    return axk::make_error(axk::ErrorCode::transaction_rejected, axk::ErrorCategory::transaction, std::move(message));
}

const axk::ObjectSnapshot *find_object(const axk::ObjectCatalog &catalog, std::string_view key) {
    const auto found = std::ranges::find(catalog.objects, key, &axk::ObjectSnapshot::key);
    return found == catalog.objects.end() ? nullptr : &*found;
}

bool supported_type(axk::ObjectType type) {
    return type == axk::ObjectType::sbac || type == axk::ObjectType::sbnk || type == axk::ObjectType::smpl;
}

std::string_view object_type_label(axk::ObjectType type) {
    switch (type) {
    case axk::ObjectType::sbac:
        return "Sample Bank";
    case axk::ObjectType::sbnk:
        return "Sample";
    case axk::ObjectType::smpl:
        return "Wave Data";
    default:
        return "Object";
    }
}

bool member_relationship(std::string_view type) {
    return type == "SBNK_LEFT_MEMBER_TO_SMPL" || type == "SBNK_RIGHT_MEMBER_TO_SMPL";
}

bool candidate_contains(const axk::Relationship &relationship, std::string_view key) {
    return std::ranges::contains(relationship.candidate_keys, key);
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
    result.object_name = object.object.header.name;
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

bool is_selected(const std::set<std::string> &selected, std::string_view key) {
    return selected.contains(std::string{key});
}

} // namespace

std::string_view axk::object_deletion_role_name(ObjectDeletionRole role) noexcept {
    switch (role) {
    case ObjectDeletionRole::target:
        return "TARGET";
    case ObjectDeletionRole::dependency:
        return "DEPENDENCY";
    }
    return "DEPENDENCY";
}

std::string_view axk::object_deletion_status_name(ObjectDeletionStatus status) noexcept {
    switch (status) {
    case ObjectDeletionStatus::required:
        return "REQUIRED";
    case ObjectDeletionStatus::optional:
        return "OPTIONAL";
    case ObjectDeletionStatus::preserved:
        return "PRESERVED";
    case ObjectDeletionStatus::blocked:
        return "BLOCKED";
    }
    return "BLOCKED";
}

std::string_view axk::object_deletion_reference_effect_name(ObjectDeletionReferenceEffect effect) noexcept {
    switch (effect) {
    case ObjectDeletionReferenceEffect::blocking:
        return "BLOCKING";
    case ObjectDeletionReferenceEffect::removed:
        return "REMOVED";
    case ObjectDeletionReferenceEffect::preserved:
        return "PRESERVED";
    }
    return "PRESERVED";
}

axk::Result<axk::ObjectDeletionInspection> axk::inspect_object_deletion(const Container &container,
                                                                        const ObjectCatalog &catalog,
                                                                        const RelationshipGraph &graph,
                                                                        const ObjectDeletionSelection &selection) {
    const auto *target = find_object(catalog, selection.target_key);
    if (target == nullptr)
        return std::unexpected(deletion_error("deletion target does not exist"));
    if (!supported_type(target->object.header.type))
        return std::unexpected(deletion_error("only Sample Bank, Sample, and Wave Data objects can be deleted"));
    if (target->object.format != ObjectFormat::current || !target->placement)
        return std::unexpected(deletion_error("deletion target requires current format and exact placement"));

    std::set<std::string> requested_dependencies;
    for (const auto &key : selection.included_dependency_keys) {
        if (key == target->key || !requested_dependencies.insert(key).second)
            return std::unexpected(
                deletion_error("included deletion dependencies must be unique and exclude the target"));
    }

    ObjectDeletionInspection result;
    result.target_key = target->key;
    result.manifest.schema_version = std::string{alteration_manifest_schema_version};
    result.impacts.push_back(
        make_impact(container, *target, ObjectDeletionRole::target, ObjectDeletionStatus::required, "Selected object"));

    const auto same_scope = [&](const ObjectSnapshot &object) {
        return object.scope_key == target->scope_key && object.placement &&
               object.placement->volume_name == target->placement->volume_name && object.partition == target->partition;
    };
    for (const auto &issue : catalog.issues) {
        if (issue.partition == target->partition) {
            add_notice(result.blockers, "SOURCE_INCONSISTENT",
                       "The source partition contains unresolved object metadata", {target->key});
        }
    }
    const auto partition = std::ranges::find(container.partitions(), target->partition, &Partition::index);
    if (partition == container.partitions().end()) {
        add_notice(result.blockers, "PARTITION_MISSING", "The target partition is not available", {target->key});
    } else if (partition->allocation.invalid_extent_record_count != 0U ||
               partition->allocation.extent_total_mismatch_count != 0U ||
               partition->allocation.conflicting_cluster_count != 0U ||
               !partition->allocation.stored_not_reconstructed.empty() ||
               !partition->allocation.reconstructed_not_stored.empty()) {
        add_notice(result.blockers, "ALLOCATION_INCONSISTENT",
                   "The target partition has unresolved allocation diagnostics", {target->key});
    }

    std::map<std::string, std::set<std::string>> wave_prerequisites;
    const auto add_dependency = [&](const ObjectSnapshot &object, ObjectDeletionStatus status,
                                    std::string reason) -> ObjectDeletionImpact & {
        const auto found = std::ranges::find(result.impacts, object.key, &ObjectDeletionImpact::object_key);
        if (found != result.impacts.end())
            return *found;
        result.impacts.push_back(
            make_impact(container, object, ObjectDeletionRole::dependency, status, std::move(reason)));
        return result.impacts.back();
    };

    const auto incoming_blocks_target = [&](const Relationship &relationship) {
        if (target->object.header.type == ObjectType::sbac)
            return relationship.type == "PROG_ASSIGNMENT_TO_SBAC";
        if (target->object.header.type == ObjectType::sbnk)
            return relationship.type == "SBAC_SLOT_TO_SBNK" || relationship.type == "PROG_ASSIGNMENT_TO_SBNK";
        return member_relationship(relationship.type);
    };
    for (const auto &relationship : graph.relationships) {
        const auto targets_target = (relationship.target_key && *relationship.target_key == target->key) ||
                                    candidate_contains(relationship, target->key);
        if (!targets_target || !incoming_blocks_target(relationship))
            continue;
        add_notice(result.blockers, "OBJECT_REFERENCED",
                   std::format("{} is referenced and cannot be deleted directly",
                               object_type_label(target->object.header.type)),
                   {relationship.source_key, target->key});
    }
    if (const auto *sample = std::get_if<CurrentSbnk>(&target->object.payload);
        sample != nullptr && !sample->linked_program_numbers.empty()) {
        add_notice(result.blockers, "OBJECT_REFERENCED", "Sample is referenced by its Program link bitmap",
                   {target->key});
    }

    std::set<std::string> candidate_samples;
    if (target->object.header.type == ObjectType::sbac) {
        const auto *bank = std::get_if<CurrentSbac>(&target->object.payload);
        if (bank == nullptr || bank->active_slot_count > bank->maximum_slot_count) {
            add_notice(result.blockers, "SAMPLE_BANK_UNREADABLE", "Sample Bank membership is unreadable",
                       {target->key});
        }
        for (const auto &relationship : graph.relationships) {
            if (relationship.source_key != target->key || relationship.type != "SBAC_SLOT_TO_SBNK")
                continue;
            if (relationship.quality != RelationshipQuality::known || !relationship.target_key) {
                add_notice(result.blockers, "SAMPLE_BANK_MEMBER_UNRESOLVED",
                           "Sample Bank contains a member that does not resolve exactly", {target->key});
                continue;
            }
            const auto *sample = find_object(catalog, *relationship.target_key);
            if (sample == nullptr || !same_scope(*sample) || sample->object.header.type != ObjectType::sbnk) {
                add_notice(result.blockers, "SAMPLE_BANK_MEMBER_UNRESOLVED",
                           "Sample Bank member is outside its exact volume scope",
                           {target->key, *relationship.target_key});
                continue;
            }
            candidate_samples.insert(sample->key);
        }
        if (bank != nullptr && candidate_samples.size() != bank->slots.size()) {
            add_notice(result.blockers, "SAMPLE_BANK_MEMBER_UNRESOLVED",
                       "Sample Bank membership cannot be represented as a complete exact deletion closure",
                       {target->key});
        }
        for (const auto &sample_key : candidate_samples) {
            const auto *sample = find_object(catalog, sample_key);
            bool shared_bank{};
            bool program_reference{};
            for (const auto &relationship : graph.relationships) {
                if ((!relationship.target_key || *relationship.target_key != sample_key) &&
                    !candidate_contains(relationship, sample_key)) {
                    continue;
                }
                if (relationship.type == "SBAC_SLOT_TO_SBNK" && relationship.source_key != target->key) {
                    shared_bank = true;
                }
                if (relationship.type == "PROG_ASSIGNMENT_TO_SBNK")
                    program_reference = true;
            }
            const auto *decoded = std::get_if<CurrentSbnk>(&sample->object.payload);
            if (shared_bank) {
                add_notice(result.blockers, "SHARED_SAMPLE", "Another Sample Bank shares a member Sample",
                           {target->key, sample_key});
            }
            if (decoded == nullptr || (decoded->sample_flags & 1U) == 0U) {
                add_notice(result.blockers, "MEMBERSHIP_FLAG_MISSING",
                           "Member Sample is missing its Sample Bank membership flag", {target->key, sample_key});
            }
            const auto preserved =
                program_reference || (decoded != nullptr && !decoded->linked_program_numbers.empty());
            add_dependency(*sample, preserved ? ObjectDeletionStatus::preserved : ObjectDeletionStatus::optional,
                           preserved ? "Sample remains because a Program references it"
                                     : "Sample becomes standalone when the Sample Bank is deleted");
        }
    } else if (target->object.header.type == ObjectType::sbnk) {
        candidate_samples.insert(target->key);
    }

    for (const auto &sample_key : candidate_samples) {
        const auto sample_impact = std::ranges::find(result.impacts, sample_key, &ObjectDeletionImpact::object_key);
        const auto sample_can_be_deleted =
            sample_key == target->key ||
            (sample_impact != result.impacts.end() && sample_impact->status == ObjectDeletionStatus::optional);
        for (const auto &relationship : graph.relationships) {
            if (relationship.source_key != sample_key || !member_relationship(relationship.type))
                continue;
            if (relationship.quality != RelationshipQuality::known || !relationship.target_key) {
                add_notice(result.warnings, "WAVE_DATA_OWNERSHIP_UNRESOLVED",
                           "Wave Data cleanup is unavailable because a Sample member is unresolved", {sample_key});
                continue;
            }
            const auto *wave = find_object(catalog, *relationship.target_key);
            if (wave == nullptr || !same_scope(*wave) || wave->object.header.type != ObjectType::smpl) {
                add_notice(result.warnings, "WAVE_DATA_OWNERSHIP_UNRESOLVED",
                           "Wave Data cleanup is unavailable outside the exact volume scope",
                           {sample_key, *relationship.target_key});
                continue;
            }
            wave_prerequisites[wave->key].insert(sample_key);
            add_dependency(*wave,
                           sample_can_be_deleted ? ObjectDeletionStatus::optional : ObjectDeletionStatus::preserved,
                           sample_can_be_deleted ? "Wave Data may be removed after every referencing Sample is removed"
                                                 : "Wave Data remains because its Sample is preserved");
        }
    }

    for (auto &[wave_key, prerequisites] : wave_prerequisites) {
        auto impact = std::ranges::find(result.impacts, wave_key, &ObjectDeletionImpact::object_key);
        if (impact == result.impacts.end())
            continue;
        bool outside_reference{};
        bool unresolved_reference{};
        for (const auto &relationship : graph.relationships) {
            if (!member_relationship(relationship.type))
                continue;
            const auto targets_wave = (relationship.target_key && *relationship.target_key == wave_key) ||
                                      candidate_contains(relationship, wave_key);
            if (!targets_wave)
                continue;
            if (relationship.quality != RelationshipQuality::known || !relationship.target_key) {
                unresolved_reference = true;
            } else if (!prerequisites.contains(relationship.source_key)) {
                outside_reference = true;
            }
        }
        if (outside_reference || unresolved_reference) {
            impact->status = ObjectDeletionStatus::preserved;
            impact->reason = outside_reference ? "Wave Data remains because another Sample references it"
                                               : "Wave Data remains because ownership is unresolved";
        } else {
            impact->prerequisite_keys.assign(prerequisites.begin(), prerequisites.end());
        }
    }

    if (target->object.header.type == ObjectType::smpl) {
        const auto orphan_report = analyze_waveform_orphans(container, catalog, graph);
        const auto row = std::ranges::find(orphan_report.rows, target->key, &WaveformOrphanRow::object_key);
        if (row == orphan_report.rows.end() || row->status != WaveformStatus::known_unreferenced) {
            add_notice(result.blockers, "WAVE_DATA_NOT_UNREFERENCED",
                       "Wave Data can be deleted only when it is confirmed unreferenced", {target->key});
        }
    }

    for (const auto &key : requested_dependencies) {
        const auto impact = std::ranges::find(result.impacts, key, &ObjectDeletionImpact::object_key);
        if (impact == result.impacts.end() || impact->role != ObjectDeletionRole::dependency)
            return std::unexpected(deletion_error("included object is not a dependency of the deletion target"));
        if (impact->status != ObjectDeletionStatus::optional)
            return std::unexpected(deletion_error("included deletion dependency cannot be removed safely"));
    }
    std::set<std::string> selected{target->key};
    selected.insert(requested_dependencies.begin(), requested_dependencies.end());
    for (const auto &impact : result.impacts) {
        if (!is_selected(selected, impact.object_key))
            continue;
        if (std::ranges::any_of(impact.prerequisite_keys,
                                [&](const auto &key) { return !is_selected(selected, key); })) {
            return std::unexpected(deletion_error("included Wave Data requires every referencing Sample dependency"));
        }
    }

    result.impacts.front().status =
        result.blockers.empty() ? ObjectDeletionStatus::required : ObjectDeletionStatus::blocked;
    for (auto &impact : result.impacts)
        impact.selected = is_selected(selected, impact.object_key);
    result.selected_keys.assign(selected.begin(), selected.end());
    std::ranges::sort(result.impacts, {}, [](const auto &impact) {
        const auto order = impact.object_type == ObjectType::sbac ? 0 : impact.object_type == ObjectType::sbnk ? 1 : 2;
        return std::tuple{order, impact.role == ObjectDeletionRole::target ? 0 : 1, impact.object_name,
                          impact.object_key};
    });

    for (const auto &relationship : graph.relationships) {
        const auto source_relevant = std::ranges::any_of(
            result.impacts, [&](const auto &impact) { return impact.object_key == relationship.source_key; });
        const auto target_key = relationship.target_key.value_or(
            relationship.candidate_keys.size() == 1U ? relationship.candidate_keys.front() : "");
        const auto target_relevant = std::ranges::any_of(result.impacts, [&](const auto &impact) {
            return impact.object_key == target_key || candidate_contains(relationship, impact.object_key);
        });
        if (!source_relevant && !target_relevant)
            continue;
        const auto source_selected = is_selected(selected, relationship.source_key);
        const auto target_selected = !target_key.empty() && is_selected(selected, target_key);
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
            !impact.selected && std::ranges::all_of(impact.prerequisite_keys, [&](const auto &key) {
                return is_selected(selected, key);
            })) {
            add_notice(result.warnings, "WAVE_DATA_WILL_BE_UNREFERENCED",
                       "Selected Sample deletion will leave valid unreferenced Wave Data", {impact.object_key});
        }
    }

    result.valid = result.blockers.empty();
    if (!result.valid) {
        result.selected_keys.clear();
        return result;
    }
    std::size_t bank_index{};
    std::size_t sample_index{};
    std::size_t wave_index{};
    for (const auto &impact : result.impacts) {
        if (!impact.selected)
            continue;
        const auto impact_partition = std::ranges::find(container.partitions(), impact.partition, &Partition::index);
        if (impact_partition == container.partitions().end())
            return std::unexpected(deletion_error("deletion impact partition is not available"));
        const auto cluster_size =
            checked_multiply(container.superblock().sector_size_bytes, impact_partition->sectors_per_cluster);
        const auto freed_bytes = cluster_size ? checked_multiply(impact.freed_clusters, *cluster_size)
                                              : Result<std::uint64_t>{std::unexpected{cluster_size.error()}};
        const auto total_bytes = freed_bytes ? checked_add(result.estimated_freed_bytes, *freed_bytes)
                                             : Result<std::uint64_t>{std::unexpected{freed_bytes.error()}};
        const auto total_clusters = checked_add(result.estimated_freed_clusters, impact.freed_clusters);
        if (!total_bytes || !total_clusters)
            return std::unexpected(deletion_error("deletion recovery estimate exceeds supported limits"));
        result.estimated_freed_bytes = *total_bytes;
        result.estimated_freed_clusters = *total_clusters;
        if (impact.object_type == ObjectType::sbac) {
            result.manifest.operations.push_back(
                {std::format("delete-sample-bank-{}", ++bank_index),
                 DeleteSampleBankOperation{impact.partition, impact.volume_name, impact.object_name}});
        } else if (impact.object_type == ObjectType::sbnk) {
            result.manifest.operations.push_back(
                {std::format("delete-sample-{}", ++sample_index),
                 DeleteSampleOperation{impact.partition, impact.volume_name, impact.object_name}});
        } else {
            result.manifest.operations.push_back(
                {std::format("delete-wave-data-{}", ++wave_index),
                 DeleteWaveformOperation{impact.partition, impact.volume_name, impact.object_name}});
        }
    }
    return result;
}
