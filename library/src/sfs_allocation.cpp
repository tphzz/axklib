#include "axklib/sfs_allocation.hpp"

#include "axklib/bytes.hpp"

axk::Result<axk::detail::SfsAllocationBitmapLayout>
axk::detail::sfs_allocation_bitmap_layout(std::uint64_t partition_start_sector, std::uint32_t cluster_count,
                                          std::uint32_t sectors_per_cluster, std::uint32_t bitmap_cluster,
                                          std::uint32_t sector_size) {
    if (cluster_count == 0U || sectors_per_cluster == 0U || sector_size == 0U) {
        return std::unexpected{make_error(ErrorCode::container_invalid_geometry, ErrorCategory::container,
                                          "SFS allocation bitmap geometry contains a zero value")};
    }
    const auto cluster_bytes = checked_multiply(sector_size, sectors_per_cluster);
    if (!cluster_bytes) {
        return std::unexpected{make_error(ErrorCode::container_invalid_geometry, ErrorCategory::container,
                                          "SFS allocation bitmap geometry overflowed")};
    }
    const auto useful_bytes = (static_cast<std::uint64_t>(cluster_count) + 7U) / 8U;
    const auto rounded_numerator = checked_add(useful_bytes, *cluster_bytes - 1U);
    if (!rounded_numerator) {
        return std::unexpected{make_error(ErrorCode::container_invalid_geometry, ErrorCategory::container,
                                          "SFS allocation bitmap geometry overflowed")};
    }
    const auto span_clusters = *rounded_numerator / *cluster_bytes;
    const auto rounded_bytes = checked_multiply(span_clusters, *cluster_bytes);
    const auto partition_start = checked_multiply(partition_start_sector, sector_size);
    const auto header_relative = checked_multiply(bitmap_cluster, *cluster_bytes);
    if (!rounded_bytes || !partition_start || !header_relative || span_clusters == 0U) {
        return std::unexpected{make_error(ErrorCode::container_invalid_geometry, ErrorCategory::container,
                                          "SFS allocation bitmap offsets overflowed")};
    }
    const auto fixed_offset = checked_add(*partition_start, sfs_fixed_allocation_bitmap_offset);
    const auto header_offset = checked_add(*partition_start, *header_relative);
    if (!fixed_offset || !header_offset) {
        return std::unexpected{make_error(ErrorCode::container_invalid_geometry, ErrorCategory::container,
                                          "SFS allocation bitmap offsets overflowed")};
    }
    return SfsAllocationBitmapLayout{useful_bytes, *rounded_bytes, span_clusters, *fixed_offset, *header_offset};
}
