#include "alteration_internal.hpp"

#include <utility>

#include "axklib/writer_internal.hpp"

namespace axk::alteration_internal {

Result<OperationReport> update_wave_data_parameters(TransactionState &state, OperationContext context,
                                                    const UpdateWaveDataParametersOperation &operation,
                                                    const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    const auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("partition index does not exist")};
    auto &partition = found->second;
    auto located =
        category_object(state, partition, operation.volume_name, "SMPL", operation.waveform_name, "SMPL", cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto payload = current_payload(state, partition, located->second, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    if (auto applied = detail::apply_wave_data_parameters_to_payload(*payload, operation.parameters); !applied)
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
    report.object_name = operation.waveform_name;
    return report;
}

} // namespace axk::alteration_internal
