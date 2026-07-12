#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/audio.hpp"
#include "axklib/audio_export.hpp"
#include "axklib/media.hpp"
#include "axklib/semantic.hpp"

namespace {

void le16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(value & 0xffU);
  bytes[offset + 1] = static_cast<std::byte>(value >> 8U);
}

void le32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
  le16(bytes, offset, static_cast<std::uint16_t>(value));
  le16(bytes, offset + 2, static_cast<std::uint16_t>(value >> 16U));
}

void be16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(value >> 8U);
  bytes[offset + 1] = static_cast<std::byte>(value & 0xffU);
}

void be32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
  be16(bytes, offset, static_cast<std::uint16_t>(value >> 16U));
  be16(bytes, offset + 2, static_cast<std::uint16_t>(value));
}

void ascii(std::span<std::byte> bytes, std::size_t offset, std::string_view value) {
  std::ranges::transform(value, bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                         [](char ch) { return static_cast<std::byte>(ch); });
}

std::vector<std::byte> smpl_object(std::string_view name = "TEST") {
  std::vector<std::byte> bytes(0xb0);
  ascii(bytes, 0, "FSFSDEV3SPLX");
  ascii(bytes, 0x0c, "SMPL");
  be32(bytes, 0x10, 0xac);
  be32(bytes, 0x1c, 4);
  be32(bytes, 0x20, 4);
  be16(bytes, 0x28, 32000);
  be16(bytes, 0x2a, 2);
  ascii(bytes, 0x32, name);
  be16(bytes, 0x8c, 32000);
  be32(bytes, 0x96, 2);
  be32(bytes, 0x9e, 2);
  bytes[0xac] = std::byte{0x12};
  bytes[0xad] = std::byte{0x34};
  bytes[0xae] = std::byte{0x56};
  bytes[0xaf] = std::byte{0x78};
  return bytes;
}

void set_fat12(std::span<std::byte> fat, std::uint16_t cluster, std::uint16_t value) {
  const auto offset = static_cast<std::size_t>(cluster) + cluster / 2U;
  auto pair = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(fat[offset])) |
              static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(fat[offset + 1])) << 8U;
  auto wide_pair = static_cast<std::uint32_t>(pair);
  const auto wide_value = static_cast<std::uint32_t>(value);
  if ((cluster & 1U) != 0U)
    wide_pair = (wide_pair & 0x000fU) | (wide_value << 4U);
  else
    wide_pair = (wide_pair & 0xf000U) | (wide_value & 0x0fffU);
  pair = static_cast<std::uint16_t>(wide_pair);
  fat[offset] = static_cast<std::byte>(wide_pair & 0xffU);
  fat[offset + 1] = static_cast<std::byte>(pair >> 8U);
}

std::vector<std::byte> fat_fixture(std::uint16_t chain_end = 0xfffU) {
  constexpr std::size_t sectors = 100;
  constexpr std::size_t sector_size = 512;
  constexpr std::size_t root_offset = 3 * sector_size;
  constexpr std::size_t data_offset = 4 * sector_size;
  std::vector<std::byte> bytes(sectors * sector_size);
  le16(bytes, 0x0b, sector_size);
  bytes[0x0d] = std::byte{1};
  le16(bytes, 0x0e, 1);
  bytes[0x10] = std::byte{2};
  le16(bytes, 0x11, 16);
  le16(bytes, 0x13, sectors);
  bytes[0x15] = std::byte{0xf0};
  le16(bytes, 0x16, 1);
  for (const auto fat_offset : {sector_size, 2 * sector_size}) {
    bytes[fat_offset] = std::byte{0xf0};
    bytes[fat_offset + 1] = std::byte{0xff};
    bytes[fat_offset + 2] = std::byte{0xff};
    set_fat12(std::span{bytes}.subspan(fat_offset, sector_size), 2, chain_end);
  }
  ascii(bytes, root_offset, "SMPTEST ");
  ascii(bytes, root_offset + 8, "004");
  bytes[root_offset + 0x0b] = std::byte{0x20};
  le16(bytes, root_offset + 0x1a, 2);
  const auto object = smpl_object();
  le32(bytes, root_offset + 0x1c, static_cast<std::uint32_t>(object.size()));
  std::ranges::copy(object, bytes.begin() + data_offset);
  return bytes;
}

