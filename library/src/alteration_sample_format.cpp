#include "alteration_internal.hpp"

#include <type_traits>
#include <utility>

#include "axklib/package_archive.hpp"
#include "axklib/sample_format_conversion.hpp"

namespace axk::alteration_internal {
namespace {

template <typename Operation>
Result<OperationReport> convert_format(TransactionState &state, OperationContext context, const Operation &operation,
                                       const CancellationToken &cancellation) {
    constexpr bool bank = std::is_same_v<Operation, ConvertSampleBankFormatOperation>;
    const auto &name = [&]() -> const std::string & {
        if constexpr (bank)
            return operation.sample_bank_name;
        else
            return operation.sample_name;
    }();
    const auto type = bank ? "SBAC" : "SBNK";
    const auto index = resolve_partition(state, operation.partition);
    if (!index)
        return std::unexpected{index.error()};
    const auto found = state.partitions.find(index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("partition index does not exist")};
    auto &partition = found->second;
    const auto located = category_object(state, partition, operation.volume_name, type, name, type, cancellation);
    if (!located)
        return std::unexpected{located.error()};
    const auto payload = current_payload(state, partition, located->second, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    if (package_internal::hex_digest(package_internal::sha256(*payload)) != operation.expected_payload_sha256)
        return std::unexpected{make_error(ErrorCode::transaction_stale, ErrorCategory::transaction,
                                          "Sample no longer matches the conversion baseline")};
    auto plan = bank ? plan_sample_bank_format_conversion(*payload, operation.target_format)
                     : plan_sample_format_conversion(*payload, operation.target_format);
    if (!plan.allowed())
        return std::unexpected{
            transaction_error("Sample format conversion is blocked: " + plan.blockers.front().message)};
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *index;
    report.volume_name = operation.volume_name;
    report.object_name = name;
    if (plan.no_op)
        return report;
    const auto growth =
        grow_record_capacity(state, partition, located->second, plan.converted_payload.size(), cancellation);
    if (!growth)
        return std::unexpected{growth.error()};
    if (auto replaced =
            replace_record_payload(state, partition, located->second, std::move(plan.converted_payload), cancellation);
        !replaced)
        return std::unexpected{replaced.error()};
    report.allocated_clusters = growth->first + growth->second;
    return report;
}
} // namespace

Result<OperationReport> convert_sbnk_format(TransactionState &state, OperationContext context,
                                            const ConvertSampleFormatOperation &operation,
                                            const CancellationToken &cancellation) {
    return convert_format(state, context, operation, cancellation);
}

Result<OperationReport> convert_sbac_format(TransactionState &state, OperationContext context,
                                            const ConvertSampleBankFormatOperation &operation,
                                            const CancellationToken &cancellation) {
    return convert_format(state, context, operation, cancellation);
}

} // namespace axk::alteration_internal
