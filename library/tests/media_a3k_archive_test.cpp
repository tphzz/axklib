#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/io.hpp"
#include "axklib/media.hpp"
#include "media_test_fixtures.hpp"

namespace {

constexpr std::size_t archive_header_size = 1110U;
constexpr std::size_t archive_index_record_size = 271U;

void write_index_record(std::span<std::byte> record, std::string_view path, std::uint32_t offset, std::uint32_t size,
                        bool banner) {
    ASSERT_EQ(record.size(), archive_index_record_size);
    ASSERT_LT(path.size(), 256U);
    ascii(record, 0U, path);
    record[256U] = banner ? std::byte{} : std::byte{1U};
    le32(record, 258U, offset);
    le32(record, 262U, size);
    record[266U] = std::byte{1U};
}

std::uint32_t read_le32(std::span<const std::byte> bytes, std::size_t offset) {
    return std::to_integer<std::uint8_t>(bytes[offset]) |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U])) << 8U |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2U])) << 16U |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3U])) << 24U;
}

std::vector<std::byte> a3k_archive(std::string_view indexed_path = "Test Volume "
                                                                   "\\"
                                                                   "SMPL"
                                                                   "\\"
                                                                   "TEST",
                                   std::string_view banner_volume = "Test Volume") {
    const auto object = smpl_object("TEST");
    const auto banner = std::string{"\r\n       Volume Name : "} + std::string{banner_volume} + "\r\n";
    constexpr std::uint32_t entry_count = 2U;
    const auto object_offset = archive_header_size + banner.size();
    const auto index_offset = object_offset + object.size();
    std::vector<std::byte> bytes(index_offset + entry_count * archive_index_record_size);
    le32(bytes, 0U, 1U);
    le32(bytes, 4U, static_cast<std::uint32_t>(index_offset));
    le32(bytes, 8U, entry_count);
    ascii(bytes, 12U,
          "A3k"
          "Dis"
          "kyPC");
    ascii(bytes, 70U, "XXXXXXXXXXXXXXXX");
    ascii(bytes, archive_header_size, banner);
    std::ranges::copy(object, bytes.begin() + static_cast<std::ptrdiff_t>(object_offset));
    auto index = std::span{bytes}.subspan(index_offset);
    write_index_record(index.first(archive_index_record_size), "/A3kFileInfo.txt",
                       static_cast<std::uint32_t>(archive_header_size), static_cast<std::uint32_t>(banner.size()),
                       true);
    write_index_record(index.subspan(archive_index_record_size, archive_index_record_size), indexed_path,
                       static_cast<std::uint32_t>(object_offset), static_cast<std::uint32_t>(object.size()), false);
    return bytes;
}

TEST(A3kArchive, OpensThroughMediaDetectionAndBuildsOneVolume) {
    auto archive = a3k_archive();
    auto media = axk::open_media(std::make_shared<axk::MemoryReader>(std::move(archive)), "archive.a3k");
    ASSERT_TRUE(media) << axk::render_error(media.error());
    EXPECT_EQ(media->kind(), axk::MediaKind::a3k_archive);

    const auto *storage = std::get_if<axk::A3kArchive>(&media->storage());
    ASSERT_NE(storage, nullptr);
    EXPECT_EQ(storage->source_name(), "archive.a3k");
    EXPECT_EQ(storage->volume_label().value, "Test Volume");
    EXPECT_EQ(storage->volume_label().status, axk::LabelStatus::confirmed);
    ASSERT_EQ(storage->entries().size(), 1U);
    EXPECT_EQ(storage->entries().front().ordinal, 1U);
    EXPECT_EQ(storage->entries().front().indexed_path, "Test Volume "
                                                       "\\"
                                                       "SMPL"
                                                       "\\"
                                                       "TEST");

    auto inventory = axk::build_media_inventory(*media, axk::MediaObjectReadMode::decoded_metadata);
    ASSERT_TRUE(inventory) << axk::render_error(inventory.error());
    ASSERT_EQ(inventory->catalog.objects.size(), 1U);
    const auto &object = inventory->catalog.objects.front();
    ASSERT_TRUE(object.placement.has_value());
    EXPECT_EQ(object.placement->volume_name, "Test Volume");
    EXPECT_EQ(object.placement->category_name, "SMPL");
    EXPECT_EQ(object.placement->entry_name, "TEST");
    EXPECT_TRUE(object.raw_payload.empty());
    EXPECT_FALSE(inventory->raw_payloads_complete);

    auto loaded = axk::load_media_object(*media, inventory->objects.front());
    ASSERT_TRUE(loaded) << axk::render_error(loaded.error());
    EXPECT_EQ(loaded->raw_payload.size(), smpl_object().size());
}

