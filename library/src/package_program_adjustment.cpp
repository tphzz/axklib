#include "package_import_support.hpp"

#include <algorithm>
#include <format>
#include <ranges>
#include <set>
#include <span>
#include <tuple>

#include "package_import_internal.hpp"

namespace axk::package_import_internal {
namespace {

constexpr std::string_view collision_reason{"UNRESOLVED_PROGRAM_ASSIGNMENT_COLLISION"};

void append_field(std::string &target, std::string_view value) { target += std::format("{}:{};", value.size(), value); }

template <typename Integer> void append_integer(std::string &target, Integer value) {
    append_field(target, std::to_string(value));
}

std::string digest_text(std::string_view value) {
    return package_internal::hex_digest(package_internal::sha256(std::as_bytes(std::span{value})));
}

bool same_scope(const PackageRootDestination &left, const PackageRootDestination &right) {
    return left.partition_index == right.partition_index && left.group_name == right.group_name &&
           left.volume_name == right.volume_name && left.raw_group == right.raw_group &&
           left.raw_volume == right.raw_volume;
}

bool existing_in_scope(const ObjectSnapshot &snapshot, const PackageRootDestination &destination) {
    if (!destination.raw_group.empty() || !destination.raw_volume.empty()) {
        const auto scope = iso_raw_scope(snapshot);
        return scope && scope->first == destination.raw_group && scope->second == destination.raw_volume;
    }
    if (destination.volume_name == "FAT root")
        return true;
    return snapshot.placement && destination.partition_index &&
           snapshot.partition.value == *destination.partition_index &&
           snapshot.placement->volume_name == destination.volume_name;
}

bool active_program_row(const ProgAssignment &assignment) {
    return !assignment.name.empty() && (assignment.kind == 0x10U || assignment.kind == 0x11U) &&
           std::to_integer<std::uint8_t>(assignment.raw_row[0x28U]) == 0xffU;
}

std::string_view assignment_target_type(const ProgAssignment &assignment) {
    return assignment.kind == 0x11U ? "SBAC" : "SBNK";
}

std::string_view assignment_role(const ProgAssignment &assignment) {
    return assignment.kind == 0x11U ? "PROG_ASSIGNMENT_TO_SBAC" : "PROG_ASSIGNMENT_TO_SBNK";
}

bool package_row_has_edge(const Candidate &candidate, std::uint32_t ordinal, const ProgAssignment &assignment) {
    return std::ranges::any_of(candidate.package->relationships, [&](const PackageRelationship &edge) {
        return edge.source_node_id == candidate.node->node_id && edge.ordinal == ordinal &&
               edge.role == assignment_role(assignment);
    });
}

bool candidate_target_exists(const Candidate &program, std::span<const Candidate> candidates,
                             std::string_view object_type, std::string_view name) {
    return std::ranges::any_of(candidates, [&](const Candidate &candidate) {
        return candidate.node->object_type == object_type && candidate.destination_name == name &&
               same_scope(*program.destination, *candidate.destination);
    });
}

bool existing_target_exists(const Candidate &program, std::span<const ExistingObject> existing,
                            std::string_view object_type, std::string_view name) {
    return std::ranges::any_of(existing, [&](const ExistingObject &candidate) {
        return candidate.snapshot->object.header.raw_type == object_type &&
               candidate.snapshot->object.header.name == name &&
               existing_in_scope(*candidate.snapshot, *program.destination);
    });
}

Result<std::string> adjusted_candidate_identity(const Candidate &candidate, std::span<const Candidate> candidates) {
    package_internal::PackageNodeRelocationContext context;
    context.destination_name = candidate.destination_name;
    context.cleared_program_assignment_ordinals = candidate.cleared_program_assignment_ordinals;
    for (const auto &edge : candidate.package->relationships) {
        if (edge.source_node_id != candidate.node->node_id)
            continue;
        const auto target = std::ranges::find_if(candidates, [&](const Candidate &item) {
            return item.package == candidate.package && item.destination == candidate.destination &&
                   item.node->node_id == edge.target_node_id;
        });
        if (target == candidates.end()) {
            return std::unexpected{planner_error("package relationship target is absent from its destination root")};
        }
        context.edge_target_names.emplace(edge.edge_id, target->destination_name);
    }
    return projected_normalized_sha256(*candidate.package, *candidate.node, context);
}

PackageProgramAssignmentAdjustment imported_adjustment(const Candidate &candidate, const CurrentProg &program,
                                                       std::uint32_t ordinal, const ProgAssignment &assignment) {
    PackageProgramAssignmentAdjustment adjustment;
    adjustment.origin = PackageProgramAssignmentOrigin::imported_program;
    adjustment.package_index = candidate.destination->package_index;
    adjustment.action_id = action_identity(candidate);
    adjustment.program_slot = candidate.destination_name;
    adjustment.program_name = program.program_name;
    adjustment.assignment_ordinal = ordinal;
    adjustment.target_object_type = assignment_target_type(assignment);
    adjustment.target_name = assignment.name;
    adjustment.partition_index = candidate.destination->partition_index.value_or(0U);
    adjustment.group_name = candidate.destination->group_name;
    adjustment.volume_name = candidate.destination->volume_name;
    adjustment.raw_group = candidate.destination->raw_group;
    adjustment.raw_volume = candidate.destination->raw_volume;
    adjustment.reason_code = collision_reason;
    adjustment.adjustment_id = program_assignment_adjustment_identity(adjustment);
    return adjustment;
}

bool incoming_program_replaces(const ObjectSnapshot &program, std::span<const Candidate> candidates) {
    return std::ranges::any_of(candidates, [&](const Candidate &candidate) {
        return candidate.node->object_type == "PROG" && candidate.destination_name == program.object.header.name &&
               existing_in_scope(program, *candidate.destination);
    });
}

} // namespace

std::string program_assignment_adjustment_identity(const PackageProgramAssignmentAdjustment &adjustment) {
    std::string source;
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
    return digest_text(source);
}

Result<void> plan_program_assignment_adjustments(std::vector<Candidate> &candidates,
                                                 std::span<const ExistingObject> existing, PackageImportPlan &plan) {
    for (auto &candidate : candidates) {
        candidate.unadjusted_normalized_sha256 = candidate.projected_normalized_sha256;
        if (candidate.node->object_type != "PROG")
            continue;
        const auto decoded = decode_object(candidate.node->raw_payload);
        if (!decoded)
            return std::unexpected{decoded.error()};
        const auto *program = std::get_if<CurrentProg>(&decoded->payload);
        if (program == nullptr)
            return std::unexpected{planner_error("planned Program package node is not decoded")};
        for (std::size_t index = 0; index < program->assignments.size(); ++index) {
            const auto &assignment = program->assignments[index];
            const auto ordinal = static_cast<std::uint32_t>(index);
            if (!active_program_row(assignment) || package_row_has_edge(candidate, ordinal, assignment))
                continue;
            const auto type = assignment_target_type(assignment);
            if (!candidate_target_exists(candidate, candidates, type, assignment.name) &&
                !existing_target_exists(candidate, existing, type, assignment.name)) {
                continue;
            }
            candidate.cleared_program_assignment_ordinals.push_back(ordinal);
            plan.program_assignment_adjustments.push_back(
                imported_adjustment(candidate, *program, ordinal, assignment));
        }
        if (!candidate.cleared_program_assignment_ordinals.empty()) {
            auto identity = adjusted_candidate_identity(candidate, candidates);
            if (!identity)
                return std::unexpected{identity.error()};
            candidate.projected_normalized_sha256 = std::move(*identity);
        }
    }

    std::set<std::pair<std::string, std::uint32_t>> adjusted_existing_rows;
    for (const auto &item : existing) {
        const auto &snapshot = *item.snapshot;
        if (snapshot.object.header.raw_type != "PROG" || incoming_program_replaces(snapshot, candidates))
            continue;
        const auto *program = std::get_if<CurrentProg>(&snapshot.object.payload);
        if (program == nullptr)
            continue;
        for (std::size_t index = 0; index < program->assignments.size(); ++index) {
            const auto &assignment = program->assignments[index];
            const auto ordinal = static_cast<std::uint32_t>(index);
            if (!active_program_row(assignment) || assignment.raw_handle != 0U)
                continue;
            const auto target = std::ranges::find_if(candidates, [&](const Candidate &candidate) {
                return candidate.node->object_type == assignment_target_type(assignment) &&
                       candidate.destination_name == assignment.name &&
                       existing_in_scope(snapshot, *candidate.destination);
            });
            if (target == candidates.end() ||
                existing_target_exists(*target, existing, assignment_target_type(assignment), assignment.name) ||
                !adjusted_existing_rows.emplace(snapshot.key, ordinal).second) {
                continue;
            }
            PackageProgramAssignmentAdjustment adjustment;
            adjustment.origin = PackageProgramAssignmentOrigin::existing_program;
            adjustment.existing_object_key = snapshot.key;
            adjustment.program_slot = snapshot.object.header.name;
            adjustment.program_name = program->program_name;
            adjustment.assignment_ordinal = ordinal;
            adjustment.target_object_type = assignment_target_type(assignment);
            adjustment.target_name = assignment.name;
            adjustment.partition_index = target->destination->partition_index.value_or(0U);
            adjustment.group_name = target->destination->group_name;
            adjustment.volume_name = target->destination->volume_name;
            adjustment.raw_group = target->destination->raw_group;
            adjustment.raw_volume = target->destination->raw_volume;
            adjustment.reason_code = collision_reason;
            adjustment.adjustment_id = program_assignment_adjustment_identity(adjustment);
            plan.program_assignment_adjustments.push_back(std::move(adjustment));
        }
    }

    std::ranges::sort(plan.program_assignment_adjustments, [](const auto &left, const auto &right) {
        return std::tie(left.partition_index, left.raw_group, left.raw_volume, left.volume_name, left.program_slot,
                        left.assignment_ordinal, left.adjustment_id) <
               std::tie(right.partition_index, right.raw_group, right.raw_volume, right.volume_name, right.program_slot,
                        right.assignment_ordinal, right.adjustment_id);
    });
    return {};
}

} // namespace axk::package_import_internal
