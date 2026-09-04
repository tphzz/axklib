#include "alteration_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <ranges>
#include <set>
#include <span>

#include "axklib/bytes.hpp"
#include "axklib/object.hpp"

namespace axk::alteration_internal {
namespace {

constexpr std::size_t slot_base = 0x14cU;
constexpr std::size_t slot_size = 0x14U;
constexpr std::size_t trailing_parameter_bytes = 0x24U;

Result<std::map<std::string, std::vector<SfsId>>>
sample_bank_memberships(const std::vector<CategoryObject> &sample_banks) {
    std::map<std::string, std::vector<SfsId>> result;
    for (const auto &row : sample_banks) {
        const auto *sample_bank = std::get_if<CurrentSbac>(&row.decoded.payload);
        if (sample_bank == nullptr || sample_bank->stored_member_count > sample_bank->maximum_member_count ||
            sample_bank->slots.size() != sample_bank->stored_member_count) {
            return std::unexpected{transaction_error("Sample Bank membership is unreadable")};
        }
        for (const auto &slot : sample_bank->slots) {
            if (slot.active)
                result[slot.name].push_back(row.id);
        }
    }
    return result;
}

Result<void> detach_members(TransactionState &state, MutablePartition &partition,
                            const std::vector<CategoryObject> &sample_banks,
                            const std::map<std::string, SfsId> &member_ids, PartitionIndex partition_index,
                            const CancellationToken &cancellation) {
    for (const auto &row : sample_banks) {
        const auto *sample_bank = std::get_if<CurrentSbac>(&row.decoded.payload);
        const auto removed = std::ranges::count_if(
            sample_bank->slots, [&](const SbacSlot &slot) { return member_ids.contains(slot.name); });
        if (removed == 0)
            continue;
        auto payload = row.payload;
        std::size_t write_index{};
        for (const auto &slot : sample_bank->slots) {
            if (member_ids.contains(slot.name))
                continue;
            std::array<std::byte, slot_size> slot_bytes{};
            const auto source = std::span{payload}.subspan(slot.offset, slot_size);
            const auto target = std::span{payload}.subspan(slot_base + write_index * slot_size, slot_size);
            std::ranges::copy(source, slot_bytes.begin());
            std::ranges::copy(slot_bytes, target.begin());
            ++write_index;
        }
        const auto old_slot_end = slot_base + sample_bank->slots.size() * slot_size;
        std::ranges::fill(std::span{payload}.subspan(slot_base + write_index * slot_size,
                                                     old_slot_end - slot_base - write_index * slot_size),
                          std::byte{});
        payload[0x144U] = static_cast<std::byte>(write_index);
        if (auto replaced = replace_fixed_object_payload(state, partition, row.id, std::move(payload), cancellation);
            !replaced) {
            return std::unexpected{replaced.error()};
        }
        std::erase_if(state.known_edges, [&](const auto &edge) {
            const auto &[edge_partition, source, target] = edge;
            return edge_partition == partition_index && source == row.id &&
                   std::ranges::any_of(member_ids, [&](const auto &member) { return member.second == target; });
        });
    }
    return {};
}

} // namespace

Result<void> append_sbac_members_to_payload(std::vector<std::byte> &payload, const CurrentSbac &sample_bank,
                                            const std::vector<std::string> &names) {
    const auto existing_count = static_cast<std::size_t>(sample_bank.stored_member_count);
    if (sample_bank.slots.size() != existing_count || existing_count > sample_bank.maximum_member_count ||
        existing_count > maximum_sample_bank_members)
        return std::unexpected{transaction_error("Target Sample Bank membership is unreadable")};
    if (names.size() > maximum_sample_bank_members - existing_count)
        return std::unexpected{transaction_error("Target Sample Bank would exceed 127 Samples")};

    const auto final_count = existing_count + names.size();
    const auto existing_slot_end = slot_base + existing_count * slot_size;
    if (payload.size() < existing_slot_end)
        return std::unexpected{transaction_error("Target Sample Bank slot table is truncated")};

    std::size_t member_region_end{};
    if (sample_bank.storage_layout == SbacStorageLayout::current_split_parameter_tail) {
        if (!sample_bank.parameter_tail_offset ||
            *sample_bank.parameter_tail_offset + trailing_parameter_bytes != payload.size()) {
            return std::unexpected{transaction_error("Target Sample Bank parameter tail is unreadable")};
        }
        member_region_end = *sample_bank.parameter_tail_offset;
    } else {
        if (sample_bank.parameter_tail_offset)
            return std::unexpected{transaction_error("Target Sample Bank legacy layout is inconsistent")};
        member_region_end = payload.size();
    }
    if (member_region_end < slot_base || (member_region_end - slot_base) % slot_size != 0U)
        return std::unexpected{transaction_error("Target Sample Bank member capacity is not row-aligned")};
    if ((member_region_end - slot_base) / slot_size != sample_bank.maximum_member_count)
        return std::unexpected{transaction_error("Target Sample Bank member capacity changed")};

    if (final_count > sample_bank.maximum_member_count) {
        const auto added_rows = final_count - sample_bank.maximum_member_count;
        if (sample_bank.storage_layout == SbacStorageLayout::current_split_parameter_tail) {
            payload.insert(payload.begin() + static_cast<std::ptrdiff_t>(member_region_end), added_rows * slot_size,
                           std::byte{});
        } else {
            payload.resize(payload.size() + added_rows * slot_size);
        }
    }

    ByteWriter writer{payload};
    if (sample_bank.storage_layout == SbacStorageLayout::current_split_parameter_tail) {
        if (auto written = writer.write_be32(0x18U, static_cast<std::uint32_t>(payload.size() - 0x54U)); !written)
            return std::unexpected{written.error()};
        if (auto written = writer.write_be32(0x1cU, static_cast<std::uint32_t>(payload.size() - 0x30U)); !written)
            return std::unexpected{written.error()};
    } else if (auto written = writer.write_be32(0x18U, static_cast<std::uint32_t>(payload.size() - 0x30U)); !written) {
        return std::unexpected{written.error()};
    }
    payload[0x144U] = static_cast<std::byte>(final_count);
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto offset = slot_base + (existing_count + index) * slot_size;
        put_padded_name(payload, offset, names[index]);
        if (auto written = writer.write_be32(offset + 0x10U, 0U); !written)
            return std::unexpected{written.error()};
    }
    return {};
}

