#include "alteration_internal.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <variant>

#include "axklib/object.hpp"
#include "axklib/writer_internal.hpp"

namespace axk::alteration_internal {

Result<OperationReport> update_sample_bank_parameters(TransactionState &state, OperationContext context,
                                                      const UpdateSampleBankParametersOperation &operation,
                                                      const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    const auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("partition index does not exist")};
    auto &partition = found->second;
    auto located = category_object(state, partition, operation.volume_name, "SBAC", operation.sample_bank_name, "SBAC",
                                   cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto banks = category_objects(state, partition, operation.volume_name, "SBAC", ObjectType::sbac, cancellation);
    if (!banks)
        return std::unexpected{banks.error()};
    const auto target = std::ranges::find(*banks, located->second, &CategoryObject::id);
    if (target == banks->end())
        return std::unexpected{transaction_error("Sample Bank target changed")};
    const auto *bank = std::get_if<CurrentSbac>(&target->decoded.payload);
    if (!bank || bank->slots.size() != bank->stored_member_count ||
        bank->stored_member_count > bank->maximum_member_count)
        return std::unexpected{transaction_error("Sample Bank membership is unreadable")};
    std::set<std::string> members;
    for (const auto &slot : bank->slots) {
        if (!slot.active || !members.insert(slot.name).second)
            return std::unexpected{transaction_error("Sample Bank membership is incomplete or duplicated")};
    }
    for (const auto &other : *banks) {
        if (other.id == target->id)
            continue;
        const auto *decoded = std::get_if<CurrentSbac>(&other.decoded.payload);
        if (!decoded || decoded->slots.size() != decoded->stored_member_count)
            return std::unexpected{transaction_error("Sample Bank membership is unreadable")};
        for (const auto &slot : decoded->slots) {
            if (slot.active && members.contains(slot.name))
                return std::unexpected{transaction_error("Sample belongs to more than one Sample Bank")};
        }
    }

    // Validate every merged member before staging any object replacement.
    std::map<SfsId, std::vector<std::byte>> replacements;
    auto bank_payload = target->payload;
    if (auto updated = detail::apply_sample_bank_parameters_to_payload(bank_payload, operation.parameters); !updated)
        return std::unexpected{updated.error()};
    replacements.emplace(target->id, std::move(bank_payload));
    for (const auto &name : members) {
        auto member = category_object(state, partition, operation.volume_name, "SBNK", name, "SBNK", cancellation);
        if (!member)
            return std::unexpected{member.error()};
        auto payload = current_payload(state, partition, member->second, cancellation);
        if (!payload)
            return std::unexpected{payload.error()};
        if (auto updated = detail::apply_sample_parameters_to_payload(*payload, operation.parameters); !updated)
            return std::unexpected{updated.error()};
        replacements.emplace(member->second, std::move(*payload));
    }
    for (auto &[id, payload] : replacements) {
        if (auto replaced = replace_fixed_object_payload(state, partition, id, std::move(payload), cancellation);
            !replaced)
            return std::unexpected{replaced.error()};
    }
    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.object_name = operation.sample_bank_name;
    return report;
}

} // namespace axk::alteration_internal
