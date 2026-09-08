#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/media.hpp"
#include "axklib/relationship.hpp"
#include "axklib/semantic.hpp"

namespace {

constexpr std::size_t sector_bytes = 512U;
constexpr std::size_t boot_offset = 256U * sector_bytes;
constexpr std::size_t fat_offset = 258U * sector_bytes;
constexpr std::size_t fat_bytes = 17U * sector_bytes;
constexpr std::size_t root_offset = fat_offset + 2U * fat_bytes;
constexpr std::size_t data_offset = root_offset + sector_bytes;

void le16(std::vector<std::byte> &bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>(value >> 8U);
}

void le32(std::vector<std::byte> &bytes, std::size_t offset, std::uint32_t value) {
    le16(bytes, offset, static_cast<std::uint16_t>(value & 0xffffU));
    le16(bytes, offset + 2U, static_cast<std::uint16_t>(value >> 16U));
}

void ascii(std::vector<std::byte> &bytes, std::size_t offset, std::string_view text) {
    for (const auto c : text)
        bytes[offset++] = static_cast<std::byte>(c);
}

void fat_entry(std::vector<std::byte> &bytes, std::uint16_t cluster, std::uint16_t next) {
    le16(bytes, fat_offset + static_cast<std::size_t>(cluster) * 2U, next);
    le16(bytes, fat_offset + fat_bytes + static_cast<std::size_t>(cluster) * 2U, next);
}

std::vector<std::byte> ex5_fixture() {
    std::vector<std::byte> bytes(data_offset + 4096U * sector_bytes);
    ascii(bytes, 0, "YAMAHA_dev3");
    ascii(bytes, 0x210U, "SY1200 V0.0.0   ");
    ascii(bytes, boot_offset + 3U, "YAMAHA??");
    le16(bytes, boot_offset + 11U, 512U);
    bytes[boot_offset + 13U] = std::byte{1};
    le16(bytes, boot_offset + 14U, 2U);
    bytes[boot_offset + 16U] = std::byte{2};
    le16(bytes, boot_offset + 17U, 16U);
    le16(bytes, boot_offset + 19U, 4096U);
    bytes[boot_offset + 21U] = std::byte{0xf8};
    le16(bytes, boot_offset + 22U, 17U);
    le16(bytes, boot_offset + 24U, 32U);
    le16(bytes, boot_offset + 26U, 8U);
    le32(bytes, boot_offset + 32U, 4096U + 34U);
    ascii(bytes, boot_offset + 54U, "FAT16   ");
    bytes[boot_offset + 510U] = std::byte{0x55};
    bytes[boot_offset + 511U] = std::byte{0xaa};
    fat_entry(bytes, 0, 0xfff8U);
    fat_entry(bytes, 1, 0xffffU);
    fat_entry(bytes, 2, 0xffffU);
    fat_entry(bytes, 3, 7U);
    fat_entry(bytes, 7, 0xffffU);
    ascii(bytes, root_offset, "DEMOS      ");
    bytes[root_offset + 11U] = std::byte{0x10};
    le16(bytes, root_offset + 26U, 2U);
    ascii(bytes, data_offset, "DEMO1   S1A");
    bytes[data_offset + 11U] = std::byte{0x20};
    le16(bytes, data_offset + 26U, 3U);
    le32(bytes, data_offset + 28U, 700U);
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(data_offset + sector_bytes), 512U, std::byte{0x31});
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(data_offset + 5U * sector_bytes), 188U, std::byte{0x72});
    return bytes;
}

} // namespace

