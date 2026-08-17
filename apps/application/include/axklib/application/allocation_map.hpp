#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/sfs.hpp"

namespace axk::app {

struct AllocationObjectIdentity {
    std::string object_id;
    std::string object_type;
    std::string object_name;
    std::string volume_name;
    std::string category_name;
};

struct ImageAllocationOwner {
    std::string claim_kind;
    std::optional<std::uint32_t> sfs_id;
    std::optional<std::uint32_t> extent_index;
    std::string record_kind;
    std::optional<std::string> object_id;
    std::string object_type;
    std::string object_name;
    std::string volume_name;
    std::string category_name;

    bool operator==(const ImageAllocationOwner &) const = default;
};

struct ImageAllocationRun {
    std::uint32_t start_cluster{};
    std::uint32_t cluster_count{};
    std::uint64_t start_sector{};
    std::uint64_t sector_count{};
    std::uint64_t byte_offset{};
    std::uint64_t byte_count{};
    bool fixed_bitmap_used{};
    bool header_bitmap_used{};
    bool reconstructed_used{};
    std::string allocation_kind;
    std::vector<ImageAllocationOwner> owners;
    std::vector<std::string> consistency_flags;
};

struct ImageAllocationSummary {
    std::uint32_t total_clusters{};
    std::uint32_t reserved_clusters{};
    std::uint32_t data_clusters{};
    std::uint32_t continuation_clusters{};
    std::uint32_t free_clusters{};
    std::uint32_t free_run_count{};
    std::uint32_t largest_free_run_clusters{};
    std::uint32_t allocated_clusters{};
    std::uint64_t allocated_bytes{};
    std::uint64_t logical_record_bytes{};
    std::uint64_t data_slack_bytes{};
    std::uint32_t record_count{};
    std::uint32_t fragmented_record_count{};
    std::uint32_t total_extent_count{};
    std::uint32_t maximum_extent_count{};
    std::uint32_t bitmap_copy_mismatch_clusters{};
    std::uint32_t claimed_but_free_clusters{};
    std::uint32_t used_without_claim_clusters{};
    std::uint32_t conflicting_clusters{};
    std::uint32_t invalid_extent_records{};
};

struct ImageAllocationMap {
    std::uint8_t partition_index{};
    std::string partition_name;
    std::uint32_t sector_size_bytes{};
    std::uint32_t sectors_per_cluster{};
    std::uint32_t cluster_size_bytes{};
    std::uint32_t cluster_count{};
    std::uint64_t partition_start_sector{};
    ImageAllocationSummary summary;
    std::vector<ImageAllocationRun> runs;
};

[[nodiscard]] Result<ImageAllocationMap>
build_image_allocation_map(const Partition &partition,
                           const std::unordered_map<std::uint32_t, AllocationObjectIdentity> &objects_by_sfs_id,
                           std::uint32_t sector_size_bytes);

} // namespace axk::app
