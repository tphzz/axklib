#include "alteration_internal.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <string_view>

namespace axk::alteration_internal {
namespace {

bool repairable_object_type(std::string_view type) {
    constexpr std::array types{std::string_view{"SMPL"}, std::string_view{"SBNK"}, std::string_view{"SBAC"},
                               std::string_view{"PROG"}, std::string_view{"SEQU"}, std::string_view{"PRF3"}};
    return std::ranges::contains(types, type);
}

Result<std::vector<std::byte>> recover_allocated_directory_payload(TransactionState &state, MutablePartition &partition,
                                                                   SfsId directory,
                                                                   const CancellationToken &cancellation) {
    auto payload = current_payload(state, partition, directory, cancellation);
    if (payload)
        return payload;
    const auto source = record(*partition.source, directory);
    if (payload.error().code != ErrorCode::io_short_read || source == nullptr ||
        source->payload_kind != PayloadKind::directory) {
        return std::unexpected{payload.error()};
    }
    std::uint64_t encoded_bytes{};
    std::uint64_t capacity{};
    for (const auto &extent : source->extents) {
        encoded_bytes += extent.byte_count;
        capacity += static_cast<std::uint64_t>(extent.cluster_count) * 1024U;
    }
    if (encoded_bytes >= source->data_size || capacity < source->data_size)
        return std::unexpected{payload.error()};

    std::vector<std::byte> recovered;
    recovered.reserve(source->data_size);
    const auto sector_size = state.container.superblock().sector_size_bytes;
    const auto cluster_bytes = static_cast<std::uint64_t>(sector_size) * partition.source->sectors_per_cluster;
    for (const auto &extent : source->extents) {
        if (const auto checked = cancellation.check(); !checked)
            return std::unexpected{checked.error()};
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
            static_cast<std::uint64_t>(extent.cluster_count) * cluster_bytes, source->data_size - recovered.size()));
        const auto offset = static_cast<std::uint64_t>(partition.source->start_sector) * sector_size +
                            static_cast<std::uint64_t>(extent.cluster_offset) * cluster_bytes;
        auto bytes = read_raw(*state.source, offset, count);
        if (!bytes)
            return std::unexpected{bytes.error()};
        recovered.insert(recovered.end(), bytes->begin(), bytes->end());
        if (recovered.size() == source->data_size)
            break;
    }
    auto entries = parse_directory(recovered, directory);
    if (!entries || entries->size() * 32U != recovered.size()) {
        return std::unexpected{transaction_error(
            std::format("SFS ID {} allocated bytes do not form one complete recoverable directory", directory.value))};
    }
    auto raw = read_raw(*state.source, source->record_offset.value, 72U);
    if (!raw)
        return std::unexpected{raw.error()};
    partition.changed.emplace(directory,
                              MutablePartition::InsertedRecord{directory, std::move(*raw), recovered, source->extents,
                                                               source->continuation_clusters, source->payload_kind});
    if (auto normalized = replace_record_payload(state, partition, directory, recovered, cancellation); !normalized)
        return std::unexpected{normalized.error()};
    return recovered;
}

} // namespace

Result<void> ensure_directory_entry_for_repair(TransactionState &state, MutablePartition &partition, SfsId directory,
                                               SfsId child, std::string_view name,
                                               const CancellationToken &cancellation) {
    auto payload = recover_allocated_directory_payload(state, partition, directory, cancellation);
    if (!payload)
        return std::unexpected{payload.error()};
    auto entries = parse_directory(*payload, directory);
    if (!entries)
        return std::unexpected{entries.error()};
    const auto exact = std::ranges::count_if(
        *entries, [&](const ParsedDirectoryEntry &entry) { return entry.id == child && entry.name == name; });
    if (exact == 1U)
        return {};
    if (exact != 0U || std::ranges::any_of(*entries, [&](const ParsedDirectoryEntry &entry) {
            return entry.id == child || entry.name == name;
        })) {
        return std::unexpected{transaction_error("placement repair directory entry conflicts with recovered data")};
    }
    return append_directory_entry(state, partition, directory, child, name, cancellation);
}

Result<OperationReport> repair_object_placements(TransactionState &state, OperationContext context,
                                                 const RepairObjectPlacementsOperation &operation,
                                                 const CancellationToken &cancellation) {
    if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
    auto partition_index = resolve_partition(state, operation.partition);
    if (!partition_index)
        return std::unexpected{partition_index.error()};
    const auto partition_state = state.partitions.find(partition_index->value);
    if (partition_state == state.partitions.end())
        return std::unexpected{transaction_error("partition index does not exist")};
    auto &partition = partition_state->second;

    for (const auto id : operation.object_sfs_ids) {
        const auto object = std::ranges::find_if(state.catalog.objects, [&](const ObjectSnapshot &candidate) {
            return candidate.partition == *partition_index && candidate.sfs_id == id;
        });
        if (object == state.catalog.objects.end()) {
            return std::unexpected{transaction_error(
                std::format("partition {} SFS ID {} is not a decoded object", partition_index->value, id.value))};
        }
        if (object->placement_resolution != PlacementResolution::missing || object->placement ||
            !object->placement_candidates.empty()) {
            return std::unexpected{
                transaction_error(std::format("partition {} SFS ID {} does not have exactly one missing placement",
                                              partition_index->value, id.value))};
        }
        if (!repairable_object_type(object->object.header.raw_type)) {
            return std::unexpected{transaction_error("object type is not supported by placement repair")};
        }
        const auto *physical = record(*partition.source, id);
        if (physical == nullptr || physical->payload_kind != PayloadKind::object) {
            return std::unexpected{transaction_error("placement repair requires an existing physical object record")};
        }
        auto category =
            volume_category(state, partition, operation.volume_name, object->object.header.raw_type, cancellation);
        if (!category)
            return std::unexpected{category.error()};
        if (auto appended = ensure_directory_entry_for_repair(state, partition, *category, id,
                                                              object->object.header.name, cancellation);
            !appended) {
            return std::unexpected{appended.error()};
        }
    }

    OperationReport report;
    report.id = context.id;
    report.type = context.type;
    report.partition = *partition_index;
    report.volume_name = operation.volume_name;
    report.placed_sfs_ids = operation.object_sfs_ids;
    return report;
}

} // namespace axk::alteration_internal
