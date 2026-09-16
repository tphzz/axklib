#include "sfs_internal.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>

#include "axklib/bytes.hpp"

namespace axk::sfs_detail {

Result<detail::SfsAllocationBitmapLayout> read_partition_geometry(const RandomAccessReader &image, Partition &partition,
                                                                  std::uint32_t sector_size,
                                                                  const OpenOptions &options) {
    const auto start = checked_multiply(partition.start_sector, sector_size);
    if (!start)
        return std::unexpected{start.error()};
    const auto fail = [&](std::string message, std::uint64_t offset) -> Error {
        return partition_error(ErrorCode::container_invalid_geometry, std::move(message), partition.index,
                               *start + offset);
    };
    const auto header = read_bytes(image, *start, sector_size, options.cancellation);
    if (!header)
        return std::unexpected{header.error()};
    if (header->size() < 512U || !begins_with(*header, "YAMAHA_dev3"))
        return std::unexpected{fail("partition header does not contain complete Yamaha SFS metadata", 0U)};
    const ByteReader reader{*header};
    const auto name = reader.ascii_field(0x40, 16);
    const auto sectors_per_cluster = reader.be32(0x80);
    const auto cluster_count = reader.be32(0x90);
    const auto active = reader.be32(0x94);
    const auto first = reader.be32(0x98);
    const auto second = reader.be32(0x9c);
    const auto index_cluster = reader.be32(0xa4);
    const auto index_span = reader.be32(0xa8);
    if (!name || !sectors_per_cluster || !cluster_count || !active || !first || !second || !index_cluster ||
        !index_span || *sectors_per_cluster == 0U || *cluster_count == 0U || *index_span == 0U)
        return std::unexpected{fail("partition contains incomplete or zero SFS geometry", 0x80U)};
    partition.name = *name;
    partition.sectors_per_cluster = *sectors_per_cluster;
    partition.large_allocation_unit_clusters = reader.be32(0x84).value();
    partition.cluster_count = *cluster_count;
    partition.active_bitmap_cluster = *active;
    partition.directory_index_cluster = *index_cluster;
    partition.directory_index_span_clusters = *index_span;
    std::copy_n(header->begin() + 0xac, partition.unresolved_header_tail.size(),
                partition.unresolved_header_tail.begin());

    // Negative copy locations retain their address but mark that copy unavailable.
    const auto location = [](std::uint32_t raw) -> std::uint32_t {
        const auto signed_value = std::bit_cast<std::int32_t>(raw);
        return static_cast<std::uint32_t>(signed_value < 0 ? -static_cast<std::int64_t>(signed_value) : signed_value);
    };
    partition.bitmap_copy1_cluster = location(*first);
    partition.bitmap_copy2_cluster = location(*second);
    partition.allocation.bitmap_copy1_valid = std::bit_cast<std::int32_t>(*first) > 0;
    partition.allocation.bitmap_copy2_valid = std::bit_cast<std::int32_t>(*second) > 0;
    partition.allocation.active_bitmap_copy =
        *active == partition.bitmap_copy1_cluster && partition.allocation.bitmap_copy1_valid   ? 1U
        : *active == partition.bitmap_copy2_cluster && partition.allocation.bitmap_copy2_valid ? 2U
                                                                                               : 0U;
    if (partition.allocation.active_bitmap_copy == 0U || !partition.allocation.bitmap_copy1_valid ||
        !partition.allocation.bitmap_copy2_valid)
        partition.diagnostics.push_back(
            fail("partition allocation bitmap selection or copy state is unavailable", 0x94U));

    if (partition.cluster_count > static_cast<std::uint64_t>(partition.sector_count) / partition.sectors_per_cluster)
        return std::unexpected{fail("partition cluster count exceeds its physical sector capacity", 0x90U)};
    const auto cluster_bytes = checked_multiply(sector_size, partition.sectors_per_cluster);
    if (!cluster_bytes)
        return std::unexpected{cluster_bytes.error()};
    const auto index_bytes = checked_multiply(*cluster_bytes, partition.directory_index_span_clusters);
    if (!index_bytes || *index_bytes > options.max_index_bytes ||
        *index_bytes > std::numeric_limits<std::size_t>::max())
        return std::unexpected{fail("partition index span exceeds configured bounds", 0xa8U)};
    const auto layout = detail::sfs_allocation_bitmap_layout(
        partition.start_sector, partition.cluster_count, partition.sectors_per_cluster, partition.bitmap_copy1_cluster,
        partition.bitmap_copy2_cluster, sector_size);
    if (!layout || layout->rounded_bytes > options.max_allocation_bitmap_bytes ||
        layout->rounded_bytes > std::numeric_limits<std::size_t>::max())
        return std::unexpected{fail("partition bitmap exceeds the configured memory bound", 0x98U)};
    const auto index_end =
        static_cast<std::uint64_t>(partition.directory_index_cluster) + partition.directory_index_span_clusters;
    const auto first_end = static_cast<std::uint64_t>(partition.bitmap_copy1_cluster) + layout->span_clusters;
    const auto second_end = static_cast<std::uint64_t>(partition.bitmap_copy2_cluster) + layout->span_clusters;
    const auto disjoint = [](std::uint64_t a, std::uint64_t end_a, std::uint64_t b, std::uint64_t end_b) {
        return end_a <= b || end_b <= a;
    };
    if (partition.bitmap_copy1_cluster < 2U || partition.bitmap_copy2_cluster < 2U ||
        partition.directory_index_cluster < 2U || first_end > partition.cluster_count ||
        second_end > partition.cluster_count || index_end > partition.cluster_count ||
        first_end > partition.directory_index_cluster || second_end > partition.directory_index_cluster ||
        !disjoint(partition.bitmap_copy1_cluster, first_end, partition.bitmap_copy2_cluster, second_end) ||
        !disjoint(partition.bitmap_copy1_cluster, first_end, partition.directory_index_cluster, index_end) ||
        !disjoint(partition.bitmap_copy2_cluster, second_end, partition.directory_index_cluster, index_end))
        return std::unexpected{fail("partition bitmap/index regions overlap or exceed the cluster range", 0x98U)};

    const auto backup_offset = checked_add(*start, *cluster_bytes);
    if (!backup_offset)
        return std::unexpected{backup_offset.error()};
    const auto backup = read_bytes(image, *backup_offset, sector_size, options.cancellation);
    partition.backup_header_matches = backup && *backup == *header;
    if (!partition.backup_header_matches)
        partition.diagnostics.push_back(partition_error(
            ErrorCode::container_backup_mismatch, "backup partition header differs from primary or is unavailable",
            partition.index, *backup_offset));
    return *layout;
}

} // namespace axk::sfs_detail
