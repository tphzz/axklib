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

#include "media_ex5_fixture.hpp"

TEST(Ex5Reader, RecognizesDescriptorBeforeResidualSfsAndReadsFragmentedFile) {
    auto media = axk::open_media(std::make_shared<axk::MemoryReader>(ex5_fixture()), "ex5.hds");
    ASSERT_TRUE(media) << media.error().message;
    const auto *fat = std::get_if<axk::FatImage>(&media->storage());
    ASSERT_NE(fat, nullptr);
    EXPECT_EQ(media->kind(), axk::MediaKind::ex5_disk);
    EXPECT_EQ(fat->geometry().profile, axk::FatProfile::ex5_disk);
    ASSERT_EQ(fat->directories().size(), 1U);
    EXPECT_EQ(fat->directories().front().path, "DEMOS");
    EXPECT_EQ(fat->directories().front().attributes, 0x10U);
    EXPECT_EQ(fat->geometry().data_cluster_count, 4096U);
    EXPECT_EQ(fat->geometry().data_offset, data_offset);
    ASSERT_EQ(fat->files().size(), 1U);
    const auto &file = fat->files().front();
    EXPECT_EQ(file.path, "DEMOS/DEMO1.S1A");
    EXPECT_EQ(file.attributes, 0x20U);
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

TEST(Fat16Reader, ReadsUnwrappedVolumeUsingStandardSectorCountsAndEndMarkers) {
    const auto source = ex5_fixture();
    std::vector<std::byte> bytes(source.begin() + boot_offset, source.end());
    ascii(bytes, 3U, "MSDOS5.0");
    le16(bytes, 19U, 0U);
    le32(bytes, 32U, static_cast<std::uint32_t>(bytes.size() / sector_bytes));
    for (const auto copy : {fat_offset - boot_offset, fat_offset - boot_offset + fat_bytes})
        le16(bytes, copy + 7U * 2U, 0xfff8U);
    const auto image = axk::open_media(std::make_shared<axk::MemoryReader>(std::move(bytes)), "mo.hda");
    ASSERT_TRUE(image) << image.error().message;
    const auto *fat = std::get_if<axk::FatImage>(&image->storage());
    ASSERT_NE(fat, nullptr);
    ASSERT_EQ(fat->files().size(), 1U);
    EXPECT_EQ(fat->files().front().path, "DEMOS/DEMO1.S1A");
    EXPECT_EQ(fat->geometry().data_cluster_count, 4096U);
    const auto contents = fat->read_file(fat->files().front());
    ASSERT_TRUE(contents);
    EXPECT_EQ(contents->size(), 700U);
    EXPECT_EQ(contents->back(), std::byte{0x72});
    EXPECT_TRUE(fat->objects()->empty());
}

TEST(Fat16Reader, ReadsPrimaryPartitionsAndRejectsOverlapOrTruncatedVolumes) {
    const auto source = ex5_fixture();
    std::vector<std::byte> volume(source.begin() + boot_offset, source.end());
    ascii(volume, 3U, "MSDOS5.0");
    le16(volume, 19U, 0U);
    le32(volume, 32U, static_cast<std::uint32_t>(volume.size() / sector_bytes));
    constexpr std::size_t start = 63U * sector_bytes;
    std::vector<std::byte> bytes(start + 2U * volume.size());
    bytes[510U] = std::byte{0x55};
    bytes[511U] = std::byte{0xaa};
    for (std::size_t slot = 0U; slot < 2U; ++slot) {
        const auto offset = start + slot * volume.size();
        bytes[446U + slot * 16U + 4U] = std::byte{0x06};
        le32(bytes, 446U + slot * 16U + 8U, static_cast<std::uint32_t>(offset / sector_bytes));
        le32(bytes, 446U + slot * 16U + 12U, static_cast<std::uint32_t>(volume.size() / sector_bytes));
        std::copy(volume.begin(), volume.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    const auto media = axk::open_media(std::make_shared<axk::MemoryReader>(bytes), "disk.hda");
    ASSERT_TRUE(media) << media.error().message;
    const auto *disk = std::get_if<axk::FatDiskImage>(&media->storage());
    ASSERT_NE(disk, nullptr);
    ASSERT_EQ(disk->partitions().size(), 2U);
    EXPECT_EQ(disk->partitions()[1].byte_offset, start + volume.size());
    EXPECT_EQ(disk->partitions()[1].volume.files().front().size, 700U);
    const auto tree = axk::build_content_tree(*media, {}, {});
    ASSERT_EQ(tree.roots.size(), 2U);
    ASSERT_FALSE(tree.roots[0].children.empty());
    ASSERT_FALSE(tree.roots[1].children.empty());
    EXPECT_NE(tree.roots[0].children[0].node_id, tree.roots[1].children[0].node_id);
    auto invalid = bytes;
    le32(invalid, 462U + 8U, 63U);
    EXPECT_FALSE(axk::open_media(std::make_shared<axk::MemoryReader>(std::move(invalid)), "overlap.hda"));
    invalid = bytes;
    le32(invalid, 446U + 12U, 1U);
    EXPECT_FALSE(axk::open_media(std::make_shared<axk::MemoryReader>(std::move(invalid)), "truncated.hda"));
    bytes[446U + 4U] = std::byte{0x05};
    EXPECT_FALSE(axk::open_media(std::make_shared<axk::MemoryReader>(std::move(bytes)), "extended.hda"));
}

TEST(Ex5Reader, RemovableUsesStandardBpbButAdmitsTheFormatterClusterBoundary) {
    constexpr std::size_t fats = 512U;
    constexpr std::size_t fat_size = 256U * 512U;
    constexpr std::size_t root = fats + 2U * fat_size;
    constexpr std::size_t data = root + 32U * 512U;
    std::vector<std::byte> bytes(data + 65525U * 512U);
    ascii(bytes, 3U, "YAMAHA??");
    ascii(bytes, 54U, "FAT16   ");
    le16(bytes, 11U, 512U);
    bytes[13U] = std::byte{1};
    le16(bytes, 14U, 1U);
    bytes[16U] = std::byte{2};
    le16(bytes, 17U, 512U);
    bytes[21U] = std::byte{0xf8};
    le16(bytes, 22U, 256U);
    le32(bytes, 32U, static_cast<std::uint32_t>(bytes.size() / 512U));
    bytes[510U] = std::byte{0x55};
    bytes[511U] = std::byte{0xaa};
    for (const auto copy : {fats, fats + fat_size}) {
        le16(bytes, copy, 0xfff8U);
        le16(bytes, copy + 2U, 0xffffU);
        le16(bytes, copy + 2U * 0xfff5U, 0xffffU);
    }
    ascii(bytes, root, "END     BIN");
    bytes[root + 11U] = std::byte{0x21};
    le16(bytes, root + 26U, 0xfff5U);
    le32(bytes, root + 28U, 1U);
    bytes[data + (0xfff5U - 2U) * 512U] = std::byte{0x42};
    const auto image = axk::open_media(std::make_shared<axk::MemoryReader>(bytes), "mo.hda");
    ASSERT_TRUE(image) << image.error().message;
    const auto &fat = std::get<axk::FatImage>(image->storage());
    EXPECT_EQ(fat.geometry().profile, axk::FatProfile::ex5_removable);
    EXPECT_EQ(fat.geometry().data_cluster_count, 65525U);
    EXPECT_EQ(fat.read_file(fat.files().front()).value(), (std::vector<std::byte>{std::byte{0x42}}));
    ascii(bytes, 3U, "MSDOS5.0");
    EXPECT_FALSE(axk::open_media(std::make_shared<axk::MemoryReader>(std::move(bytes)), "not-ex5.hda"));
}