std::vector<std::byte> nested_fat_fixture() {
  auto bytes = fat_fixture();
  const auto object = smpl_object();
  constexpr std::size_t root = 3U * 512U;
  constexpr std::size_t data = 4U * 512U;
  std::fill_n(bytes.begin() + root, 32, std::byte{});
  ascii(bytes, root, "OBJECTS ");
  bytes[root + 0x0b] = std::byte{0x10};
  le16(bytes, root + 0x1a, 2);
  std::fill_n(bytes.begin() + data, 512, std::byte{});
  ascii(bytes, data, "SMPTEST ");
  ascii(bytes, data + 8, "004");
  bytes[data + 0x0b] = std::byte{0x20};
  le16(bytes, data + 0x1a, 3);
  le32(bytes, data + 0x1c, static_cast<std::uint32_t>(object.size()));
  std::ranges::copy(object, bytes.begin() + data + 512);
  for (const auto fat_offset : {512U, 1024U}) {
    auto fat = std::span{bytes}.subspan(fat_offset, 512);
    set_fat12(fat, 2, 0xfff);
    set_fat12(fat, 3, 0xfff);
  }
  return bytes;
}

std::vector<std::byte> iso_record(std::span<const std::byte> name, std::uint32_t extent,
                                  std::uint32_t size, std::uint8_t flags) {
  auto length = 33U + name.size();
  if ((length & 1U) != 0U)
    ++length;
  std::vector<std::byte> record(length);
  record[0] = static_cast<std::byte>(length);
  le32(record, 2, extent);
  be32(record, 6, extent);
  le32(record, 10, size);
  be32(record, 14, size);
  record[25] = static_cast<std::byte>(flags);
  le16(record, 28, 1);
  be16(record, 30, 1);
  record[32] = static_cast<std::byte>(name.size());
  std::ranges::copy(name, record.begin() + 33);
  return record;
}

std::vector<std::byte> iso_record(std::string_view name, std::uint32_t extent, std::uint32_t size,
                                  std::uint8_t flags) {
  std::vector<std::byte> bytes;
  bytes.reserve(name.size());
  for (const auto ch : name)
    bytes.push_back(static_cast<std::byte>(ch));
  return iso_record(bytes, extent, size, flags);
}

void append_record(std::span<std::byte> sector, std::size_t &offset,
                   const std::vector<std::byte> &record) {
  std::ranges::copy(record, sector.begin() + static_cast<std::ptrdiff_t>(offset));
  offset += record.size();
}

std::vector<std::byte> iso_fixture(bool outside_extent = false) {
  constexpr std::size_t sector_size = 2048;
  constexpr std::uint32_t sector_count = 24;
  std::vector<std::byte> bytes(sector_count * sector_size);
  auto pvd = std::span{bytes}.subspan(16 * sector_size, sector_size);
  pvd[0] = std::byte{1};
  ascii(pvd, 1, "CD001");
  pvd[6] = std::byte{1};
  ascii(pvd, 40, "TESTVOL");
  le32(pvd, 80, sector_count);
  be32(pvd, 84, sector_count);
  le16(pvd, 128, sector_size);
  be16(pvd, 130, sector_size);
  const std::array dot{std::byte{0}};
  const std::array dotdot{std::byte{1}};
  const auto root_record = iso_record(dot, 18, sector_size, 2);
  std::ranges::copy(root_record, pvd.begin() + 156);

  auto root = std::span{bytes}.subspan(18 * sector_size, sector_size);
  std::size_t offset{};
  append_record(root, offset, root_record);
  append_record(root, offset, iso_record(dotdot, 18, sector_size, 2));
  append_record(root, offset, iso_record("GROUP", 19, sector_size, 2));

  auto group = std::span{bytes}.subspan(19 * sector_size, sector_size);
  offset = 0;
  append_record(group, offset, iso_record(dot, 19, sector_size, 2));
  append_record(group, offset, iso_record(dotdot, 18, sector_size, 2));
  append_record(group, offset, iso_record("F001", 20, sector_size, 2));
  append_record(group, offset, iso_record("0000.;1", 22, 32, 0));

  const auto object = smpl_object("CD WAVE");
  auto volume = std::span{bytes}.subspan(20 * sector_size, sector_size);
  offset = 0;
  append_record(volume, offset, iso_record(dot, 20, sector_size, 2));
  append_record(volume, offset, iso_record(dotdot, 19, sector_size, 2));
  append_record(volume, offset,
                iso_record("F000.;1", outside_extent ? 99 : 21,
                           static_cast<std::uint32_t>(object.size()), 0));
  if (!outside_extent)
    std::ranges::copy(object, bytes.begin() + 21 * sector_size);
  auto table = std::span{bytes}.subspan(22 * sector_size, 32);
  table[0] = std::byte{0xdd};
  ascii(table, 1, "Mapped Volume");
  ascii(table, 18, "F001");
  return bytes;
}

} // namespace

