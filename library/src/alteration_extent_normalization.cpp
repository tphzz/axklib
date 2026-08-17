#include "alteration_internal.hpp"

#include <algorithm>
#include <limits>
#include <ranges>

namespace axk::alteration_internal {
namespace {

bool allocation_has_only_extent_byte_total_mismatches(const AllocationSummary &allocation) {
    return allocation.stored_copies_match && allocation.stored_copy_mismatch_byte_count == 0U &&
           allocation.fixed_not_header.empty() && allocation.header_not_fixed.empty() &&
           allocation.fixed_location.marked_used_without_index_extent_count == 0U &&
           allocation.fixed_location.index_extent_marked_free_count == 0U &&
           allocation.header_addressed.marked_used_without_index_extent_count == 0U &&
           allocation.header_addressed.index_extent_marked_free_count == 0U &&
           allocation.invalid_extent_record_count == 0U && allocation.extent_total_mismatch_count == 0U &&
           allocation.extent_byte_total_mismatch_count != 0U && allocation.conflicting_cluster_count == 0U;
}

std::uint64_t extent_byte_total(const IndexRecord &record) {
    return std::ranges::fold_left(record.extents, std::uint64_t{},
                                  [](std::uint64_t total, const Extent &extent) { return total + extent.byte_count; });
}

} // namespace

bool placement_repair_can_normalize_directory_extents(const Partition &partition) {
    if (!allocation_has_only_extent_byte_total_mismatches(partition.allocation))
        return false;
    std::uint32_t mismatch_count{};
    for (const auto &record : partition.records) {
        const auto byte_total = extent_byte_total(record);
        if (byte_total == record.data_size)
            continue;
        ++mismatch_count;
        if (record.payload_kind != PayloadKind::directory)
            return false;
        const auto recovered_size = std::max<std::uint64_t>(record.data_size, byte_total);
        if (recovered_size > std::numeric_limits<std::size_t>::max())
            return false;
        auto extents = record.extents;
        if (!normalize_extent_byte_counts(extents, static_cast<std::size_t>(recovered_size)))
            return false;
    }
    return mismatch_count == partition.allocation.extent_byte_total_mismatch_count;
}

Result<void> stage_recoverable_directory_extent_repairs(const RandomAccessReader &source, const Partition &partition,
                                                        MutablePartition &mutable_state,
                                                        const CancellationToken &cancellation) {
    for (const auto &record : partition.records) {
        const auto byte_total = extent_byte_total(record);
        if (byte_total == record.data_size)
            continue;
        const auto recovered_size = std::max<std::uint64_t>(record.data_size, byte_total);
        if (record.payload_kind != PayloadKind::directory || recovered_size > std::numeric_limits<std::size_t>::max()) {
            return std::unexpected{transaction_error("SFS extent mismatch is not a recoverable directory")};
        }
        std::vector<std::byte> payload;
        payload.reserve(static_cast<std::size_t>(recovered_size));
        for (const auto &extent : record.extents) {
            if (const auto checked = cancellation.check(); !checked)
                return std::unexpected{checked.error()};
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
                static_cast<std::uint64_t>(extent.cluster_count) * 1024U, recovered_size - payload.size()));
            const auto offset = (static_cast<std::uint64_t>(partition.start_sector) +
                                 static_cast<std::uint64_t>(extent.cluster_offset) * partition.sectors_per_cluster) *
                                512U;
            auto bytes = read_raw(source, offset, count);
            if (!bytes)
                return std::unexpected{bytes.error()};
            payload.insert(payload.end(), bytes->begin(), bytes->end());
            if (payload.size() == recovered_size)
                break;
        }
        if (payload.size() != recovered_size)
            return std::unexpected{transaction_error("SFS directory extents do not contain the declared payload")};
        auto entries = parse_directory(payload, record.sfs_id);
        if (!entries || entries->size() * 32U != payload.size()) {
            return std::unexpected{
                transaction_error("mismatched SFS directory payload is not structurally recoverable")};
        }
        auto raw = read_raw(source, record.record_offset.value, 72U);
        if (!raw)
            return std::unexpected{raw.error()};
        mutable_state.changed.emplace(
            record.sfs_id,
            MutablePartition::InsertedRecord{record.sfs_id, std::move(*raw), std::move(payload), record.extents,
                                             record.continuation_clusters, record.payload_kind, true});
    }
    return {};
}

} // namespace axk::alteration_internal