Result<OperationReport> assign_sbac_members(TransactionState &state, OperationContext context,
                                            const AssignSampleBankMembersOperation &operation,
                                            const CancellationToken &cancellation) {
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    auto found = state.partitions.find(partition_index->value);
    if (found == state.partitions.end())
        return std::unexpected{transaction_error("assign-sbac-members target is invalid")};
    auto &partition = found->second;
    auto located = category_object(state, partition, operation.volume_name, "SBAC", operation.sample_bank_name, "SBAC",
                                   cancellation);
    if (!located)
        return std::unexpected{located.error()};
    auto sample_banks =
        category_objects(state, partition, operation.volume_name, "SBAC", ObjectType::sbac, cancellation);
    if (!sample_banks)
        return std::unexpected{sample_banks.error()};
    auto memberships = sample_bank_memberships(*sample_banks);
    if (!memberships)
        return std::unexpected{memberships.error()};
    const auto target_row =
        std::ranges::find_if(*sample_banks, [&](const CategoryObject &row) { return row.id == located->second; });
    if (target_row == sample_banks->end())
        return std::unexpected{transaction_error("target Sample Bank is unreadable")};
    const auto *target_bank = std::get_if<CurrentSbac>(&target_row->decoded.payload);
    std::set<std::string> target_names;
    for (const auto &slot : target_bank->slots) {
        if (slot.active)
            target_names.insert(slot.name);
    }

    std::map<std::string, SfsId> changed_member_ids;
    std::vector<std::string> appended_names;
    for (const auto &name : operation.sample_names) {
        auto sample = category_object(state, partition, operation.volume_name, "SBNK", name, "SBNK", cancellation);
        if (!sample)
            return std::unexpected{sample.error()};
        auto sample_payload = current_payload(state, partition, sample->second, cancellation);
        if (!sample_payload)
            return std::unexpected{sample_payload.error()};
        auto decoded = decode_object(*sample_payload);
        if (!decoded)
            return std::unexpected{decoded.error()};
        const auto *current_sample = std::get_if<CurrentSbnk>(&decoded->payload);
        if (current_sample == nullptr)
            return std::unexpected{transaction_error("Sample is unreadable")};
        if (!current_sample->linked_program_numbers.empty())
            return std::unexpected{transaction_error("Sample is assigned directly to a Program")};
        const auto banked = (std::to_integer<std::uint8_t>((*sample_payload)[0xd0U]) & 1U) != 0U;
        const auto sources = memberships->find(name);
        const auto source_count = sources == memberships->end() ? 0U : sources->second.size();
        if (source_count > 1U)
            return std::unexpected{transaction_error("Sample is shared by multiple Sample Banks")};
        if (banked != (source_count == 1U))
            return std::unexpected{transaction_error("Sample membership flag disagrees with its Sample Bank")};
        if (target_names.contains(name))
            continue;
        changed_member_ids.emplace(name, sample->second);
        appended_names.push_back(name);
    }
    if (appended_names.empty())
        return std::unexpected{transaction_error("All selected Samples already belong to the target Sample Bank")};
    if (target_bank->slots.size() + appended_names.size() > maximum_sample_bank_members)
        return std::unexpected{transaction_error("Target Sample Bank would exceed 127 Samples")};

    auto target_payload = target_row->payload;
    if (auto appended = append_sbac_members_to_payload(target_payload, *target_bank, appended_names); !appended)
        return std::unexpected{appended.error()};
    if (auto detached =
            detach_members(state, partition, *sample_banks, changed_member_ids, *partition_index, cancellation);
        !detached) {
        return std::unexpected{detached.error()};
    }
    if (auto replaced =
            replace_record_payload(state, partition, located->second, std::move(target_payload), cancellation);
        !replaced) {
        if (replaced.error().message == "record payload growth exceeds its current extent capacity") {
            return std::unexpected{
                transaction_error("Target Sample Bank does not have enough allocated record capacity")};
        }
        return std::unexpected{replaced.error()};
    }
    for (const auto &[name, id] : changed_member_ids) {
        static_cast<void>(name);
        if (auto updated = set_sbnk_sample_bank_flag(state, partition, id, true, cancellation); !updated)
            return std::unexpected{updated.error()};
        state.known_edges.emplace_back(*partition_index, located->second, id);
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
