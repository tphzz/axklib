#include "axklib/su700.hpp"
#include "su700_test_fixture.hpp"

namespace {
axk::FatImage floppy_fixture(std::string sample_name = "SAMPLE", bool truncated = false, bool generic = false,
                             bool legacy_song = false) {
    auto image = axk::FatImage::open(
        std::make_shared<axk::MemoryReader>(su700_floppy_bytes(sample_name, truncated, generic, legacy_song)));
    EXPECT_TRUE(image) << image.error().message;
    return std::move(*image);
}
} // namespace

TEST(Su700Control, PreservesPaddedNamesAndIgnoresInactiveDefaults) {
    const auto decoded = axk::decode_su700_control(control_fixture());
    ASSERT_TRUE(decoded) << decoded.error().message;
    ASSERT_EQ(decoded->songs.size(), 1U);
    EXPECT_EQ(decoded->songs[0].name, "SONG    ");
    EXPECT_EQ(decoded->songs[0].samples, (std::vector<std::string>{"SAMPLE  "}));
}

TEST(Su700Control, RejectsMalformedOrUnprovenControlStatesAndNames) {
    for (const auto offset : {8U, 17U, 369U}) {
        auto bytes = control_fixture();
        bytes[offset] = std::byte{2};
        EXPECT_FALSE(axk::decode_su700_control(bytes));
    }
    auto bytes = control_fixture();
    bytes[0] = std::byte{'/'};
    EXPECT_FALSE(axk::decode_su700_control(bytes));
    bytes.resize(7399);
    EXPECT_FALSE(axk::decode_su700_control(bytes));
}

TEST(Su700Floppy, MapsReferencesToPaddedHardDiskNamesAndPreservesExtras) {
    EXPECT_EQ(axk::inspect_su700_floppy(floppy_fixture("SAMPLE", false, false, true))->status,
              axk::Su700ImportStatus::complete);
    const auto result = axk::inspect_su700_floppy(floppy_fixture());
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->status, axk::Su700ImportStatus::complete) << result->issue;
    EXPECT_EQ(result->song_count, 1U);
    EXPECT_EQ(result->sample_count, 1U);
    EXPECT_EQ(result->suggested_volume_name, "SONG");
    ASSERT_EQ(result->files.size(), 4U);
    EXPECT_EQ(result->files[1].source_path, "SONG.SSQ");
    EXPECT_EQ(result->files[1].destination_path, (std::vector<std::string>{"SUSQ", "SONG    .SSQ"}));
    EXPECT_EQ(result->files[2].destination_path, (std::vector<std::string>{"SUSP", "SAMPLE  .SSP"}));
    EXPECT_EQ(result->files[3].role, axk::Su700ImportRole::extra);
}
TEST(Su700Floppy, RejectsMissingAndTruncatedReferencesWithoutMisclassifyingGenericFat) {
    for (const auto &image : {floppy_fixture("MISSING"), floppy_fixture("SAMPLE", true)}) {
        const auto result = axk::inspect_su700_floppy(image);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->status, axk::Su700ImportStatus::unsupported);
        EXPECT_TRUE(result->files.empty());
        EXPECT_FALSE(result->issue.empty());
    }
    EXPECT_EQ(axk::inspect_su700_floppy(floppy_fixture("SAMPLE", false, true))->status,
              axk::Su700ImportStatus::unrelated);
    axk::CancellationSource cancelled;
    cancelled.cancel();
    EXPECT_FALSE(axk::inspect_su700_floppy(floppy_fixture(), cancelled.token()));
}