TEST(Ex5Reader, RecognizesDescriptorBeforeResidualSfsAndReadsFragmentedFile) {
    auto media = axk::open_media(std::make_shared<axk::MemoryReader>(ex5_fixture()), "ex5.hds");
    ASSERT_TRUE(media) << media.error().message;
    const auto *fat = std::get_if<axk::FatImage>(&media->storage());
    ASSERT_NE(fat, nullptr);
    EXPECT_EQ(media->kind(), axk::MediaKind::ex5_disk);
    EXPECT_EQ(fat->geometry().profile, axk::FatProfile::ex5_disk);
    ASSERT_EQ(fat->directories().size(), 1U);
    EXPECT_EQ(fat->directories().front().path, "DEMOS");
    EXPECT_EQ(fat->geometry().data_cluster_count, 4096U);
    EXPECT_EQ(fat->geometry().data_offset, data_offset);
    ASSERT_EQ(fat->files().size(), 1U);
    const auto &file = fat->files().front();
    EXPECT_EQ(file.path, "DEMOS/DEMO1.S1A");
    EXPECT_EQ(file.clusters, (std::vector<std::uint16_t>{3U, 7U}));
    const auto read = fat->read_file_range(file, 508U, 8U);
    ASSERT_TRUE(read) << read.error().message;
    EXPECT_EQ(*read, (std::vector<std::byte>{std::byte{0x31}, std::byte{0x31}, std::byte{0x31}, std::byte{0x31},
                                             std::byte{0x72}, std::byte{0x72}, std::byte{0x72}, std::byte{0x72}}));
    EXPECT_TRUE(fat->validation_issues().empty());
    const auto objects = fat->objects();
    ASSERT_TRUE(objects);
    EXPECT_TRUE(objects->empty());
    const auto tree = axk::build_content_tree(*media, {}, {});
    ASSERT_EQ(tree.roots.size(), 1U);
    ASSERT_EQ(tree.roots.front().children.size(), 1U);
    const auto &directory = tree.roots.front().children.front();
    EXPECT_EQ(directory.node_type, "directory");
    EXPECT_EQ(directory.display_name, "DEMOS");
    ASSERT_EQ(directory.children.size(), 1U);
    EXPECT_EQ(directory.children.front().display_name, "DEMO1.S1A");
    EXPECT_EQ(directory.children.front().node_type, "file");
}

TEST(Ex5Reader, RejectsInvalidGeometryWithoutFallingBackToSfs) {
    for (const auto offset : {11U, 13U, 14U, 16U, 17U, 19U, 22U, 24U, 26U, 32U, 54U, 510U}) {
        auto bytes = ex5_fixture();
        le16(bytes, boot_offset + offset, 0U);
        const auto image = axk::open_media(std::make_shared<axk::MemoryReader>(std::move(bytes)), "bad-ex5.hds");
        EXPECT_FALSE(image) << "accepted invalid field at " << offset;
        if (!image) {
            EXPECT_NE(image.error().message.find("EX5"), std::string::npos) << image.error().message;
        }
    }
}

TEST(Ex5Reader, RejectsTruncationAndUndersizedFat) {
    auto bytes = ex5_fixture();
    bytes.resize(bytes.size() - 1U);
    EXPECT_FALSE(axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(bytes))));
    bytes = ex5_fixture();
    le16(bytes, boot_offset + 22U, 1U);
    EXPECT_FALSE(axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(bytes))));
}

TEST(Ex5Reader, RejectsDirectSfsAccessToAvoidInterpretingResidualHeaders) {
    const auto sfs = axk::open_image(std::make_shared<axk::MemoryReader>(ex5_fixture()), "ex5.hds");
    ASSERT_FALSE(sfs);
    EXPECT_EQ(sfs.error().code, axk::ErrorCode::unsupported_profile);
    EXPECT_NE(sfs.error().message.find("EX5"), std::string::npos);
}

TEST(Ex5Reader, ChecksAllocatedEmptyFileChains) {
    auto bytes = ex5_fixture();
    le16(bytes, data_offset + 26U, 2U);
    le32(bytes, data_offset + 28U, 0U);
    const auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(bytes)));
    ASSERT_FALSE(image);
    EXPECT_EQ(image.error().code, axk::ErrorCode::allocation_invalid_extent);
}

