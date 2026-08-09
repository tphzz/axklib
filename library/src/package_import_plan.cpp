#include "package_import_internal.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <tuple>

#include "axklib/package_archive.hpp"

#include "package_import_program_slots.hpp"

namespace axk {
namespace {

Error planner_error(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
}

std::string digest_text(std::string_view value) {
    return package_internal::hex_digest(package_internal::sha256(std::as_bytes(std::span{value})));
}

void append_field(std::string &target, std::string_view value) { target += std::format("{}:{};", value.size(), value); }

template <typename Integer> void append_integer(std::string &target, Integer value) {
    append_field(target, std::to_string(value));
}

bool valid_digest(std::string_view value) {
    return value.size() == 64U && std::ranges::all_of(value, [](unsigned char byte) {
               return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
           });
}

bool valid_slot_ranges(std::span<const PackageProgramSlotRange> ranges) {
    std::uint16_t previous_last{};
    for (const auto &range : ranges) {
        if (range.first < 1U || range.last < range.first || range.last > 128U || range.first <= previous_last)
            return false;
        previous_last = range.last;
    }
    return true;
}

bool slot_in_ranges(std::uint8_t slot, std::span<const PackageProgramSlotRange> ranges) {
    return std::ranges::any_of(ranges, [slot](const auto &range) { return slot >= range.first && slot <= range.last; });
}

std::uint16_t slot_range_cardinality(std::span<const PackageProgramSlotRange> ranges) {
    std::uint32_t result{};
    for (const auto &range : ranges)
        result += static_cast<std::uint32_t>(range.last) - static_cast<std::uint32_t>(range.first) + 1U;
    return static_cast<std::uint16_t>(result);
}

bool valid_sfs_index_capacity(const PackageImportPlan &plan) {
    if (plan.target_kind != MediaKind::sfs)
        return plan.sfs_index_capacity.empty();
    std::set<std::uint8_t> partitions;
    for (const auto &capacity : plan.sfs_index_capacity) {
        std::set<std::size_t> package_indices;
        std::uint64_t required{};
        std::uint64_t allocated{};
        std::uint64_t shortfall{};
        for (const auto &usage : capacity.packages) {
            if (usage.package_index >= plan.package_ids.size() ||
                !package_indices.emplace(usage.package_index).second ||
                usage.standalone_required_record_slots !=
                    usage.effective_object_record_slots + usage.volume_scaffolding_record_slots ||
                usage.planned_record_slots !=
                    usage.planned_object_record_slots + usage.volume_scaffolding_record_slots ||
                usage.planned_record_slots != usage.allocated_record_slots + usage.shortfall_record_slots ||
                usage.planned_object_record_slots + usage.reused_object_count > usage.effective_object_record_slots) {
                return false;
            }
            required += usage.planned_record_slots;
            allocated += usage.allocated_record_slots;
            shortfall += usage.shortfall_record_slots;
        }
        if (!partitions.emplace(capacity.partition_index).second || capacity.records_per_index_block != 14U ||
            capacity.total_record_slots != capacity.index_block_count * capacity.records_per_index_block ||
            capacity.reserved_record_slots > capacity.total_record_slots ||
            capacity.allocatable_record_slots != capacity.total_record_slots - capacity.reserved_record_slots ||
            capacity.used_record_slots + capacity.free_record_slots != capacity.allocatable_record_slots ||
            required != capacity.required_record_slots || allocated != capacity.allocated_record_slots ||
            shortfall != capacity.shortfall_record_slots || allocated > capacity.free_record_slots ||
            capacity.remaining_record_slots != capacity.free_record_slots - allocated ||
            capacity.required_record_slots != capacity.allocated_record_slots + capacity.shortfall_record_slots) {
            return false;
        }
    }
    return true;
}

} // namespace

namespace package_import_internal {

std::string plan_identity(const PackageImportPlan &plan) {
    std::string source;
    append_field(source, plan.schema_version);
    append_integer(source, static_cast<std::uint8_t>(plan.target_kind));
    append_field(source, plan.target_snapshot_id);
    append_field(source, plan.policy_digest);
    for (const auto &package_id : plan.package_ids)
        append_field(source, package_id);
    for (const auto &destination : plan.destinations) {
        append_integer(source, destination.partition_index);
        append_field(source, destination.group_name);
        append_field(source, destination.volume_name);
        append_field(source, destination.raw_group);
        append_field(source, destination.raw_volume);
        append_integer(source, destination.create);
        for (const auto id : destination.infrastructure_sfs_ids)
            append_integer(source, id);
        append_integer(source, destination.infrastructure_clusters);
        append_integer(source, destination.root_directory_growth_bytes);
    }
    for (const auto &object : plan.objects) {
        append_field(source, object.action_id);
        append_integer(source, object.package_index);
        append_integer(source, object.root_index);
        append_field(source, object.package_id);
        append_field(source, object.node_id);
        append_field(source, object.object_type);
        append_field(source, object.source_name);
        append_field(source, object.destination_name);
        append_field(source, object.normalized_sha256);
        append_integer(source, object.partition_index);
        append_field(source, object.group_name);
        append_field(source, object.volume_name);
        append_field(source, object.raw_group);
        append_field(source, object.raw_volume);
        for (const auto action : object.actions)
            append_field(source, package_import_action_name(action));
        append_field(source, object.canonical_action_id.value_or(""));
        append_field(source, object.existing_object_key.value_or(""));
        append_integer(source, object.target_sfs_id.value_or(0U));
        append_integer(source, object.target_wave_data_reference_value.value_or(0U));
        for (const auto number : object.target_program_numbers)
            append_integer(source, number);
        append_integer(source, object.target_sample_bank_member);
        append_integer(source, object.payload_clusters);
        append_integer(source, object.payload_sectors);
        append_integer(source, object.continuation_clusters);
    }
    for (const auto &adjustment : plan.program_assignment_adjustments) {
        append_field(source, adjustment.adjustment_id);
        append_field(source, package_program_assignment_origin_name(adjustment.origin));
        append_integer(source, adjustment.package_index.value_or(0U));
        append_field(source, adjustment.action_id.value_or(""));
        append_field(source, adjustment.existing_object_key.value_or(""));
        append_field(source, adjustment.program_slot);
        append_field(source, adjustment.program_name);
        append_integer(source, adjustment.assignment_ordinal);
        append_field(source, adjustment.target_object_type);
        append_field(source, adjustment.target_name);
        append_integer(source, adjustment.partition_index);
        append_field(source, adjustment.group_name);
        append_field(source, adjustment.volume_name);
        append_field(source, adjustment.raw_group);
        append_field(source, adjustment.raw_volume);
        append_field(source, adjustment.reason_code);
        append_field(source, package_program_assignment_disposition_name(adjustment.disposition));
    }
    for (const auto &placement : plan.program_slot_placements) {
        append_field(source, placement.placement_id);
        append_integer(source, placement.partition_index);
        append_field(source, placement.volume_name);
        append_field(source, package_program_slot_placement_mode_name(placement.mode));
        append_integer(source, placement.applied);
        append_integer(source, placement.suggested_start_slot.value_or(0U));
        append_integer(source, placement.required_slot_count);
        append_integer(source, placement.available_slot_count);
        append_integer(source, placement.occupied_ranges.size());
        for (const auto &range : placement.occupied_ranges) {
            append_integer(source, range.first);
            append_integer(source, range.last);
        }
        append_integer(source, placement.source_ranges.size());
        for (const auto &range : placement.source_ranges) {
            append_integer(source, range.first);
            append_integer(source, range.last);
        }
        append_integer(source, placement.destination_ranges.size());
        for (const auto &range : placement.destination_ranges) {
            append_integer(source, range.first);
            append_integer(source, range.last);
        }
        append_integer(source, placement.mappings.size());
        for (const auto &mapping : placement.mappings) {
            append_integer(source, mapping.package_index);
            append_field(source, mapping.node_id);
            append_integer(source, mapping.source_slot);
            append_integer(source, mapping.destination_slot);
            append_integer(source, mapping.requires_user_action);
        }
    }
    for (const auto &delta : plan.allocation) {
        append_integer(source, delta.partition_index);
        append_field(source, delta.group_name);
        append_field(source, delta.volume_name);
        append_field(source, delta.raw_group);
        append_field(source, delta.raw_volume);
        append_integer(source, delta.inserted_object_count);
        append_integer(source, delta.reused_object_count);
        append_integer(source, delta.blocked_object_count);
        append_integer(source, delta.payload_clusters);
        append_integer(source, delta.payload_sectors);
        append_integer(source, delta.continuation_clusters);
        append_integer(source, delta.directory_growth_bytes);
        append_integer(source, delta.directory_growth_clusters);
        append_integer(source, delta.directory_continuation_clusters);
        append_integer(source, delta.infrastructure_clusters);
        append_integer(source, delta.additional_allocated_bytes);
        append_integer(source, delta.remaining_object_ids);
        append_integer(source, delta.remaining_clusters);
        append_integer(source, delta.projected_image_sectors);
        append_integer(source, delta.projected_image_size_bytes);
    }
    for (const auto &capacity : plan.sfs_index_capacity) {
        append_integer(source, capacity.partition_index);
        append_integer(source, capacity.index_block_count);
        append_integer(source, capacity.records_per_index_block);
        append_integer(source, capacity.total_record_slots);
        append_integer(source, capacity.reserved_record_slots);
        append_integer(source, capacity.allocatable_record_slots);
        append_integer(source, capacity.used_record_slots);
        append_integer(source, capacity.free_record_slots);
        append_integer(source, capacity.required_record_slots);
        append_integer(source, capacity.allocated_record_slots);
        append_integer(source, capacity.shortfall_record_slots);
        append_integer(source, capacity.remaining_record_slots);
        for (const auto &usage : capacity.packages) {
            append_integer(source, usage.package_index);
            append_integer(source, usage.effective_object_record_slots);
            append_integer(source, usage.volume_scaffolding_record_slots);
            append_integer(source, usage.standalone_required_record_slots);
            append_integer(source, usage.planned_object_record_slots);
            append_integer(source, usage.planned_record_slots);
            append_integer(source, usage.reused_object_count);
            append_integer(source, usage.allocated_record_slots);
            append_integer(source, usage.shortfall_record_slots);
        }
    }
    for (const auto &warning : plan.warnings) {
        append_field(source, warning.code);
        append_field(source, warning.message);
        append_field(source, package_import_warning_origin_name(warning.origin));
        append_integer(source, warning.package_index.value_or(0U));
        append_field(source, warning.node_id);
        append_field(source, warning.object_type);
        append_field(source, warning.object_name);
        append_integer(source, warning.partition_index.value_or(0U));
        append_field(source, warning.volume_name);
    }
    for (const auto &sequence : plan.opaque_sequences) {
        append_integer(source, sequence.package_index);
        append_field(source, sequence.node_id);
        append_field(source, sequence.name);
        append_field(source, sequence.action ? package_opaque_sequence_action_name(*sequence.action) : "");
    }
    for (const auto &object : plan.preserved_target_objects) {
        append_field(source, object.object_key);
        append_integer(source, object.partition_index);
        append_integer(source, object.sfs_id);
        append_field(source, object.object_type);
        append_field(source, object.object_name);
        append_field(source, object.volume_name);
        append_field(source, object.category_name);
        append_field(source, object.entry_name);
        append_field(source, object.payload_sha256);
    }
    for (const auto &conflict : plan.conflicts) {
        append_field(source, conflict.code);
        append_field(source, conflict.message);
        append_field(source, conflict.package_id);
        append_field(source, conflict.node_id);
        append_integer(source, conflict.package_index.value_or(0U));
        append_integer(source, conflict.root_index.value_or(0U));
        append_integer(source, conflict.partition_index.value_or(0U));
        append_field(source, conflict.group_name);
        append_field(source, conflict.volume_name);
        append_field(source, conflict.raw_group);
        append_field(source, conflict.raw_volume);
    }
    return digest_text(source);
}

} // namespace package_import_internal

Result<void> verify_package_import_plan(const PackageImportPlan &plan) {
    if (plan.schema_version != "1.0" || !valid_digest(plan.target_snapshot_id) || !valid_digest(plan.policy_digest) ||
        !valid_digest(plan.plan_id)) {
        return std::unexpected{planner_error("package import plan identity fields are invalid")};
    }
    if (!valid_sfs_index_capacity(plan))
        return std::unexpected{planner_error("package import plan contains invalid SFS index capacity metadata")};
    if (std::ranges::any_of(plan.warnings, [&](const auto &warning) {
            return warning.code.empty() || warning.message.empty() ||
                   (warning.package_index && *warning.package_index >= plan.package_ids.size());
        })) {
        return std::unexpected{planner_error("package import plan contains an invalid warning")};
    }
    if (std::ranges::any_of(plan.opaque_sequences,
                            [&](const auto &sequence) {
                                return sequence.package_index >= plan.package_ids.size() || sequence.node_id.empty() ||
                                       sequence.name.empty();
                            }) ||
        std::ranges::any_of(plan.preserved_target_objects, [&](const auto &object) {
            return object.object_key.empty() || object.object_type.empty() || object.object_name.empty() ||
                   !valid_digest(object.payload_sha256);
        })) {
        return std::unexpected{planner_error("package import plan contains invalid preservation metadata")};
    }

    std::set<std::tuple<std::uint8_t, std::string, std::string, std::string, std::string>> destination_keys;
    for (const auto &destination : plan.destinations) {
        const auto has_infrastructure = !destination.infrastructure_sfs_ids.empty() ||
                                        destination.infrastructure_clusters != 0U ||
                                        destination.root_directory_growth_bytes != 0U;
        const auto record_capacity_exhausted = std::ranges::any_of(plan.conflicts, [&](const auto &conflict) {
            return conflict.code == "SFS_RECORD_CAPACITY_EXHAUSTED" && conflict.partition_index &&
                   *conflict.partition_index == destination.partition_index;
        });
        const auto valid_creation =
            !destination.create ||
            (plan.target_kind == MediaKind::sfs && destination.infrastructure_sfs_ids.size() == 6U &&
             destination.infrastructure_clusters == 12U && destination.root_directory_growth_bytes == 32U) ||
            (plan.target_kind == MediaKind::sfs && !has_infrastructure && record_capacity_exhausted) ||
            (plan.target_kind == MediaKind::iso9660 && !has_infrastructure);
        if (destination.volume_name.empty() ||
            !destination_keys
                 .emplace(destination.partition_index, destination.group_name, destination.volume_name,
                          destination.raw_group, destination.raw_volume)
                 .second ||
            !valid_creation || (!destination.create && has_infrastructure)) {
            return std::unexpected{planner_error("package import plan contains an invalid destination action")};
        }
    }

    std::set<std::string, std::less<>> action_ids;
    std::map<std::string, const PlannedPackageObject *, std::less<>> actions;
    for (const auto &object : plan.objects) {
        if (plan.valid() && std::ranges::find_if(plan.destinations, [&](const auto &destination) {
                                return destination.partition_index == object.partition_index &&
                                       destination.group_name == object.group_name &&
                                       destination.volume_name == object.volume_name &&
                                       destination.raw_group == object.raw_group &&
                                       destination.raw_volume == object.raw_volume;
                            }) == plan.destinations.end()) {
            return std::unexpected{planner_error("package import action has no planned destination")};
        }
        if (object.package_index >= plan.package_ids.size() ||
            object.package_id != plan.package_ids[object.package_index] || object.actions.empty() ||
            !valid_digest(object.action_id) || !valid_digest(object.normalized_sha256) ||
            !action_ids.emplace(object.action_id).second) {
            return std::unexpected{planner_error("package import plan contains an invalid action")};
        }
        const auto inserts = std::ranges::contains(object.actions, PackageImportObjectAction::insert);
        const auto reuses = std::ranges::contains(object.actions, PackageImportObjectAction::reuse);
        const auto conflicts = std::ranges::contains(object.actions, PackageImportObjectAction::conflict);
        const std::set<PackageImportObjectAction> unique_actions(object.actions.begin(), object.actions.end());
        const auto sorted_programs = std::ranges::is_sorted(object.target_program_numbers);
        const std::set<std::uint8_t> unique_programs(object.target_program_numbers.begin(),
                                                     object.target_program_numbers.end());
        if (unique_actions.size() != object.actions.size() || (inserts && reuses) ||
            (plan.valid() && !inserts && !reuses) || (plan.valid() && conflicts) ||
            (!sorted_programs || unique_programs.size() != object.target_program_numbers.size()) ||
            std::ranges::any_of(object.target_program_numbers,
                                [](const auto number) { return number < 1U || number > 128U; }) ||
            (object.object_type != "SBNK" &&
             (!object.target_program_numbers.empty() || object.target_sample_bank_member)) ||
            (plan.target_kind == MediaKind::iso9660
                 ? (object.payload_clusters != 0U || object.continuation_clusters != 0U ||
                    (inserts && !conflicts && object.payload_sectors == 0U))
                 : object.payload_sectors != 0U) ||
            (inserts && !conflicts &&
             ((plan.target_kind == MediaKind::sfs && !object.target_sfs_id) ||
              (object.object_type == "SMPL" && !object.target_wave_data_reference_value))) ||
            (reuses && !conflicts && !object.existing_object_key && !object.canonical_action_id)) {
            return std::unexpected{planner_error("package import plan action decision is "
                                                 "incomplete or contradictory")};
        }
        actions.emplace(object.action_id, &object);
    }
    std::set<std::string, std::less<>> adjustment_ids;
    std::set<std::pair<std::string, std::uint32_t>> adjusted_rows;
    for (const auto &adjustment : plan.program_assignment_adjustments) {
        const auto imported = adjustment.origin == PackageProgramAssignmentOrigin::imported_program;
        const auto existing = adjustment.origin == PackageProgramAssignmentOrigin::existing_program;
        const auto action = adjustment.action_id ? actions.find(*adjustment.action_id) : actions.end();
        const auto scope_matches_action = action != actions.end() && action->second->object_type == "PROG" &&
                                          action->second->destination_name == adjustment.program_slot &&
                                          action->second->partition_index == adjustment.partition_index &&
                                          action->second->group_name == adjustment.group_name &&
                                          action->second->volume_name == adjustment.volume_name &&
                                          action->second->raw_group == adjustment.raw_group &&
                                          action->second->raw_volume == adjustment.raw_volume;
        const auto row_owner =
            imported ? adjustment.action_id.value_or("") : adjustment.existing_object_key.value_or("");
        if (!valid_digest(adjustment.adjustment_id) ||
            adjustment.adjustment_id != package_import_internal::program_assignment_adjustment_identity(adjustment) ||
            !adjustment_ids.emplace(adjustment.adjustment_id).second || row_owner.empty() ||
            !adjusted_rows.emplace(row_owner, adjustment.assignment_ordinal).second ||
            adjustment.program_slot.empty() || adjustment.assignment_ordinal >= 128U ||
            (adjustment.target_object_type != "SBAC" && adjustment.target_object_type != "SBNK") ||
            adjustment.target_name.empty() || adjustment.reason_code != "UNRESOLVED_PROGRAM_ASSIGNMENT_COLLISION" ||
            adjustment.disposition != PackageProgramAssignmentDisposition::clear_assignment ||
            (imported && (!adjustment.package_index || *adjustment.package_index >= plan.package_ids.size() ||
                          !adjustment.action_id || adjustment.existing_object_key || !scope_matches_action ||
                          action->second->package_index != *adjustment.package_index)) ||
            (existing && (adjustment.package_index || adjustment.action_id || !adjustment.existing_object_key)) ||
            (!imported && !existing)) {
            return std::unexpected{
                planner_error("package import plan contains an invalid Program assignment adjustment")};
        }
    }
    std::set<std::string, std::less<>> placement_ids;
    for (const auto &placement : plan.program_slot_placements) {
        const auto unavailable = placement.mode == PackageProgramSlotPlacementMode::unavailable;
        const auto contiguous = placement.mode == PackageProgramSlotPlacementMode::contiguous;
        const auto occupied_ranges_valid = valid_slot_ranges(placement.occupied_ranges);
        const auto destination_span_matches = placement.destination_ranges.size() == 1U &&
                                              static_cast<std::uint64_t>(placement.destination_ranges.front().last -
                                                                         placement.destination_ranges.front().first +
                                                                         1U) == placement.required_slot_count;
        std::set<std::pair<std::size_t, std::string>> mapping_nodes;
        std::map<std::uint8_t, std::size_t> destination_counts;
        for (const auto &mapping : placement.mappings)
            ++destination_counts[mapping.destination_slot];
        bool mappings_valid = true;
        for (const auto &mapping : placement.mappings) {
            const auto matching_action = std::ranges::any_of(plan.objects, [&](const auto &object) {
                return object.package_index == mapping.package_index && object.node_id == mapping.node_id &&
                       object.object_type == "PROG" && object.partition_index == placement.partition_index &&
                       object.volume_name == placement.volume_name &&
                       object.source_name == std::format("{:03}", mapping.source_slot) &&
                       (!placement.applied ||
                        object.destination_name == std::format("{:03}", mapping.destination_slot));
            });
            const auto requires_action = slot_in_ranges(mapping.destination_slot, placement.occupied_ranges) ||
                                         destination_counts[mapping.destination_slot] > 1U;
            mappings_valid = mappings_valid && mapping.package_index < plan.package_ids.size() &&
                             !mapping.node_id.empty() && mapping.source_slot >= 1U && mapping.source_slot <= 128U &&
                             mapping.destination_slot >= 1U && mapping.destination_slot <= 128U &&
                             mapping_nodes.emplace(mapping.package_index, mapping.node_id).second && matching_action &&
                             mapping.requires_user_action == requires_action;
        }
        if (plan.target_kind != MediaKind::sfs || !valid_digest(placement.placement_id) ||
            placement.placement_id != package_import_internal::program_slot_placement_identity(
                                          {placement.partition_index, placement.volume_name}) ||
            !placement_ids.emplace(placement.placement_id).second || placement.volume_name.empty() ||
            placement.required_slot_count == 0U || placement.available_slot_count > 128U || !occupied_ranges_valid ||
            (occupied_ranges_valid &&
             placement.available_slot_count != 128U - slot_range_cardinality(placement.occupied_ranges)) ||
            !valid_slot_ranges(placement.source_ranges) || !valid_slot_ranges(placement.destination_ranges) ||
            !mappings_valid ||
            (unavailable && (!placement.mappings.empty() || placement.suggested_start_slot ||
                             !placement.destination_ranges.empty())) ||
            (!unavailable && (placement.mappings.size() != placement.required_slot_count ||
                              !placement.suggested_start_slot || placement.destination_ranges.empty())) ||
            (contiguous != destination_span_matches)) {
            return std::unexpected{planner_error("package import plan contains an invalid Program slot placement")};
        }
    }
    for (const auto &object : plan.objects) {
        if (!object.canonical_action_id)
            continue;
        const auto canonical = actions.find(*object.canonical_action_id);
        if (canonical == actions.end() ||
            !std::ranges::contains(canonical->second->actions, PackageImportObjectAction::insert) ||
            object.partition_index != canonical->second->partition_index ||
            object.group_name != canonical->second->group_name ||
            object.volume_name != canonical->second->volume_name || object.raw_group != canonical->second->raw_group ||
            object.raw_volume != canonical->second->raw_volume ||
            object.object_type != canonical->second->object_type ||
            object.destination_name != canonical->second->destination_name ||
            object.normalized_sha256 != canonical->second->normalized_sha256 ||
            object.target_sfs_id != canonical->second->target_sfs_id ||
            object.target_wave_data_reference_value != canonical->second->target_wave_data_reference_value ||
            object.target_program_numbers != canonical->second->target_program_numbers ||
            object.target_sample_bank_member != canonical->second->target_sample_bank_member) {
            return std::unexpected{planner_error("package import plan canonical reuse binding is invalid")};
        }
    }
    for (const auto &allocation : plan.allocation) {
        const auto iso = plan.target_kind == MediaKind::iso9660;
        if ((iso && (allocation.payload_clusters != 0U || allocation.continuation_clusters != 0U ||
                     allocation.directory_growth_clusters != 0U || allocation.directory_continuation_clusters != 0U ||
                     allocation.infrastructure_clusters != 0U || allocation.projected_image_sectors == 0U ||
                     allocation.projected_image_size_bytes != allocation.projected_image_sectors * 2048U)) ||
            (!iso && (allocation.payload_sectors != 0U || allocation.projected_image_sectors != 0U ||
                      allocation.projected_image_size_bytes != 0U))) {
            return std::unexpected{planner_error("package import plan allocation units do not "
                                                 "match the target media")};
        }
    }
    if (plan.plan_id != package_import_internal::plan_identity(plan))
        return std::unexpected{planner_error("package import plan identity does not match its actions")};
    return {};
}

} // namespace axk
