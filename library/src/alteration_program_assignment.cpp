#include "alteration_internal.hpp"

#include <algorithm>
#include <format>
#include <ranges>

#include "axklib/package_relocation.hpp"

namespace axk::alteration_internal {

Result<OperationReport> clear_program_assignments(TransactionState &state, OperationContext context,
                                                  const ClearProgramAssignmentsOperation &operation,
                                                  const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("clear-program-assignments target is invalid")};

    auto &partition = found->second;
    const auto slot = std::format("{:03}", operation.program_number);
    auto located = category_object(state, partition, operation.volume_name, "PROG", slot, "PROG", cancellation);
    if (!located)
        return std::unexpected{located.error()};
    const auto snapshot = std::ranges::find_if(state.catalog.objects, [&](const ObjectSnapshot &object) {
        return object.partition == *partition_index && object.sfs_id == located->second;
    });
    if (snapshot == state.catalog.objects.end())
        return std::unexpected{transaction_error("Program is absent from the relationship graph")};

    for (const auto ordinal : operation.assignment_ordinals) {
        const auto relationship = std::ranges::find_if(state.graph.relationships, [&](const Relationship &row) {
            return row.source_key == snapshot->key && row.assignment_index == ordinal;
        });
        if (relationship == state.graph.relationships.end() ||
            relationship->assignment_state != AssignmentState::stored_assignment || relationship->target_key) {
            return std::unexpected{transaction_error(
                std::format("Program {} assignment {} is not an unresolved stored assignment", slot, ordinal + 1U))};
        }
    }

    auto payload = current_payload(state, partition, located->second, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    std::vector<std::uint32_t> ordinals(operation.assignment_ordinals.begin(), operation.assignment_ordinals.end());
    auto cleared = package_internal::clear_program_assignment_rows(*payload, ordinals);
    if (!cleared)
        return std::unexpected{cleared.error()};
    if (auto replaced =
            replace_fixed_object_payload(state, partition, located->second, std::move(*cleared), cancellation);
        !replaced) {
        return std::unexpected{replaced.error()};
    }

    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = snapshot->object.header.name;
    return report;
}

} // namespace axk::alteration_internal
