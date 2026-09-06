#include "alteration_internal.hpp"

#include <format>
#include <utility>

#include "axklib/program_parameter_codec.hpp"

namespace axk::alteration_internal {

Result<OperationReport> update_program_parameters(TransactionState &state, OperationContext context,
                                                  const UpdateProgramParametersOperation &operation,
                                                  const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    const auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("partition index does not exist")};
    auto &partition = found->second;
    const auto slot = std::format("{:03}", operation.program_number);
    const auto located = category_object(state, partition, operation.volume_name, "PROG", slot, "PROG", cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto payload = current_payload(state, partition, located->second, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};

    // Both scopes edit a private payload before anything enters the transaction's changed records.
    if (auto applied = detail::apply_program_parameters(*payload, operation.parameters, operation.model); !applied)
        return std::unexpected{applied.error()};
    if (auto applied = detail::apply_program_assignment_patches(*payload, operation.assignments, operation.model);
        !applied)
        return std::unexpected{applied.error()};
    if (auto replaced =
            replace_fixed_object_payload(state, partition, located->second, std::move(*payload), cancellation);
        !replaced)
        return std::unexpected{replaced.error()};

    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = slot;
    return report;
}

} // namespace axk::alteration_internal
