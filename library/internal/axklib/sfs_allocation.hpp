#pragma once

#include <cstdint>

#include "axklib/error.hpp"

namespace axk::detail {

inline constexpr std::uint32_t sfs_initial_bitmap_cluster = 2U;

struct SfsAllocationBitmapLayout {
    std::uint64_t useful_bytes{};
    std::uint64_t rounded_bytes{};
    std::uint64_t span_clusters{};
    std::uint64_t bitmap_copy1_offset{};
    std::uint64_t bitmap_copy2_offset{};
};

[[nodiscard]] Result<SfsAllocationBitmapLayout>
sfs_allocation_bitmap_layout(std::uint64_t partition_start_sector, std::uint32_t cluster_count,
                             std::uint32_t sectors_per_cluster, std::uint32_t bitmap_copy1_cluster,
                             std::uint32_t bitmap_copy2_cluster, std::uint32_t sector_size = 512U);

} // namespace axk::detail
