#include <array>
#include <fstream>

#include "media_test_fixtures.hpp"

TEST(StandaloneObject, UsesSharedDecoderAndRejectsArbitraryFiles) {
    auto object = axk::StandaloneObject::open(std::make_shared<axk::MemoryReader>(smpl_object()), "wave-data.obj");
    ASSERT_TRUE(object);
    EXPECT_EQ(object->object().decoded.header.name, "TEST");

    std::vector<std::byte> arbitrary(64);
    auto invalid =
        axk::StandaloneObject::open(std::make_shared<axk::MemoryReader>(std::move(arbitrary)), "invalid.bin");
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, axk::ErrorCode::container_unrecognized);
}

TEST(AxkObjectDirectory, AggregatesRecognizedObjectsAndIgnoresSupportFiles) {
    std::vector<axk::AxkObjectDirectoryEntry> entries;
    entries.push_back({"SMP_0002.002", std::make_shared<axk::MemoryReader>(smpl_object("SECOND"))});
    entries.push_back({"YAMAHA.SYM", std::make_shared<axk::MemoryReader>(std::vector<std::byte>(32U))});
    entries.push_back({"SMP_0001.001", std::make_shared<axk::MemoryReader>(smpl_object("FIRST"))});

    auto directory = axk::AxkObjectDirectory::open(std::move(entries), "objects");
    ASSERT_TRUE(directory);
    ASSERT_EQ(directory->stored_objects().size(), 2U);
    EXPECT_EQ(directory->stored_objects()[0].logical_path, "SMP_0001.001");
    EXPECT_EQ(directory->stored_objects()[1].logical_path, "SMP_0002.002");
    EXPECT_EQ(directory->stored_objects()[0].scope_key, "axk-object-directory");

    const axk::MediaContainer media{std::move(*directory)};
    EXPECT_EQ(media.kind(), axk::MediaKind::axk_object_directory);
    auto inventory = axk::build_media_inventory(media, axk::MediaObjectReadMode::decoded_metadata);
    ASSERT_TRUE(inventory);
    EXPECT_EQ(inventory->objects.size(), 2U);
    EXPECT_EQ(inventory->catalog.objects.size(), 2U);
}

TEST(AxkObjectDirectory, RejectsEmptyAmbiguousAndOversizedObjectSets) {
    auto empty = axk::AxkObjectDirectory::open(
        {{"YAMAHA.SYM", std::make_shared<axk::MemoryReader>(std::vector<std::byte>(32U))}}, "empty");
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, axk::ErrorCode::container_unrecognized);

    auto duplicate =
        axk::AxkObjectDirectory::open({{"SMP_0001.001", std::make_shared<axk::MemoryReader>(smpl_object("FIRST"))},
                                       {"smp_0001.001", std::make_shared<axk::MemoryReader>(smpl_object("SECOND"))}},
                                      "duplicate");
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, axk::ErrorCode::invalid_argument);

    auto oversized_bytes = smpl_object("LARGE");
    oversized_bytes.resize(static_cast<std::size_t>(axk::AxkObjectDirectory::maximum_payload_bytes + 1U));
    auto oversized = axk::AxkObjectDirectory::open(
        {{"SMP_0001.001", std::make_shared<axk::MemoryReader>(std::move(oversized_bytes))}}, "oversized");
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code, axk::ErrorCode::io_unsupported_size);
}

TEST(AxkObjectDirectory, OpensAFlatFilesystemDirectoryAsMedia) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-axk-object-directory";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    ASSERT_TRUE(std::filesystem::create_directories(root));
    {
        const auto bytes = smpl_object("FROM PATH");
        std::ofstream output{root / "SMP_0001.001", std::ios::binary};
        ASSERT_TRUE(output.good());
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    const auto media = axk::open_media(root);
    ASSERT_TRUE(media);
    EXPECT_EQ(media->kind(), axk::MediaKind::axk_object_directory);
    const auto objects = media->objects();
    ASSERT_TRUE(objects);
    ASSERT_EQ(objects->size(), 1U);
    EXPECT_EQ(objects->front().decoded.header.name, "FROM PATH");

    ASSERT_TRUE(std::filesystem::create_directory(root / "nested"));
    const auto nested = axk::open_media(root);
    ASSERT_FALSE(nested);
    EXPECT_EQ(nested.error().code, axk::ErrorCode::invalid_argument);
    std::filesystem::remove_all(root, error);
}

TEST(MediaPaths, SanitizesTraversalAndPlatformReservedCharacters) {
    EXPECT_EQ(axk::sanitize_path_component("../unsafe/name", "fallback"), "unsafe_name");
    EXPECT_EQ(axk::sanitize_path_component("..", "fallback"), "fallback");
    EXPECT_EQ(axk::sanitize_path_component("A:B*C?", "fallback"), "A_B_C");
    EXPECT_EQ(axk::sanitize_path_component("  Partition      A  ", "fallback"), "Partition A");
    EXPECT_EQ(axk::sanitize_path_component("A___B*", "fallback"), "A_B (2)");
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