TEST(Fat12Reader, ReadsBoundedObjectAndBuildsSharedRelationshipsCatalog) {
  auto image =
      axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), "fixture.ima");
  ASSERT_TRUE(image) << image.error().message;
  EXPECT_EQ(image->geometry().data_cluster_count, 96U);
  ASSERT_EQ(image->files().size(), 1U);
  EXPECT_EQ(image->files()[0].path, "SMPTEST.004");
  auto objects = image->objects();
  ASSERT_TRUE(objects);
  ASSERT_EQ(objects->size(), 1U);
  EXPECT_EQ((*objects)[0].decoded.header.name, "TEST");
  EXPECT_EQ((*objects)[0].data_offset, 4U * 512U);
  auto waveform = axk::decode_waveform(objects->front());
  ASSERT_TRUE(waveform);
  EXPECT_EQ(waveform->format.sample_rate, 32000U);
  EXPECT_EQ(waveform->pcm, (std::vector<std::byte>{std::byte{0x34}, std::byte{0x12},
                                                   std::byte{0x78}, std::byte{0x56}}));

  axk::MediaContainer media{std::move(*image)};
  auto catalog = axk::build_object_catalog(media);
  ASSERT_TRUE(catalog);
  ASSERT_EQ(catalog->objects.size(), 1U);
  EXPECT_EQ(catalog->objects[0].placement->volume_name, "FAT root");
  const auto graph = axk::build_relationship_graph(*catalog);
  auto plan = axk::build_export_plan(media, *catalog, graph);
  ASSERT_TRUE(plan);
  ASSERT_EQ(plan->volumes.size(), 1U);
  EXPECT_EQ(plan->volumes[0].relative_root.generic_string(), "objects/FAT root");
  ASSERT_EQ(plan->volumes[0].waveforms.size(), 1U);
  const auto output = std::filesystem::temp_directory_path() / "axklib-media-export-test";
  std::error_code error;
  std::filesystem::remove_all(output, error);
  auto written = axk::write_export_audio(*plan, output);
  ASSERT_TRUE(written);
  ASSERT_EQ(written->written_files.size(), 1U);
  EXPECT_TRUE(std::filesystem::is_regular_file(written->written_files[0]));
  auto conflict = axk::write_export_audio(*plan, output);
  ASSERT_FALSE(conflict);
  EXPECT_EQ(conflict.error().code, axk::ErrorCode::io_open_failed);
  std::filesystem::remove_all(output, error);
}

TEST(Fat12Reader, RetainsHeaderInventoryWhenOneObjectPayloadIsMalformed) {
  auto image = fat_fixture();
  be32(image, 4U * 512U + 0x1cU, 0xffffffffU);
  auto reader = std::make_shared<axk::MemoryReader>(std::move(image));
  const auto fat = axk::FatImage::open(std::move(reader), "malformed.ima");
  ASSERT_TRUE(fat) << fat.error().message;
  const auto objects = fat->objects();
  ASSERT_TRUE(objects) << objects.error().message;
  ASSERT_EQ(objects->size(), 1U);
  EXPECT_EQ(objects->front().decoded.header.type, axk::ObjectType::smpl);
  EXPECT_TRUE(objects->front().decode_issue.has_value());
  const axk::MediaContainer container{*fat};
  const auto catalog = axk::build_object_catalog(container);
  ASSERT_TRUE(catalog) << catalog.error().message;
  EXPECT_EQ(catalog->objects.size(), 1U);
  ASSERT_EQ(catalog->issues.size(), 1U);
  EXPECT_EQ(catalog->issues.front().code, "media_object_decode_failed");
}

TEST(Fat12Reader, ContentTreeRetainsAnEmptyFatRoot) {
  auto image = fat_fixture();
  image[3U * 512U] = std::byte{0};
  const auto fat =
      axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(image)), "empty.ima");
  ASSERT_TRUE(fat) << fat.error().message;
  const axk::MediaContainer container{*fat};
  const auto catalog = axk::build_object_catalog(container);
  ASSERT_TRUE(catalog) << catalog.error().message;
  const auto graph = axk::build_relationship_graph(*catalog);
  const auto tree = axk::build_content_tree(container, *catalog, graph);
  ASSERT_EQ(tree.roots.size(), 1U);
  EXPECT_EQ(tree.roots.front().node_type, "volume");
  EXPECT_EQ(tree.roots.front().display_name, "FAT root");
  EXPECT_TRUE(tree.roots.front().children.empty());
}

