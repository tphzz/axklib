#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/io.hpp"
#include "axklib/sfs.hpp"

namespace {

constexpr std::uint32_t sector_bytes = 512U;
constexpr std::uint32_t partition_sector = 3U;
constexpr std::uint32_t partition_clusters = 72U;
constexpr std::uint32_t index_cluster = 4U;
constexpr std::uint32_t index_clusters = 2U;
constexpr std::uint32_t payload_cluster = index_cluster + index_clusters;
constexpr std::size_t record_bytes = 72U;

void ascii(std::span<std::byte> bytes, std::size_t offset, std::string_view text) {
    std::transform(text.begin(), text.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                   [](char value) { return static_cast<std::byte>(value); });
}

std::vector<std::byte> file_payload(std::uint32_t id, std::size_t size) {
    std::vector<std::byte> result(size);
    for (std::size_t index = 0; index < size; ++index)
        result[index] = static_cast<std::byte>((index + id) % 251U);
    return result;
}

std::vector<std::byte> geometry_fixture(std::uint32_t sectors_per_cluster, std::uint32_t active_bitmap,
                                        std::uint32_t record_count) {
    const auto cluster_bytes = sectors_per_cluster * sector_bytes;
    const auto sectors = partition_sector + partition_clusters * sectors_per_cluster;
    const auto partition = partition_sector * sector_bytes;
    std::vector<std::byte> bytes(static_cast<std::size_t>(sectors) * sector_bytes);
    axk::ByteWriter writer{bytes};
    for (const std::size_t offset : {0U, sector_bytes}) {
        ascii(bytes, offset, "YAMAHA_dev3");
        EXPECT_TRUE(writer.write_be32(offset + 0x9cU, sector_bytes));
        EXPECT_TRUE(writer.write_be32(offset + 0xa0U, sectors));
        EXPECT_TRUE(writer.write_be32(offset + 0xa8U, partition_sector));
        EXPECT_TRUE(writer.write_be32(offset + 0xacU, sectors - partition_sector));
    }
    ascii(bytes, partition, "YAMAHA_dev3");
    ascii(bytes, partition + 0x40U, "Geometry");
    EXPECT_TRUE(writer.write_be32(partition + 0x80U, sectors_per_cluster));
    EXPECT_TRUE(writer.write_be32(partition + 0x90U, partition_clusters));
    EXPECT_TRUE(writer.write_be32(partition + 0x94U, active_bitmap));
    EXPECT_TRUE(writer.write_be32(partition + 0x98U, 2U));
    EXPECT_TRUE(writer.write_be32(partition + 0x9cU, 3U));
    const auto records_per_page = cluster_bytes / record_bytes;
    EXPECT_TRUE(writer.write_be32(partition + 0xa0U, static_cast<std::uint32_t>(records_per_page * index_clusters)));
    EXPECT_TRUE(writer.write_be32(partition + 0xa4U, index_cluster));
    EXPECT_TRUE(writer.write_be32(partition + 0xa8U, index_clusters));
    std::copy_n(bytes.begin() + partition, 1024U, bytes.begin() + partition + cluster_bytes);

    const auto mark_allocated = [&](std::uint32_t cluster) {
        for (const auto copy : {2U, 3U})
            bytes[partition + copy * cluster_bytes + cluster / 8U] |= static_cast<std::byte>(0x80U >> (cluster & 7U));
    };
    const auto directory_entry = [&](std::size_t offset, std::string_view name, std::uint32_t id) {
        EXPECT_TRUE(writer.write_be16(offset, 0x20U));
        EXPECT_TRUE(writer.write_be16(offset + 2U, static_cast<std::uint16_t>(name.size() + 1U)));
        EXPECT_TRUE(writer.write_be32(offset + 4U, id));
        ascii(bytes, offset + 8U, name);
    };
    const auto root = partition + payload_cluster * cluster_bytes;
    directory_entry(root, ".", 0U);
    directory_entry(root + 32U, "..", 0U);
    for (std::uint32_t id = 0U; id < record_count; ++id) {
        const auto record = partition + index_cluster * cluster_bytes + (id / records_per_page) * cluster_bytes +
                            (id % records_per_page) * record_bytes;
        const bool fragmented = id == record_count - 1U;
        const auto size = id == 0U ? (record_count + 1U) * 32U : fragmented ? cluster_bytes + 17U : 37U;
        const auto cluster = payload_cluster + id;
        EXPECT_TRUE(writer.write_be16(record, fragmented ? 2U : 1U));
        EXPECT_TRUE(writer.write_be16(record + 4U, fragmented ? 2U : 1U));
        EXPECT_TRUE(writer.write_be32(record + 6U, size));
        EXPECT_TRUE(writer.write_be32(record + 0x0aU, cluster));
        EXPECT_TRUE(writer.write_be32(record + 0x0eU, 1U));
        EXPECT_TRUE(writer.write_be32(record + 0x12U, fragmented ? cluster_bytes : size));
        mark_allocated(cluster);
        if (id == 0U)
            continue;
        directory_entry(root + (id + 1U) * 32U, "FILE" + std::to_string(id), id);
        const auto payload = file_payload(id, size);
        const auto first_size = fragmented ? cluster_bytes : size;
        std::copy_n(payload.begin(), first_size, bytes.begin() + partition + cluster * cluster_bytes);
        if (fragmented) {
            const auto second_cluster = cluster + 2U;
            EXPECT_TRUE(writer.write_be32(record + 0x16U, second_cluster));
            EXPECT_TRUE(writer.write_be32(record + 0x1aU, 1U));
            EXPECT_TRUE(writer.write_be32(record + 0x1eU, 17U));
            std::copy(payload.begin() + first_size, payload.end(),
                      bytes.begin() + partition + second_cluster * cluster_bytes);
            mark_allocated(second_cluster);
        }
    }
    return bytes;
}

void replace_header_word(std::vector<std::byte> &bytes, std::size_t offset, std::uint32_t value) {
    axk::ByteWriter writer{bytes};
    constexpr auto partition = partition_sector * sector_bytes;
    for (const auto header : {partition, partition + 1024U})
        ASSERT_TRUE(writer.write_be32(header + offset, value));
}

void expect_invalid_geometry(std::vector<std::byte> bytes) {
    const auto image = axk::open_image(std::make_shared<axk::MemoryReader>(std::move(bytes)), "invalid-geometry.hds");
    if (!image) {
        EXPECT_EQ(image.error().code, axk::ErrorCode::container_invalid_geometry);
        return;
    }
    EXPECT_TRUE(image->partitions().empty());
    EXPECT_TRUE(std::ranges::any_of(image->diagnostics(), [](const axk::Error &error) {
        return error.code == axk::ErrorCode::container_invalid_geometry;
    }));
}

TEST(SfsGeometry, ReadsFourKiBIndexPagesWithFiftySixRecordsPerPage) {
    const auto image =
        axk::open_image(std::make_shared<axk::MemoryReader>(geometry_fixture(8U, 2U, 57U)), "four-kib-clusters.hds");
    ASSERT_TRUE(image) << image.error().message;
    ASSERT_EQ(image->partitions().size(), 1U);
    const auto &partition = image->partitions().front();
    EXPECT_EQ(partition.sectors_per_cluster, 8U);
    EXPECT_TRUE(partition.backup_header_matches);
    ASSERT_EQ(partition.records.size(), 57U);
    for (std::uint32_t id = 0U; id < 57U; ++id)
        EXPECT_EQ(partition.records[id].sfs_id.value, id);
    EXPECT_EQ(partition.records.back().record_offset.value,
              static_cast<std::uint64_t>(partition_sector * sector_bytes + 5U * 4096U));
    EXPECT_EQ(partition.records.front().payload_kind, axk::PayloadKind::directory);
    ASSERT_EQ(partition.records.front().directory_entries.size(), 58U);
    EXPECT_EQ(partition.records.front().directory_entries.back().name, "FILE56");
    EXPECT_EQ(partition.allocation.reconstructed_used_cluster_count, 58U);
    EXPECT_TRUE(partition.allocation.stored_copies_match);
    EXPECT_TRUE(axk::allocation_is_safe_for_mutation(partition.allocation));
}

TEST(SfsGeometry, ReadsExactDataAndRangesAcrossFragmentedFourKiBExtents) {
    const auto image =
        axk::open_image(std::make_shared<axk::MemoryReader>(geometry_fixture(8U, 2U, 57U)), "four-kib-fragmented.hds");
    ASSERT_TRUE(image) << image.error().message;
    for (std::uint32_t id = 1U; id < 57U; ++id) {
        const auto expected = file_payload(id, id == 56U ? 4113U : 37U);
        const auto payload = image->read_record_data(axk::PartitionIndex{0U}, axk::SfsId{id}, expected.size());
        ASSERT_TRUE(payload) << "record=" << id << ": " << payload.error().message;
        EXPECT_EQ(*payload, expected);
    }
    const auto range = image->read_record_range(axk::PartitionIndex{0U}, axk::SfsId{56U}, 4090U, 23U);
    ASSERT_TRUE(range) << range.error().message;
    const auto payload = file_payload(56U, 4113U);
    EXPECT_TRUE(std::ranges::equal(*range, std::span{payload}.subspan(4090U, 23U)));
}

TEST(SfsGeometry, ActiveSecondBitmapDoesNotChangeOneKiBClusterGeometry) {
    const auto image =
        axk::open_image(std::make_shared<axk::MemoryReader>(geometry_fixture(2U, 3U, 2U)), "second-bitmap-active.hds");
    ASSERT_TRUE(image) << image.error().message;
    ASSERT_EQ(image->partitions().size(), 1U);
    const auto &partition = image->partitions().front();
    EXPECT_EQ(partition.sectors_per_cluster, 2U);
    EXPECT_TRUE(partition.backup_header_matches);
    ASSERT_EQ(partition.records.size(), 2U);
    EXPECT_EQ(partition.records.back().sfs_id.value, 1U);
    const auto payload = image->read_record_data(partition.index, axk::SfsId{1U}, 1041U);
    ASSERT_TRUE(payload) << payload.error().message;
    EXPECT_EQ(*payload, file_payload(1U, 1041U));
    EXPECT_TRUE(partition.allocation.stored_copies_match);
    EXPECT_TRUE(axk::allocation_is_safe_for_mutation(partition.allocation));
}

TEST(SfsGeometry, RejectsZeroGeometryInsteadOfUsingTheActiveBitmapAsGeometry) {
    auto bytes = geometry_fixture(2U, 2U, 2U);
    replace_header_word(bytes, 0x80U, 0U);
    expect_invalid_geometry(std::move(bytes));
}

TEST(SfsGeometry, RejectsBitmapOutsideTheReservedMetadataPrefix) {
    auto bytes = geometry_fixture(2U, 2U, 2U);
    replace_header_word(bytes, 0x9cU, 70U);
    expect_invalid_geometry(std::move(bytes));
}

TEST(SfsGeometry, RejectsGeometryExceedingThePartitionCapacity) {
    auto bytes = geometry_fixture(2U, 2U, 2U);
    replace_header_word(bytes, 0x80U, std::numeric_limits<std::uint32_t>::max());
    expect_invalid_geometry(std::move(bytes));
}

TEST(SfsGeometry, InvalidActiveBitmapIsReportedWithoutHidingReadableRecords) {
    auto bytes = geometry_fixture(2U, 2U, 2U);
    replace_header_word(bytes, 0x94U, 5U);
    const auto image = axk::open_image(std::make_shared<axk::MemoryReader>(std::move(bytes)), "invalid-selection.hds");
    ASSERT_TRUE(image) << image.error().message;
    ASSERT_EQ(image->partitions().size(), 1U);
    const auto &partition = image->partitions().front();
    ASSERT_EQ(partition.records.size(), 2U);
    EXPECT_EQ(partition.active_bitmap_cluster, 5U);
    EXPECT_EQ(partition.allocation.active_bitmap_copy, 0U);
    EXPECT_FALSE(partition.diagnostics.empty());
    EXPECT_FALSE(axk::allocation_is_safe_for_mutation(partition.allocation));
}

TEST(SfsGeometry, InvalidInactiveCopyRetainsItsLocationAndDoesNotPreventExactReads) {
    auto bytes = geometry_fixture(2U, 3U, 2U);
    replace_header_word(bytes, 0x98U, 0xfffffffeU);
    const auto image = axk::open_image(std::make_shared<axk::MemoryReader>(std::move(bytes)), "invalid-first-copy.hds");
    ASSERT_TRUE(image) << image.error().message;
    ASSERT_EQ(image->partitions().size(), 1U);
    const auto &partition = image->partitions().front();
    EXPECT_EQ(partition.bitmap_copy1_cluster, 2U);
    EXPECT_FALSE(partition.allocation.bitmap_copy1_valid);
    EXPECT_TRUE(partition.allocation.bitmap_copy2_valid);
    EXPECT_EQ(partition.allocation.active_bitmap_copy, 2U);
    EXPECT_FALSE(partition.diagnostics.empty());
    EXPECT_FALSE(axk::allocation_is_safe_for_mutation(partition.allocation));
    const auto payload = image->read_record_data(partition.index, axk::SfsId{1U}, 1041U);
    ASSERT_TRUE(payload) << payload.error().message;
    EXPECT_EQ(*payload, file_payload(1U, 1041U));
}

TEST(SfsGeometry, InvalidSelectedCopyIsNotSilentlyReplacedByTheOtherCopy) {
    auto bytes = geometry_fixture(2U, 2U, 2U);
    replace_header_word(bytes, 0x98U, 0xfffffffeU);
    const auto image =
        axk::open_image(std::make_shared<axk::MemoryReader>(std::move(bytes)), "invalid-selected-copy.hds");
    ASSERT_TRUE(image) << image.error().message;
    ASSERT_EQ(image->partitions().size(), 1U);
    const auto &partition = image->partitions().front();
    EXPECT_EQ(partition.active_bitmap_cluster, 2U);
    EXPECT_FALSE(partition.allocation.bitmap_copy1_valid);
    EXPECT_EQ(partition.allocation.active_bitmap_copy, 0U);
    EXPECT_FALSE(partition.diagnostics.empty());
    EXPECT_FALSE(axk::allocation_is_safe_for_mutation(partition.allocation));
}

TEST(SfsGeometry, RejectsBitmapOverlapWithTheIndexOrBackupHeader) {
    for (const auto bitmap_cluster : {1U, 4U, 5U}) {
        SCOPED_TRACE(bitmap_cluster);
        auto bytes = geometry_fixture(2U, 3U, 2U);
        replace_header_word(bytes, 0x98U, bitmap_cluster);
        expect_invalid_geometry(std::move(bytes));
    }
}

TEST(SfsGeometry, RejectsOverlappingBitmapCopies) {
    auto bytes = geometry_fixture(2U, 2U, 2U);
    replace_header_word(bytes, 0x9cU, 2U);
    expect_invalid_geometry(std::move(bytes));
}

TEST(SfsGeometry, RejectsBitmapOutsideThePartition) {
    auto bytes = geometry_fixture(2U, 2U, 2U);
    replace_header_word(bytes, 0x9cU, partition_clusters);
    expect_invalid_geometry(std::move(bytes));
}

TEST(SfsGeometry, FreeSpaceUsesTheSelectedBitmapWhenCopiesDisagree) {
    for (const auto active_bitmap : {2U, 3U}) {
        SCOPED_TRACE(active_bitmap);
        auto bytes = geometry_fixture(2U, active_bitmap, 2U);
        constexpr auto first_bitmap = partition_sector * sector_bytes + 2U * 1024U;
        constexpr auto extra_cluster = 11U;
        bytes[first_bitmap + extra_cluster / 8U] |= static_cast<std::byte>(0x80U >> (extra_cluster & 7U));
        const auto image =
            axk::open_image(std::make_shared<axk::MemoryReader>(std::move(bytes)), "bitmap-disagreement.hds");
        ASSERT_TRUE(image) << image.error().message;
        ASSERT_EQ(image->partitions().size(), 1U);
        const auto &allocation = image->partitions().front().allocation;
        EXPECT_FALSE(allocation.stored_copies_match);
        EXPECT_FALSE(axk::allocation_is_safe_for_mutation(allocation));
        ASSERT_TRUE(allocation.free_space);
        const auto used_clusters = active_bitmap == 2U ? 4U : 3U;
        EXPECT_EQ(allocation.free_space->allocated_cluster_count, used_clusters);
        EXPECT_EQ(allocation.free_space->free_cluster_count, partition_clusters - payload_cluster - used_clusters);
        EXPECT_EQ(allocation.free_space->free_bytes,
                  static_cast<std::uint64_t>(partition_clusters - payload_cluster - used_clusters) * 1024U);
    }
}

} // namespace