TEST(A3kArchive, UsesBannerWhenIndexedPathIsMissing) {
    auto archive = a3k_archive("\\", "Banner Volume");
    auto opened = axk::A3kArchive::open(std::make_shared<axk::MemoryReader>(std::move(archive)), "fallback.a3k");
    ASSERT_TRUE(opened) << axk::render_error(opened.error());
    EXPECT_EQ(opened->volume_label().value, "Banner Volume");
    EXPECT_EQ(opened->volume_label().status, axk::LabelStatus::navigation_aid);
    ASSERT_EQ(opened->validation_issues().size(), 1U);
    EXPECT_EQ(opened->validation_issues().front().code, "a3k_index_path_incomplete");

    auto objects = opened->objects(axk::MediaObjectReadMode::decoded_metadata);
    ASSERT_TRUE(objects) << axk::render_error(objects.error());
    ASSERT_EQ(objects->size(), 1U);
    EXPECT_EQ(objects->front().raw_volume, "Banner Volume");
    EXPECT_EQ(objects->front().decoded.header.name, "TEST");
}

TEST(A3kArchive, EmbeddedObjectIdentityOverridesMismatchedIndexPath) {
    auto archive = a3k_archive("Test Volume "
                               "\\"
                               "SBNK"
                               "\\"
                               "WRONG");
    auto opened = axk::A3kArchive::open(std::make_shared<axk::MemoryReader>(std::move(archive)), "mismatch.a3k");
    ASSERT_TRUE(opened) << axk::render_error(opened.error());
    ASSERT_EQ(opened->validation_issues().size(), 1U);
    EXPECT_EQ(opened->validation_issues().front().code, "a3k_index_identity_mismatch");

    auto objects = opened->objects(axk::MediaObjectReadMode::decoded_metadata);
    ASSERT_TRUE(objects) << axk::render_error(objects.error());
    ASSERT_EQ(objects->size(), 1U);
    EXPECT_EQ(objects->front().logical_path, "Test Volume/SMPL/TEST");
    EXPECT_EQ(objects->front().decoded.header.type, axk::ObjectType::smpl);
    EXPECT_EQ(objects->front().decoded.header.name, "TEST");
}

TEST(A3kArchive, RejectsIndexRangesThatOverlapTheIndex) {
    auto archive = a3k_archive();
    const auto index_offset = static_cast<std::size_t>(read_le32(archive, 4U));
    auto second_record =
        std::span{archive}.subspan(index_offset + archive_index_record_size, archive_index_record_size);
    le32(second_record, 258U, static_cast<std::uint32_t>(index_offset));
    auto opened = axk::A3kArchive::open(std::make_shared<axk::MemoryReader>(std::move(archive)), "bad.a3k");
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code, axk::ErrorCode::allocation_invalid_extent);
}

TEST(A3kArchive, RejectsIndexCountThatDoesNotMatchTheFileGeometry) {
    auto archive = a3k_archive();
    le32(archive, 8U, 3U);
    auto opened = axk::A3kArchive::open(std::make_shared<axk::MemoryReader>(std::move(archive)), "bad.a3k");
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code, axk::ErrorCode::container_invalid_geometry);
}

TEST(A3kArchive, RejectsInvalidIndexRecordMarkers) {
    auto archive = a3k_archive();
    const auto index_offset = static_cast<std::size_t>(read_le32(archive, 4U));
    archive[index_offset + archive_index_record_size + 266U] = std::byte{};
    auto opened = axk::A3kArchive::open(std::make_shared<axk::MemoryReader>(std::move(archive)), "bad.a3k");
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code, axk::ErrorCode::container_invalid_geometry);
}

TEST(A3kArchive, RejectsOverlappingPayloadEntries) {
    auto archive = a3k_archive();
    const auto index_offset = static_cast<std::size_t>(read_le32(archive, 4U));
    auto banner_record = std::span{archive}.subspan(index_offset, archive_index_record_size);
    le32(banner_record, 262U, read_le32(banner_record, 262U) + 1U);
    auto opened = axk::A3kArchive::open(std::make_shared<axk::MemoryReader>(std::move(archive)), "bad.a3k");
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code, axk::ErrorCode::allocation_cross_link);
}

TEST(A3kArchive, RejectsEntriesWithoutEmbeddedYamahaObjects) {
    auto archive = a3k_archive();
    const auto index_offset = static_cast<std::size_t>(read_le32(archive, 4U));
    const auto object_record =
        std::span{archive}.subspan(index_offset + archive_index_record_size, archive_index_record_size);
    archive[read_le32(object_record, 258U)] = std::byte{};
    auto opened = axk::A3kArchive::open(std::make_shared<axk::MemoryReader>(std::move(archive)), "bad.a3k");
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code, axk::ErrorCode::object_malformed);
}

} // namespace
