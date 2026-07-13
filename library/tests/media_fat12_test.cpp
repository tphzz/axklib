#include "media_test_fixtures.hpp"

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

  auto unresolved = graph;
  axk::Relationship missing_member;
  missing_member.source_key = catalog->objects.front().key;
  missing_member.type = "SBNK_LEFT_MEMBER_TO_SMPL";
  missing_member.quality = axk::RelationshipQuality::unknown;
  unresolved.relationships.push_back(std::move(missing_member));
  const auto tree = axk::build_content_tree(media, *catalog, unresolved);
  ASSERT_EQ(tree.issues.size(), 1U);
  EXPECT_EQ(tree.issues.front().code, "REL_SBNK_MEMBER_TARGET_MISSING");
  EXPECT_EQ(tree.issues.front().severity, "warning");
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

TEST(Fat12Reader, ExportSkipsInvalidWaveformAndRetainsValidWaveforms) {
  auto image = axk::FatImage::open(
      std::make_shared<axk::MemoryReader>(fat_fixture_with_invalid_then_valid_waveform()),
      "mixed.ima");
  ASSERT_TRUE(image) << image.error().message;
  const axk::MediaContainer media{*image};
  const auto catalog = axk::build_object_catalog(media);
  ASSERT_TRUE(catalog) << catalog.error().message;
  const auto graph = axk::build_relationship_graph(*catalog);
  const auto plan = axk::build_export_plan(media, *catalog, graph);
  ASSERT_TRUE(plan) << plan.error().message;
  ASSERT_EQ(plan->volumes.size(), 1U);
  ASSERT_EQ(plan->volumes.front().waveforms.size(), 1U);
  EXPECT_EQ(plan->volumes.front().waveforms.front().display_name, "VALID");
  ASSERT_EQ(plan->decode_errors.size(), 1U);
  EXPECT_NE(plan->decode_errors.front().find("TEST"), std::string::npos);
  EXPECT_NE(plan->decode_errors.front().find("PCM span"), std::string::npos);
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

TEST(Fat12Reader, IgnoresLongNamesAndRequiresMatchingFatCopies) {
  auto long_name = fat_fixture();
  constexpr std::size_t root = 3U * 512U;
  std::ranges::copy_n(long_name.begin() + root, 32, long_name.begin() + root + 32U);
  std::fill_n(long_name.begin() + root, 32, std::byte{});
  long_name[root] = std::byte{0x41};
  long_name[root + 0x0bU] = std::byte{0x0f};
  const auto image = axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(long_name)),
                                         "long-name.ima");
  ASSERT_TRUE(image) << image.error().message;
  ASSERT_EQ(image->files().size(), 1U);
  EXPECT_EQ(image->files().front().path, "SMPTEST.004");

  auto mismatched = fat_fixture();
  mismatched[2U * 512U + 3U] ^= std::byte{0x01};
  const auto invalid = axk::FatImage::open(
      std::make_shared<axk::MemoryReader>(std::move(mismatched)), "fat-mismatch.ima");
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().code, axk::ErrorCode::container_backup_mismatch);
}
