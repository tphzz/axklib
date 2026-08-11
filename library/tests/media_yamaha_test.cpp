#include <array>
#include <fstream>

#include "media_test_fixtures.hpp"

namespace {

std::vector<std::byte> smpl_segment(std::string_view name, std::uint32_t total_bytes, std::uint32_t segment_offset,
                                    std::span<const std::byte> pcm) {
    auto bytes = smpl_object(name);
    bytes.resize(0xacU + pcm.size());
    be32(bytes, 0x1cU, total_bytes);
    be32(bytes, 0x20U, static_cast<std::uint32_t>(pcm.size()));
    be32(bytes, 0x24U, segment_offset);
    std::ranges::copy(pcm, bytes.begin() + 0xacU);
    return bytes;
}

} // namespace

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

TEST(AxkObjectDirectory, RecognizesCandidatesFromPrefixesWithoutDecodingPayloads) {
    auto object_prefix = smpl_object("PREFIX");
    object_prefix.resize(12U);
    auto candidate = axk::AxkObjectDirectory::recognizes(
        {{"SMP_0001.001", std::make_shared<axk::MemoryReader>(std::move(object_prefix))},
         {"YAMAHA.SYM", std::make_shared<axk::MemoryReader>(std::vector<std::byte>(32U))}},
        "candidate");
    ASSERT_TRUE(candidate);
    EXPECT_TRUE(*candidate);

    auto support_only = axk::AxkObjectDirectory::recognizes(
        {{"YAMAHA.SYM", std::make_shared<axk::MemoryReader>(std::vector<std::byte>(32U))}}, "support-only");
    ASSERT_TRUE(support_only);
    EXPECT_FALSE(*support_only);
}

TEST(AxkObjectDirectory, RecognizesBoundedEntryPrefixes) {
    std::array<std::byte, 0x28U> prefix{};
    std::ranges::copy(std::as_bytes(std::span{"FSFSDEV3SPLX", 12U}), prefix.begin());
    EXPECT_TRUE(axk::AxkObjectDirectory::recognizes_entry_prefix(std::span{prefix}.first(12U), false));

    std::ranges::copy(std::as_bytes(std::span{"SMPL", 4U}), prefix.begin() + 0x0cU);
    prefix[0x1fU] = std::byte{4U};
    prefix[0x23U] = std::byte{2U};
    EXPECT_TRUE(axk::AxkObjectDirectory::recognizes_entry_prefix(prefix, true));

    prefix[0x1fU] = std::byte{2U};
    prefix[0x23U] = std::byte{2U};
    EXPECT_FALSE(axk::AxkObjectDirectory::recognizes_entry_prefix(prefix, true));
    EXPECT_FALSE(axk::AxkObjectDirectory::recognizes_entry_prefix(std::span{prefix}.first(12U), true));
}

TEST(AxkObjectDirectory, AssemblesContiguousWaveDataSegmentsFromNestedDiskFolders) {
    const std::array first_pcm{std::byte{0x10}, std::byte{0x20}};
    const std::array second_pcm{std::byte{0x30}, std::byte{0x40}, std::byte{0x50}, std::byte{0x60}};
    auto directory = axk::AxkObjectDirectory::open(
        {{"DISK1/SMP_0001.001", std::make_shared<axk::MemoryReader>(smpl_segment("SPLIT", 6U, 2U, second_pcm))},
         {"DISK2/SMP_0001.009", std::make_shared<axk::MemoryReader>(smpl_segment("SPLIT", 6U, 0U, first_pcm))}},
        "disk-set");
    ASSERT_TRUE(directory) << directory.error().message;
    ASSERT_EQ(directory->stored_objects().size(), 1U);
    const auto &object = directory->stored_objects().front();
    EXPECT_EQ(object.logical_path, "DISK2/SMP_0001.009");
    EXPECT_EQ(object.raw_payload.size(), 0xacU + 6U);
    EXPECT_FALSE(object.decode_issue);
    EXPECT_EQ(object.decoded.header.payload_bytes_0x1c, 6U);
    EXPECT_EQ(object.decoded.header.payload_bytes_0x20, 6U);
    EXPECT_EQ(object.decoded.header.payload_offset_0x24, 0U);
    const std::array expected_pcm{std::byte{0x10}, std::byte{0x20}, std::byte{0x30},
                                  std::byte{0x40}, std::byte{0x50}, std::byte{0x60}};
    EXPECT_TRUE(std::ranges::equal(std::span{object.raw_payload}.subspan(0xacU), expected_pcm));
}

TEST(AxkObjectDirectory, PreservesIncompleteWaveDataSegmentWithAnExplicitIssue) {
    const std::array pcm{std::byte{0x30}, std::byte{0x40}, std::byte{0x50}, std::byte{0x60}};
    auto directory = axk::AxkObjectDirectory::open(
        {{"DISK2/SMP_0001.001", std::make_shared<axk::MemoryReader>(smpl_segment("SPLIT", 6U, 2U, pcm))}},
        "incomplete-disk");
    ASSERT_TRUE(directory) << directory.error().message;
    ASSERT_EQ(directory->stored_objects().size(), 1U);
    const auto &object = directory->stored_objects().front();
    ASSERT_TRUE(object.decode_issue);
    EXPECT_EQ(object.decode_issue->message,
              "SMPL Wave Data is an incomplete multi-disk segment; open its parent object directory");
    const auto waveform = axk::decode_waveform(object);
    ASSERT_FALSE(waveform);
    EXPECT_EQ(waveform.error().code, axk::ErrorCode::object_missing);
    EXPECT_EQ(waveform.error().message,
              "SMPL Wave Data is an incomplete multi-disk segment; open its parent object directory");
}

TEST(AxkObjectDirectory, OpensAFlatOrOneLevelFilesystemDirectoryAsMedia) {
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

    std::filesystem::remove_all(root, error);
    ASSERT_TRUE(std::filesystem::create_directories(root / "DISK1"));
    ASSERT_TRUE(std::filesystem::create_directories(root / "DISK2"));
    const std::array first_pcm{std::byte{0x10}, std::byte{0x20}};
    const std::array second_pcm{std::byte{0x30}, std::byte{0x40}};
    {
        const auto bytes = smpl_segment("FROM SET", 4U, 0U, first_pcm);
        std::ofstream output{root / "DISK1" / "SMP_0001.001", std::ios::binary};
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    {
        const auto bytes = smpl_segment("FROM SET", 4U, 2U, second_pcm);
        std::ofstream output{root / "DISK2" / "SMP_0001.001", std::ios::binary};
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const auto nested = axk::open_media(root);
    ASSERT_TRUE(nested) << nested.error().message;
    const auto nested_objects = nested->objects();
    ASSERT_TRUE(nested_objects);
    ASSERT_EQ(nested_objects->size(), 1U);
    EXPECT_EQ(nested_objects->front().decoded.header.name, "FROM SET");
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