TEST(Fat12Reader, ReadsSubdirectoriesAndRejectsCrossLinkedFiles) {
  auto image =
      axk::FatImage::open(std::make_shared<axk::MemoryReader>(nested_fat_fixture()), "nested.ima");
  ASSERT_TRUE(image) << image.error().message;
  ASSERT_EQ(image->files().size(), 1U);
  EXPECT_EQ(image->files()[0].path, "OBJECTS/SMPTEST.004");
  EXPECT_EQ(image->files()[0].directory_offset, 4U * 512U);
  EXPECT_EQ(image->files()[0].first_data_offset, 5U * 512U);

  auto cross_link = fat_fixture();
  constexpr std::size_t second = 3U * 512U + 32U;
  std::ranges::copy_n(cross_link.begin() + 3U * 512U, 32, cross_link.begin() + second);
  ascii(cross_link, second, "OTHER   ");
  auto invalid = axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(cross_link)),
                                     "cross-link.ima");
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().code, axk::ErrorCode::allocation_invalid_extent);
}

TEST(Fat12Reader, RejectsLoopsBadClustersTruncationDuplicatesAndNonFat12) {
  auto loop = fat_fixture(2);
  auto result =
      axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(loop)), "loop.ima");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, axk::ErrorCode::allocation_cycle);

  auto bad = fat_fixture(0xff7);
  result = axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(bad)), "bad.ima");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, axk::ErrorCode::allocation_invalid_extent);

  auto truncated = fat_fixture();
  le32(truncated, 3U * 512U + 0x1cU, 600);
  result = axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(truncated)),
                               "truncated.ima");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, axk::ErrorCode::container_truncated);

  auto duplicate = fat_fixture();
  std::ranges::copy_n(duplicate.begin() + 3 * 512, 32, duplicate.begin() + 3 * 512 + 32);
  result = axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(duplicate)),
                               "duplicate.ima");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, axk::ErrorCode::container_invalid_geometry);

  auto fat16 = fat_fixture();
  le16(fat16, 0x13, 5000);
  fat16.resize(5000U * 512U);
  result = axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(fat16)), "fat16.img");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, axk::ErrorCode::unsupported_profile);

  auto traversal = fat_fixture();
  traversal[3U * 512U + 2U] = std::byte{'/'};
  result = axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(traversal)),
                               "traversal.ima");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, axk::ErrorCode::container_invalid_geometry);
}

TEST(Iso9660Reader, LoadsYamahaScopeLabelsObjectsAndStructuredPaths) {
  auto image =
      axk::IsoImage::open(std::make_shared<axk::MemoryReader>(iso_fixture()), "fixture.iso");
  ASSERT_TRUE(image) << image.error().message;
  EXPECT_EQ(image->volume_id(), "TESTVOL");
  auto objects = image->objects();
  ASSERT_TRUE(objects) << objects.error().message;
  ASSERT_EQ(objects->size(), 1U);
  const auto &object = objects->front();
  EXPECT_EQ(object.logical_path, "GROUP/F001/F000");
  EXPECT_EQ(object.volume_label.value, "Mapped Volume");
  EXPECT_EQ(object.volume_label.status, axk::LabelStatus::confirmed);
  const auto path = axk::structured_object_path(object);
  EXPECT_EQ(path.relative_path.generic_string(), "GROUP/Mapped Volume/SMPL/CD WAVE");
}

