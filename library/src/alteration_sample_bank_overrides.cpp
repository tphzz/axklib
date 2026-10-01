#include "alteration_internal.hpp"

#include "axklib/package_archive.hpp"
#include "axklib/sample_bank_overrides.hpp"

namespace axk::alteration_internal {
Result<OperationReport> update_sample_bank_overrides(TransactionState &state, OperationContext context,
                                                     const UpdateSampleBankOverridesOperation &operation,
                                                     const CancellationToken &cancellation) {
    const auto index = resolve_partition(state, operation.partition);
    if (!index)
        return std::unexpected{index.error()};
    const auto found = state.partitions.find(index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("Partition does not exist")};
    auto &partition = found->second;
    const auto target = category_object(state, partition, operation.volume_name, "SBAC", operation.sample_bank_name,
                                        "SBAC", cancellation);
    if (!target)
        return std::unexpected{target.error()};
    auto payload = current_payload(state, partition, target->second, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    if (package_internal::hex_digest(package_internal::sha256(*payload)) != operation.expected_payload_sha256)
        return std::unexpected{make_error(ErrorCode::transaction_stale, ErrorCategory::transaction,
                                          "Sample Bank no longer matches the edited baseline")};
    if (auto applied = apply_sample_bank_overrides(*payload, operation.overrides); !applied)
        return std::unexpected{applied.error()};
    if (auto replaced = replace_record_payload(state, partition, target->second, std::move(*payload), cancellation);
        !replaced)
        return std::unexpected{replaced.error()};
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *index;
    report.volume_name = operation.volume_name;
    report.object_name = operation.sample_bank_name;
    return report;
}
} // namespace axk::alteration_internal