TEST(Ex5Reader, OnlyFfffTerminatesEx5Chains) {
    auto bytes = ex5_fixture();
    fat_entry(bytes, 7U, 0xfff8U);
    EXPECT_FALSE(axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(bytes))));

    constexpr std::size_t large_fat_bytes = 256U * sector_bytes;
    constexpr std::size_t large_root = fat_offset + 2U * large_fat_bytes;
    constexpr std::size_t large_data = large_root + sector_bytes;
    constexpr std::uint16_t cluster_count = 0xfffdU;
    const auto small = ex5_fixture();
    bytes.assign(large_data + static_cast<std::size_t>(cluster_count) * sector_bytes, std::byte{});
    std::copy_n(small.begin(), fat_offset, bytes.begin());
    le16(bytes, boot_offset + 19U, cluster_count);
    le16(bytes, boot_offset + 22U, 256U);
    le32(bytes, boot_offset + 32U, static_cast<std::uint32_t>(cluster_count) + 512U);
    for (const auto copy : {fat_offset, fat_offset + large_fat_bytes}) {
        le16(bytes, copy, 0xfff8U);
        le16(bytes, copy + 2U, 0xffffU);
        le16(bytes, copy + 4U, 0xfff8U);
        le16(bytes, copy + 0xfff8U * 2U, 0xffffU);
    }
    ascii(bytes, large_root, "HIGH    S1A");
    le16(bytes, large_root + 26U, 2U);
    le32(bytes, large_root + 28U, 513U);
    bytes[large_data] = std::byte{0x31};
    bytes[large_data + (0xfff8U - 2U) * sector_bytes] = std::byte{0x72};
    const auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(bytes)));
    ASSERT_TRUE(image) << image.error().message;
    ASSERT_EQ(image->files().size(), 1U);
    EXPECT_EQ(image->files().front().clusters, (std::vector<std::uint16_t>{2U, 0xfff8U}));
    const auto read = image->read_file_range(image->files().front(), 512U, 1U);
    ASSERT_TRUE(read);
    EXPECT_EQ(*read, (std::vector<std::byte>{std::byte{0x72}}));
}

TEST(Ex5Reader, RejectsDisagreeingMirrorsAndInvalidChains) {
    auto bytes = ex5_fixture();
    le16(bytes, fat_offset + 6U, 8U);
    const auto mismatch = axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(bytes)));
    ASSERT_FALSE(mismatch);
    EXPECT_EQ(mismatch.error().code, axk::ErrorCode::container_backup_mismatch);
    for (const auto next : {0U, 1U, 3U, 4098U, 0xfff0U, 0xfff7U, 0xffffU}) {
        bytes = ex5_fixture();
        fat_entry(bytes, 3U, static_cast<std::uint16_t>(next));
        EXPECT_FALSE(axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(bytes))))
            << "accepted invalid successor " << next;
    }
}

TEST(Ex5Reader, RejectsDirectoryFileCrossLinksAndDuplicateNames) {
    auto bytes = ex5_fixture();
    le16(bytes, data_offset + 26U, 2U);
    le32(bytes, data_offset + 28U, 100U);
    EXPECT_FALSE(axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(bytes))));
    bytes = ex5_fixture();
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(data_offset), 32U,
                bytes.begin() + static_cast<std::ptrdiff_t>(data_offset + 32U));
    EXPECT_FALSE(axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(bytes))));
}

TEST(Ex5Reader, ReportsPhysicalDirectoryEntryOffsetsAcrossFragmentation) {
    auto bytes = ex5_fixture();
    fat_entry(bytes, 2U, 6U);
    fat_entry(bytes, 6U, 0xffffU);
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(data_offset), sector_bytes, std::byte{0xe5});
    const auto offset = data_offset + 4U * sector_bytes;
    ascii(bytes, offset, "EMPTY   S1A");
    bytes[offset + 11U] = std::byte{0x20};
    const auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(bytes)));
    ASSERT_TRUE(image) << image.error().message;
    ASSERT_EQ(image->files().size(), 1U);
    EXPECT_EQ(image->files().front().directory_offset, offset);
    EXPECT_EQ(image->files().front().path, "DEMOS/EMPTY.S1A");
}
