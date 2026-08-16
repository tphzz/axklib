#include <gtest/gtest.h>

#include <numeric>

#include "axklib/application/allocation_map.hpp"

TEST(ImageAllocationMap, ProducesExhaustiveRunsAndReportsScatteredData) {
    axk::Partition partition;
    partition.index = axk::PartitionIndex{1U};
    partition.name = "Test";
    partition.start_sector = 100U;
    partition.cluster_count = 12U;
    partition.sectors_per_cluster = 2U;
    partition.directory_index_cluster = 2U;
    partition.directory_index_span_clusters = 1U;
    partition.allocation.fixed_location.used_cluster_ranges = {{3U, 4U}, {8U, 8U}};
    partition.allocation.header_addressed.used_cluster_ranges = partition.allocation.fixed_location.used_cluster_ranges;
    axk::IndexRecord record;
    record.sfs_id = axk::SfsId{7U};
    record.data_size = 2'500U;
    record.extents = {{3U, 2U, 2'000U}, {8U, 1U, 500U}};
    record.payload_kind = axk::PayloadKind::object;
    record.object_type = "SMPL";
    record.object_name = "Wave";
    partition.records.push_back(record);

    const auto map = axk::app::build_image_allocation_map(
        partition, {{7U, {"object-7", "SMPL", "Wave", "Volume", "Wave Data"}}}, 512U);

    ASSERT_TRUE(map) << map.error().message;
    EXPECT_EQ(map->summary.total_clusters, 12U);
    EXPECT_EQ(map->summary.reserved_clusters, 3U);
    EXPECT_EQ(map->summary.allocated_clusters, 3U);
    EXPECT_EQ(map->summary.allocated_bytes, 3'072U);
    EXPECT_EQ(map->summary.fragmented_record_count, 1U);
    EXPECT_EQ(map->summary.free_clusters, 6U);
    EXPECT_EQ(map->summary.free_run_count, 2U);
    EXPECT_EQ(map->summary.largest_free_run_clusters, 3U);
    EXPECT_EQ(map->summary.claimed_but_free_clusters, 0U);
    EXPECT_EQ(map->summary.used_without_claim_clusters, 0U);
    EXPECT_EQ(map->summary.data_slack_bytes, 572U);
    EXPECT_EQ(map->summary.reserved_clusters + map->summary.allocated_clusters + map->summary.free_clusters,
              map->summary.total_clusters);
    EXPECT_EQ(std::accumulate(map->runs.begin(), map->runs.end(), 0U,
                              [](auto total, const auto &run) { return total + run.cluster_count; }),
              partition.cluster_count);
    ASSERT_FALSE(map->runs.empty());
    EXPECT_EQ(map->runs.front().start_cluster, 0U);
    EXPECT_EQ(map->runs.front().allocation_kind, "RESERVED");
    EXPECT_FALSE(map->runs.front().reconstructed_used);
    EXPECT_TRUE(map->runs.front().consistency_flags.empty());
    EXPECT_EQ(map->runs.back().start_cluster + map->runs.back().cluster_count, partition.cluster_count);
    const auto data = std::ranges::find(map->runs, "DATA", &axk::app::ImageAllocationRun::allocation_kind);
    ASSERT_NE(data, map->runs.end());
    ASSERT_EQ(data->owners.size(), 1U);
    EXPECT_EQ(data->owners.front().object_id, "object-7");
}

TEST(ImageAllocationMap, ReportsOnlyPayloadAndContinuationClaimsMissingFromTheBitmap) {
    axk::Partition partition;
    partition.index = axk::PartitionIndex{0U};
    partition.cluster_count = 8U;
    partition.sectors_per_cluster = 2U;
    partition.directory_index_cluster = 1U;
    partition.directory_index_span_clusters = 1U;
    axk::IndexRecord record;
    record.sfs_id = axk::SfsId{4U};
    record.payload_kind = axk::PayloadKind::object;
    record.continuation_clusters = {5U};
    record.extents = {{3U, 1U, 512U}};
    partition.records = {record};

    const auto map = axk::app::build_image_allocation_map(partition, {}, 512U);

    ASSERT_TRUE(map);
    EXPECT_EQ(map->summary.claimed_but_free_clusters, 2U);
    const auto reserved = std::ranges::find(map->runs, "RESERVED", &axk::app::ImageAllocationRun::allocation_kind);
    ASSERT_NE(reserved, map->runs.end());
    EXPECT_TRUE(reserved->consistency_flags.empty());
    const auto data = std::ranges::find(map->runs, "DATA", &axk::app::ImageAllocationRun::allocation_kind);
    ASSERT_NE(data, map->runs.end());
    EXPECT_EQ(data->consistency_flags, std::vector<std::string>{"CLAIMED_BUT_FREE"});
    const auto continuation =
        std::ranges::find(map->runs, "CONTINUATION", &axk::app::ImageAllocationRun::allocation_kind);
    ASSERT_NE(continuation, map->runs.end());
    EXPECT_EQ(continuation->consistency_flags, std::vector<std::string>{"CLAIMED_BUT_FREE"});
}

TEST(ImageAllocationMap, IdentifiesFilesystemDirectoriesAndSupportFiles) {
    axk::Partition partition;
    partition.index = axk::PartitionIndex{0U};
    partition.cluster_count = 10U;
    partition.sectors_per_cluster = 2U;
    partition.directory_index_cluster = 1U;
    partition.directory_index_span_clusters = 1U;
    partition.allocation.fixed_location.used_cluster_ranges = {{2U, 4U}};
    partition.allocation.header_addressed.used_cluster_ranges = partition.allocation.fixed_location.used_cluster_ranges;

    axk::IndexRecord support;
    support.sfs_id = axk::SfsId{0U};
    support.extents = {{2U, 2U, 2'048U}};

    axk::IndexRecord root;
    root.sfs_id = axk::SfsId{1U};
    root.payload_kind = axk::PayloadKind::directory;
    root.directory_id = axk::LinkId{1U};
    root.parent_directory_id = axk::LinkId{1U};
    root.directory_entries = {{0x20U, axk::LinkId{0U}, "sfserram", 0U}};
    root.extents = {{4U, 1U, 128U}};
    partition.records = {support, root};

    const auto map = axk::app::build_image_allocation_map(partition, {}, 512U);

    ASSERT_TRUE(map) << map.error().message;
    const auto support_run = std::ranges::find(map->runs, "SUPPORT", &axk::app::ImageAllocationRun::allocation_kind);
    ASSERT_NE(support_run, map->runs.end());
    ASSERT_EQ(support_run->owners.size(), 1U);
    EXPECT_EQ(support_run->owners.front().record_kind, "SUPPORT");
    EXPECT_EQ(support_run->owners.front().object_name, "sfserram");

    const auto directory_run =
        std::ranges::find(map->runs, "DIRECTORY", &axk::app::ImageAllocationRun::allocation_kind);
    ASSERT_NE(directory_run, map->runs.end());
    ASSERT_EQ(directory_run->owners.size(), 1U);
    EXPECT_EQ(directory_run->owners.front().record_kind, "DIRECTORY");
    EXPECT_EQ(directory_run->owners.front().object_name, "/");
    EXPECT_EQ(map->summary.claimed_but_free_clusters, 0U);
    EXPECT_EQ(map->summary.used_without_claim_clusters, 0U);
}

TEST(ImageAllocationMap, DistinguishesBitmapMismatchesAndMultipleClaims) {
    axk::Partition partition;
    partition.index = axk::PartitionIndex{0U};
    partition.cluster_count = 6U;
    partition.sectors_per_cluster = 2U;
    partition.directory_index_cluster = 1U;
    partition.directory_index_span_clusters = 1U;
    partition.allocation.fixed_location.used_cluster_ranges = {{0U, 2U}};
    partition.allocation.header_addressed.used_cluster_ranges = {{0U, 1U}, {3U, 3U}};
    axk::IndexRecord first;
    first.sfs_id = axk::SfsId{1U};
    first.extents = {{2U, 1U, 512U}};
    axk::IndexRecord second;
    second.sfs_id = axk::SfsId{2U};
    second.extents = {{2U, 1U, 512U}};
    partition.records = {first, second};

    const auto map = axk::app::build_image_allocation_map(partition, {}, 512U);

    ASSERT_TRUE(map);
    EXPECT_EQ(map->summary.bitmap_copy_mismatch_clusters, 2U);
    EXPECT_EQ(map->summary.claimed_but_free_clusters, 1U);
    EXPECT_EQ(map->summary.used_without_claim_clusters, 3U);
    EXPECT_EQ(map->summary.conflicting_clusters, 1U);
}
