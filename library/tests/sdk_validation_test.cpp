#include "axklib/sdk.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>

#include <gtest/gtest.h>

#include "axklib/audio.hpp"
#include "axklib/media.hpp"
#include "axklib/sample_storage.hpp"
#include "axklib/semantic.hpp"
#include "axklib/writer.hpp"

namespace {

class SdkMediaValidation : public testing::TestWithParam<axk::MediaImageFormat> {
  protected:
    std::filesystem::path root;

    void SetUp() override {
        std::string name = testing::UnitTest::GetInstance()->current_test_info()->name();
        std::ranges::replace(name, '/', '-');
        for (std::size_t attempt = 0; attempt < 1024U; ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path() /
                                   ("axklib-sdk-validation-" + name + '-' + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                root = candidate;
                break;
            }
            ASSERT_FALSE(error) << error.message();
        }
        ASSERT_FALSE(root.empty());
        axk::Waveform waveform;
        waveform.format = {1U, 2U, 44'100U};
        waveform.frame_count = 2U;
        waveform.pcm = {std::byte{}, std::byte{}, std::byte{1}, std::byte{}};
        ASSERT_TRUE(axk::write_wav_atomic(root / "tone.wav", waveform));
        axk::VolumeSpec volume;
        volume.name = "Volume";
        volume.waveforms.push_back({"wave", "Wave", root / "tone.wav", 60U, {}});
        axk::SampleSpec sample;
        sample.name = "Sample";
        sample.waveform_id = "wave";
        sample.storage_format = axk::SampleStorageFormat::a3000_188;
        volume.samples.push_back(sample);
        sample.name = "Member";
        volume.samples.push_back(sample);
        volume.sample_banks.push_back({"Bank", {"Member"}, {}, axk::SampleStorageFormat::a3000_188});
        volume.programs.push_back({8U, "Direct", {{"SBNK", "Sample", {}}}});
        volume.programs.push_back({21U, "BankProg", {{"SBAC", "Bank", {}}}});
        axk::MediaBuildManifest manifest;
        manifest.schema_version = "1.0";
        manifest.format = GetParam();
        manifest.volume_name = volume.name;
        manifest.authored_volume = std::move(volume);
        const auto written = axk::write_media_image(manifest, root / "source.img");
        ASSERT_TRUE(written) << written.error().message;
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        EXPECT_FALSE(error) << error.message();
    }
};

TEST_P(SdkMediaValidation, EmptyBitmapsRemainWarningsInImageAndSnapshotAfterImageRelease) {
    const auto path = root / "source.img";
    {
        const auto media = axk::open_media(path);
        ASSERT_TRUE(media) << media.error().message;
        const auto objects = media->objects();
        ASSERT_TRUE(objects) << objects.error().message;
        std::fstream output{path, std::ios::binary | std::ios::in | std::ios::out};
        ASSERT_TRUE(output);
        constexpr std::array<char, 16> empty{};
        for (const auto &object : *objects) {
            if (object.decoded.header.type != axk::ObjectType::sbac &&
                object.decoded.header.type != axk::ObjectType::sbnk)
                continue;
            const auto bitmap_offset = object.decoded.header.type == axk::ObjectType::sbac ? 0x90U : 0xc0U;
            output.seekp(static_cast<std::streamoff>(object.data_offset + bitmap_offset));
            output.write(empty.data(), static_cast<std::streamsize>(empty.size()));
            ASSERT_TRUE(output);
        }
    }
    const auto media = axk::open_media(path);
    ASSERT_TRUE(media);
    for (const auto mode : {axk::MediaObjectReadMode::complete, axk::MediaObjectReadMode::decoded_metadata}) {
        const auto inventory = axk::build_media_inventory(*media, mode);
        ASSERT_TRUE(inventory);
        const auto graph = axk::build_relationship_graph(inventory->catalog);
        const auto validation = axk::validate_semantics(*media, *inventory, graph);
        EXPECT_EQ(validation.issues.size(), 2U);
        EXPECT_EQ(
            std::ranges::count(validation.issues, "REL_SBAC_PROGRAM_BITMAP_MISMATCH", &axk::ValidationIssue::code), 1);
        EXPECT_EQ(
            std::ranges::count(validation.issues, "REL_SBNK_PROGRAM_BITMAP_MISMATCH", &axk::ValidationIssue::code), 1);
    }
    axk::operation_context context;
    auto opened = axk::image::open(path.string(), context);
    ASSERT_TRUE(opened) << opened.error().message;
    const auto summary = opened->validation(context);
    ASSERT_TRUE(summary);
    EXPECT_TRUE(summary->valid);
    EXPECT_EQ(summary->issue_count, 2U);
    EXPECT_EQ(summary->warning_count, 2U);
    EXPECT_EQ(summary->error_count, 0U);
    const auto issues = opened->validation_issues(0U, 10U, context);
    ASSERT_TRUE(issues);
    ASSERT_EQ(issues->items.size(), 2U);
    auto snapshot = opened->make_snapshot();
    ASSERT_TRUE(snapshot);
    opened = axk::error{axk::error_code::invalid_argument, axk::error_category::internal, "released", {}};
    const auto retained = snapshot->validation();
    ASSERT_TRUE(retained);
    EXPECT_EQ(retained->issue_count, summary->issue_count);
    for (std::uint64_t offset = 0; offset < 2U; ++offset) {
        const auto page = snapshot->validation_issues(offset, 1U);
        ASSERT_TRUE(page);
        ASSERT_EQ(page->items.size(), 1U);
        EXPECT_EQ(page->total_count, 2U);
        EXPECT_EQ(page->items.front().code, issues->items[static_cast<std::size_t>(offset)].code);
        EXPECT_EQ(page->items.front().severity, "warning");
        EXPECT_FALSE(page->items.front().object_key.empty());
    }
}

TEST_P(SdkMediaValidation, CleanMatchingBitmapsHaveNoIssues) {
    axk::operation_context context;
    const auto opened = axk::image::open((root / "source.img").string(), context);
    ASSERT_TRUE(opened) << opened.error().message;
    const auto validation = opened->validation(context);
    ASSERT_TRUE(validation);
    EXPECT_TRUE(validation->valid);
    EXPECT_EQ(validation->issue_count, 0U);
}

TEST_P(SdkMediaValidation, ActiveMissingWaveDataIsReportedOnceWithoutTreeDuplicate) {
    const auto path = root / "source.img";
    {
        const auto media = axk::open_media(path);
        ASSERT_TRUE(media);
        const auto objects = media->objects();
        ASSERT_TRUE(objects);
        std::fstream output{path, std::ios::binary | std::ios::in | std::ios::out};
        ASSERT_TRUE(output);
        constexpr std::array<char, 16> missing{'A', 'b', 's', 'e', 'n', 't'};
        constexpr std::array<char, 4> no_cache{};
        for (const auto &object : *objects) {
            if (object.decoded.header.type != axk::ObjectType::sbnk)
                continue;
            output.seekp(static_cast<std::streamoff>(object.data_offset + 0x78U));
            output.write(missing.data(), static_cast<std::streamsize>(missing.size()));
            output.seekp(static_cast<std::streamoff>(object.data_offset + 0xa0U));
            output.write(no_cache.data(), static_cast<std::streamsize>(no_cache.size()));
            ASSERT_TRUE(output);
        }
    }
    axk::operation_context context;
    const auto opened = axk::image::open(path.string(), context);
    ASSERT_TRUE(opened);
    const auto validation = opened->validation(context);
    ASSERT_TRUE(validation);
    EXPECT_FALSE(validation->valid);
    EXPECT_EQ(validation->issue_count, 1U);
    EXPECT_EQ(validation->error_count, 1U);
    EXPECT_EQ(validation->warning_count, 0U);
    const auto issues = opened->validation_issues(0U, 10U, context);
    ASSERT_TRUE(issues);
    ASSERT_EQ(issues->items.size(), 1U);
    EXPECT_EQ(issues->items.front().code, "REL_ACTIVE_PROGRAM_SBNK_MEMBER_TARGET_MISSING");
    EXPECT_TRUE(issues->items.front().message.starts_with("2 Sample-to-Wave-Data link(s) across 2 Sample(s)"));
}

TEST_P(SdkMediaValidation, MalformedWavePayloadReturnsObjectBoundErrorsInsteadOfValidMetadata) {
    const auto path = root / "source.img";
    {
        const auto media = axk::open_media(path);
        ASSERT_TRUE(media);
        const auto objects = media->objects();
        ASSERT_TRUE(objects);
        const auto wave = std::ranges::find_if(
            *objects, [](const auto &object) { return object.decoded.header.type == axk::ObjectType::smpl; });
        ASSERT_NE(wave, objects->end());
        std::fstream output{path, std::ios::binary | std::ios::in | std::ios::out};
        ASSERT_TRUE(output);
        constexpr std::array<char, 4> oversized{static_cast<char>(0xff), static_cast<char>(0xff),
                                                static_cast<char>(0xff), static_cast<char>(0xff)};
        output.seekp(static_cast<std::streamoff>(wave->data_offset + 0x1cU));
        output.write(oversized.data(), static_cast<std::streamsize>(oversized.size()));
        ASSERT_TRUE(output);
    }
    axk::operation_context context;
    const auto opened = axk::image::open(path.string(), context);
    ASSERT_TRUE(opened);
    const auto validation = opened->validation(context);
    ASSERT_TRUE(validation);
    EXPECT_FALSE(validation->valid);
    EXPECT_EQ(validation->issue_count, 2U);
    EXPECT_EQ(validation->error_count, 2U);
    const auto issues = opened->validation_issues(0U, 10U, context);
    ASSERT_TRUE(issues);
    ASSERT_EQ(issues->items.size(), 2U);
    EXPECT_EQ(issues->items[0].code, "OBJECT_PAYLOAD_TRUNCATED");
    EXPECT_EQ(issues->items[1].code, "media_object_decode_failed");
    EXPECT_FALSE(issues->items[0].object_key.empty());
    EXPECT_EQ(issues->items[0].object_key, issues->items[1].object_key);
}

INSTANTIATE_TEST_SUITE_P(NonSfs, SdkMediaValidation,
                         testing::Values(axk::MediaImageFormat::fat12_floppy, axk::MediaImageFormat::iso9660));

} // namespace
