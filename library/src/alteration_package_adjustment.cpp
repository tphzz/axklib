#include "alteration_internal.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <ranges>

namespace axk::alteration_internal {
namespace {

std::string_view assignment_target_type(const ProgAssignment &assignment) {
    return assignment.kind == 0x11U ? "SBAC" : "SBNK";
}

Result<const CurrentProg *>
validate_program_owner(const DecodedObject &decoded,
                       std::span<const PackageProgramAssignmentAdjustment *const> adjustments) {
    if (adjustments.empty())
        return std::unexpected{transaction_error("Program assignment adjustment group is empty")};
    const auto *program = std::get_if<CurrentProg>(&decoded.payload);
    if (decoded.header.raw_type != "PROG" || program == nullptr)
        return std::unexpected{transaction_error("Program assignment adjustment owner is not a Program")};
    for (const auto *adjustment : adjustments) {
        if (adjustment == nullptr || decoded.header.name != adjustment->program_slot ||
            program->program_name != adjustment->program_name) {
            return std::unexpected{transaction_error("Program assignment adjustment owner changed after planning")};
        }
    }
    return program;
}

Result<void> validate_source_rows(const CurrentProg &program,
                                  std::span<const PackageProgramAssignmentAdjustment *const> adjustments) {
    for (const auto *adjustment : adjustments) {
        if (adjustment->assignment_ordinal >= program.assignments.size())
            return std::unexpected{transaction_error("Program assignment adjustment row is unavailable")};
        const auto &assignment = program.assignments[adjustment->assignment_ordinal];
        if (assignment.name != adjustment->target_name ||
            assignment_target_type(assignment) != adjustment->target_object_type ||
            (adjustment->origin == PackageProgramAssignmentOrigin::existing_program && assignment.raw_handle != 0U)) {
            return std::unexpected{transaction_error("Program assignment adjustment row changed after planning")};
        }
    }
    return {};
}

} // namespace

Result<std::vector<std::byte>>
clear_program_assignment_adjustments(std::span<const std::byte> payload,
                                     std::span<const PackageProgramAssignmentAdjustment *const> adjustments) {
    auto decoded = decode_object(payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    auto program = validate_program_owner(*decoded, adjustments);
    if (!program)
        return std::unexpected{program.error()};
    if (auto validated = validate_source_rows(**program, adjustments); !validated)
        return std::unexpected{validated.error()};
    std::vector<std::uint32_t> ordinals;
    ordinals.reserve(adjustments.size());
    for (const auto *adjustment : adjustments)
        ordinals.push_back(adjustment->assignment_ordinal);
    return package_internal::clear_program_assignment_rows(payload, ordinals);
}

Result<void> validate_cleared_program_assignment_adjustments(
    const ObjectSnapshot &snapshot, std::span<const PackageProgramAssignmentAdjustment *const> adjustments) {
    auto program = validate_program_owner(snapshot.object, adjustments);
    if (!program)
        return std::unexpected{program.error()};
    for (const auto *adjustment : adjustments) {
        if (adjustment->assignment_ordinal >= (*program)->assignments.size() ||
            (*program)->assignments[adjustment->assignment_ordinal].raw_row != std::array<std::byte, 0x38>{}) {
            return std::unexpected{
                transaction_error(std::format("Program '{}' assignment row {} was not cleared",
                                              adjustment->program_name, adjustment->assignment_ordinal))};
        }
    }
    return {};
}

Result<void> apply_existing_sfs_program_assignment_adjustments(TransactionState &state, const PackageImportPlan &plan,
                                                               const CancellationToken &cancellation) {
    std::map<std::string, std::vector<const PackageProgramAssignmentAdjustment *>, std::less<>> grouped;
    for (const auto &adjustment : plan.program_assignment_adjustments) {
        if (adjustment.origin == PackageProgramAssignmentOrigin::existing_program)
            grouped[*adjustment.existing_object_key].push_back(&adjustment);
    }
    for (const auto &[object_key, adjustments] : grouped) {
        if (const auto checked = cancellation.check(); !checked)
            return std::unexpected{checked.error()};
        const auto snapshot = std::ranges::find(state.catalog.objects, object_key, &ObjectSnapshot::key);
        if (snapshot == state.catalog.objects.end() ||
            snapshot->partition.value != adjustments.front()->partition_index)
            return std::unexpected{transaction_error("planned existing Program adjustment owner is unavailable")};
        const auto partition = state.partitions.find(snapshot->partition.value);
        if (partition == state.partitions.end())
            return std::unexpected{transaction_error("existing Program adjustment partition is unavailable")};
        auto payload = current_payload(state, partition->second, snapshot->sfs_id, cancellation);
        if (!payload)
            return std::unexpected{payload.error()};
        auto cleared = clear_program_assignment_adjustments(*payload, adjustments);
        if (!cleared)
            return std::unexpected{cleared.error()};
        if (auto replaced = replace_fixed_object_payload(state, partition->second, snapshot->sfs_id,
                                                         std::move(*cleared), cancellation);
            !replaced) {
            return std::unexpected{replaced.error()};
        }
    }
    return {};
}

} // namespace axk::alteration_internal