TEST(Iso9660Reader, RejectsInvalidDescriptorAndOutOfRangeExtent) {
  auto invalid = iso_fixture();
  invalid[16U * 2048U + 1U] = std::byte{'X'};
  auto result =
      axk::IsoImage::open(std::make_shared<axk::MemoryReader>(std::move(invalid)), "invalid.iso");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, axk::ErrorCode::container_unrecognized);

  result =
      axk::IsoImage::open(std::make_shared<axk::MemoryReader>(iso_fixture(true)), "extent.iso");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, axk::ErrorCode::allocation_invalid_extent);

  auto duplicate = iso_fixture();
  constexpr std::size_t root = 18U * 2048U;
  std::ranges::copy_n(duplicate.begin() + root + 68U, 38, duplicate.begin() + root + 106U);
  result = axk::IsoImage::open(std::make_shared<axk::MemoryReader>(std::move(duplicate)),
                               "duplicate.iso");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, axk::ErrorCode::container_invalid_geometry);

  auto traversal = iso_fixture();
  traversal[root + 68U + 33U] = std::byte{'/'};
  result = axk::IsoImage::open(std::make_shared<axk::MemoryReader>(std::move(traversal)),
                               "traversal.iso");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, axk::ErrorCode::container_invalid_geometry);

  auto malformed = iso_fixture();
  malformed[18U * 2048U] = std::byte{20};
  result = axk::IsoImage::open(std::make_shared<axk::MemoryReader>(std::move(malformed)),
                               "malformed.iso");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, axk::ErrorCode::container_invalid_geometry);
}

TEST(Iso9660Reader, KeepsReadableInventoryWhenDeclaredTailFileIsMissing) {
  auto image = iso_fixture(true);
  constexpr std::size_t pvd = 16U * 2048U;
  le32(image, pvd + 80U, 120U);
  be32(image, pvd + 84U, 120U);
  const auto iso = axk::IsoImage::open(std::make_shared<axk::MemoryReader>(std::move(image)),
                                       "missing-tail.iso");
  ASSERT_TRUE(iso) << iso.error().message;
  const auto objects = iso->objects();
  ASSERT_TRUE(objects) << objects.error().message;
  EXPECT_TRUE(objects->empty());
}

TEST(Iso9660Reader, MarksContentFallbackAsNavigationAid) {
  auto fixture = iso_fixture();
  std::fill_n(fixture.begin() + 22U * 2048U, 32, std::byte{});
  auto image =
      axk::IsoImage::open(std::make_shared<axk::MemoryReader>(std::move(fixture)), "fallback.iso");
  ASSERT_TRUE(image) << image.error().message;
  auto objects = image->objects();
  ASSERT_TRUE(objects);
  ASSERT_EQ(objects->size(), 1U);
  EXPECT_EQ(objects->front().volume_label.value, "CD WAVE");
  EXPECT_EQ(objects->front().volume_label.status, axk::LabelStatus::navigation_aid);
  EXPECT_EQ(objects->front().volume_label.basis,
            "ISO directory path plus content-derived volume label fallback");
}

TEST(Iso9660Reader, DisambiguatesDuplicateContentTreeVolumesByRawIdentifier) {
  auto image =
      axk::IsoImage::open(std::make_shared<axk::MemoryReader>(iso_fixture()), "duplicate.iso");
  ASSERT_TRUE(image) << image.error().message;
  const axk::MediaContainer container{*image};
  auto catalog = axk::build_object_catalog(container);
  ASSERT_TRUE(catalog) << catalog.error().message;
  ASSERT_EQ(catalog->objects.size(), 1U);
  catalog->objects.front().placement->partition_name = " GROUP ";
  catalog->objects.front().placement->volume_name = " Mapped Volume ";
  catalog->objects.front().placement->container_directory = "GROUP/F001";
  auto duplicate = catalog->objects.front();
  duplicate.key = "duplicate";
  duplicate.sfs_id = axk::SfsId{2};
  duplicate.placement->volume_directory = axk::SfsId{2};
  duplicate.placement->container_directory = "GROUP/F002";
  catalog->objects.push_back(std::move(duplicate));

  const auto graph = axk::build_relationship_graph(*catalog);
  const auto tree = axk::build_content_tree(container, *catalog, graph);
  ASSERT_EQ(tree.roots.size(), 1U);
  EXPECT_EQ(tree.roots.front().display_name, "GROUP");
  ASSERT_EQ(tree.roots.front().children.size(), 2U);
  EXPECT_EQ(tree.roots.front().children[0].display_name, "Mapped Volume (F001)");
  EXPECT_EQ(tree.roots.front().children[1].display_name, "Mapped Volume (F002)");
}

TEST(Iso9660Reader, MarksOnlyUnknownActiveSampleBankMembersAsVolumeErrors) {
  auto image =
      axk::IsoImage::open(std::make_shared<axk::MemoryReader>(iso_fixture()), "member.iso");
  ASSERT_TRUE(image) << image.error().message;
  const axk::MediaContainer container{*image};
  auto catalog = axk::build_object_catalog(container);
  ASSERT_TRUE(catalog) << catalog.error().message;
  ASSERT_EQ(catalog->objects.size(), 1U);

  auto bank = catalog->objects.front();
  bank.key = "bank";
  bank.object.header.type = axk::ObjectType::sbnk;
  bank.object.header.name = "BANK";
  bank.object.payload = axk::CurrentSbnk{};
  auto program = bank;
  program.key = "program";
  program.object.header.type = axk::ObjectType::prog;
  program.object.header.name = "001";
  program.object.payload = axk::CurrentProg{};
  catalog->objects.push_back(std::move(bank));
  catalog->objects.push_back(std::move(program));

  axk::RelationshipGraph graph;
  axk::Relationship assignment;
  assignment.key = "assignment";
  assignment.source_key = "program";
  assignment.target_key = "bank";
  assignment.type = "PROG_ASSIGNMENT_TO_SBNK";
  assignment.quality = axk::RelationshipQuality::known;
  assignment.basis = "test";
  assignment.assignment_index = 0U;
  assignment.assignment_name = "BANK";
  assignment.assignment_state = axk::AssignmentState::active;
  graph.relationships.push_back(std::move(assignment));
  axk::Relationship member;
  member.key = "member";
  member.source_key = "bank";
  member.type = "SBNK_LEFT_MEMBER_TO_SMPL";
  member.quality = axk::RelationshipQuality::likely;
  member.basis = "test";
  graph.relationships.push_back(std::move(member));

  auto tree = axk::build_content_tree(container, *catalog, graph);
  ASSERT_EQ(tree.roots.size(), 1U);
  ASSERT_EQ(tree.roots.front().children.size(), 1U);
  EXPECT_EQ(tree.roots.front().children.front().display_name, "Mapped Volume");
  EXPECT_TRUE(tree.issues.empty());

  graph.relationships.back().quality = axk::RelationshipQuality::unknown;
  tree = axk::build_content_tree(container, *catalog, graph);
  EXPECT_EQ(tree.roots.front().children.front().display_name, "Mapped Volume (errors detected)");
  ASSERT_EQ(tree.issues.size(), 1U);
  EXPECT_EQ(tree.issues.front().code, "REL_ACTIVE_PROGRAM_SBNK_MEMBER_TARGET_MISSING");
}

TEST(StandaloneObject, UsesSharedDecoderAndRejectsArbitraryFiles) {
  auto object =
      axk::StandaloneObject::open(std::make_shared<axk::MemoryReader>(smpl_object()), "sample.obj");
  ASSERT_TRUE(object);
  EXPECT_EQ(object->object().decoded.header.name, "TEST");

  std::vector<std::byte> arbitrary(64);
  auto invalid = axk::StandaloneObject::open(
      std::make_shared<axk::MemoryReader>(std::move(arbitrary)), "invalid.bin");
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().code, axk::ErrorCode::container_unrecognized);
}

TEST(MediaPaths, SanitizesTraversalAndPlatformReservedCharacters) {
  EXPECT_EQ(axk::sanitize_path_component("../unsafe/name", "fallback"), ".._unsafe_name");
  EXPECT_EQ(axk::sanitize_path_component("..", "fallback"), "fallback");
  EXPECT_EQ(axk::sanitize_path_component("A:B*C?", "fallback"), "A_B_C_");
}

TEST(MediaPaths, DisambiguatesDuplicateDisplayedIsoVolumesByRawIdentifier) {
  auto decoded = axk::decode_object(smpl_object());
  ASSERT_TRUE(decoded);
  axk::MediaObject first{"first",
                         "GROUP/F001/F000",
                         "scope",
                         "GROUP",
                         "F001",
                         {"ORGANS", axk::LabelStatus::confirmed, "menu"},
                         {"Or11 Argent", axk::LabelStatus::confirmed, "menu"},
                         0,
                         1,
                         *decoded,
                         {},
                         {}};
  axk::MediaObject second = first;
  second.key = "second";
  second.logical_path = "GROUP/F002/F000";
  second.raw_volume = "F002";
  const std::array objects{first, second};
  const auto paths = axk::structured_object_paths(objects);
  ASSERT_EQ(paths.size(), 2U);
  EXPECT_EQ(paths[0].relative_path.generic_string(), "ORGANS/Or11 Argent (F001)/SMPL/TEST");
  EXPECT_EQ(paths[1].relative_path.generic_string(), "ORGANS/Or11 Argent (F002)/SMPL/TEST");
}
