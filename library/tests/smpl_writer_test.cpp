#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/audio.hpp"
#include "axklib/media.hpp"
#include "axklib/object.hpp"
#include "axklib/writer.hpp"
#include "axklib/writer_internal.hpp"

namespace {

axk::ImportedAudio source_audio() {
    axk::ImportedAudio audio;
    audio.output_sample_rate = 44'100U;
    audio.output_sample_width_bits = 16U;
    audio.output_frames = 16U;
    audio.pcm_channels.resize(1U);
    for (std::uint16_t index = 0; index < 16U; ++index) {
        audio.pcm_channels.front().push_back(static_cast<std::byte>(index));
        audio.pcm_channels.front().push_back(static_cast<std::byte>(0x70U + index));
    }
    return audio;
}

axk::WaveformSpec waveform_spec(unsigned mode) {
    axk::WaveformSpec spec;
    spec.id = "mode" + std::to_string(mode);
    spec.name = spec.id;
    spec.root_key = 64U;
    spec.fine_tune_cents = -17;
    spec.loop_mode = static_cast<axk::AudioSamplerLoopMode>(mode);
    spec.loop_start_frame = 3U;
    spec.loop_length_frames = 9U;
    return spec;
}

void expect_metadata(const axk::DecodedObject &object, const axk::WaveformSpec &spec, std::uint32_t loop_start,
                     std::uint32_t loop_length) {
    const auto *smpl = std::get_if<axk::CurrentSmpl>(&object.payload);
    ASSERT_NE(smpl, nullptr);
    EXPECT_EQ(object.header.name, spec.name);
    EXPECT_EQ(smpl->sample_rate.value, 44'100U);
    EXPECT_EQ(smpl->duplicate_sample_rate.value, 44'100U);
    EXPECT_EQ(smpl->stored_sample_width_bytes.value, 2U);
    EXPECT_EQ(smpl->pcm_transfer_control.value, 0x30U);
    EXPECT_EQ(smpl->root_key.value, 64U);
    EXPECT_EQ(smpl->fine_tune_cents.value, -17);
    EXPECT_EQ(smpl->loop_mode.value, static_cast<unsigned>(spec.loop_mode));
    EXPECT_EQ(smpl->wave_start_frame.value, 0U);
    EXPECT_EQ(smpl->wave_length_frames.value, 16U);
    EXPECT_EQ(smpl->wave_end_frame_exclusive, 16U);
    EXPECT_EQ(smpl->loop_start_frame.value, loop_start);
    EXPECT_EQ(smpl->loop_length_frames.value, loop_length);
    EXPECT_EQ(smpl->loop_end_frame_exclusive, loop_start + loop_length);
    EXPECT_EQ(smpl->stored_pcm_bytes, 40U);
    EXPECT_EQ(smpl->stored_segment_bytes, 40U);
    EXPECT_EQ(smpl->stored_segment_offset, 0U);
    EXPECT_EQ(smpl->transient_name_hash_next_handle.value, 0U);
    EXPECT_EQ(smpl->transient_512_byte_block_counter.value, 0U);
}

class SmplWriterTest : public testing::Test {
  protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() / "axklib-smpl-writer-test";
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        std::filesystem::create_directories(root_);
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }
    std::filesystem::path root_;
};

} // namespace

TEST(SmplWriter, SerializesAllSixModesWithIndependentLoopWindowsAndCanonicalState) {
    const auto audio = source_audio();
    for (unsigned mode = 0U; mode < 6U; ++mode) {
        SCOPED_TRACE(mode);
        const auto spec = waveform_spec(mode);
        const auto payload = axk::detail::prepare_smpl_payload(spec, audio, 0x12345678U, "Volume");
        ASSERT_TRUE(payload) << payload.error().message;
        const auto object = axk::decode_object(*payload);
        ASSERT_TRUE(object);
        expect_metadata(*object, spec, 3U, 9U);
        for (const auto &[start, length] : std::array<std::pair<std::size_t, std::size_t>, 7>{
                 {{0x43U, 7U}, {0x68U, 4U}, {0x6fU, 9U}, {0x82U, 2U}, {0x86U, 8U}, {0x9eU, 12U}, {0xaaU, 2U}}}) {
            EXPECT_TRUE(std::ranges::all_of(std::span{*payload}.subspan(start, length),
                                            [](std::byte value) { return value == std::byte{}; }));
        }
        EXPECT_TRUE(std::ranges::equal(std::span{*payload}.subspan(0x6cU, 3U), std::span{*payload}.subspan(0x78U, 3U)));
        const axk::ObjectSnapshot snapshot{"wave",  axk::PartitionIndex{0}, axk::SfsId{1}, "fixture",
                                           *object, std::nullopt,           *payload};
        const auto waveform = axk::decode_waveform(snapshot, "wave.obj");
        ASSERT_TRUE(waveform);
        auto expected = audio.pcm_channels.front();
        expected.insert(expected.end(), audio.pcm_channels.front().begin(), audio.pcm_channels.front().begin() + 8);
        EXPECT_EQ(waveform->pcm, expected);
        EXPECT_EQ(waveform->frame_count, 20U);
    }
}

TEST(SmplWriter, DefaultsOnlyNonLoopingModesToTheFullLogicalWindow) {
    for (unsigned mode = 0U; mode < 6U; ++mode) {
        SCOPED_TRACE(mode);
        auto spec = waveform_spec(mode);
        spec.loop_start_frame = 0U;
        spec.loop_length_frames = 0U;
        const auto payload = axk::detail::prepare_smpl_payload(spec, source_audio(), 1U, "Volume");
        if (mode == 1U || mode == 2U) {
            ASSERT_FALSE(payload);
            EXPECT_EQ(payload.error().code, axk::ErrorCode::manifest_invalid);
        } else {
            ASSERT_TRUE(payload);
            const auto object = axk::decode_object(*payload);
            ASSERT_TRUE(object);
            expect_metadata(*object, spec, 0U, 16U);
        }
    }
}

TEST(SmplWriter, ValidatesLoopModeAndLogicalWindowBoundaries) {
    for (unsigned mode = 0U; mode < 6U; ++mode) {
        SCOPED_TRACE(mode);
        auto spec = waveform_spec(mode);
        spec.loop_start_frame = 3U;
        spec.loop_length_frames = 13U;
        const auto exact_end = axk::detail::prepare_smpl_payload(spec, source_audio(), 1U, "Volume");
        ASSERT_TRUE(exact_end);
        const auto object = axk::decode_object(*exact_end);
        ASSERT_TRUE(object);
        expect_metadata(*object, spec, 3U, 13U);
        for (const auto &[start, length] : std::array<std::pair<std::uint32_t, std::uint32_t>, 5>{
                 {{3U, 0U}, {16U, 1U}, {3U, 14U}, {0U, 17U}, {1U, std::numeric_limits<std::uint32_t>::max()}}}) {
            spec.loop_start_frame = start;
            spec.loop_length_frames = length;
            const auto invalid = axk::detail::prepare_smpl_payload(spec, source_audio(), 1U, "Volume");
            ASSERT_FALSE(invalid);
            EXPECT_EQ(invalid.error().code, axk::ErrorCode::manifest_invalid);
        }
    }
    for (unsigned mode = 6U; mode <= 255U; ++mode) {
        const auto invalid = axk::detail::prepare_smpl_payload(waveform_spec(mode), source_audio(), 1U, "Volume");
        ASSERT_FALSE(invalid) << mode;
        EXPECT_EQ(invalid.error().code, axk::ErrorCode::manifest_invalid);
    }
}

TEST_F(SmplWriterTest, ReopensAllSixModesAndExactPhysicalPcmAcrossHdsFat12AndIso) {
    const auto audio = source_audio();
    axk::Waveform source;
    source.format = {1U, 2U, audio.output_sample_rate};
    source.frame_count = audio.output_frames;
    source.pcm = audio.pcm_channels.front();
    const auto wav_path = root_ / "source.wav";
    ASSERT_TRUE(axk::write_wav_atomic(wav_path, source));
    axk::VolumeSpec volume;
    volume.name = "Volume";
    for (unsigned mode = 0U; mode < 6U; ++mode) {
        auto spec = waveform_spec(mode);
        spec.path = wav_path;
        volume.waveforms.push_back(std::move(spec));
    }
    const auto hds_path = root_ / "modes.hds";
    axk::HdsBuildManifest hds{"1.0", 4U * 1024U * 1024U, {{"Partition", {volume}}}};
    const auto written = axk::write_hds_image(hds, hds_path);
    ASSERT_TRUE(written) << written.error().message;
    std::vector<std::filesystem::path> paths{hds_path};
    for (const auto format : {axk::MediaImageFormat::fat12_floppy, axk::MediaImageFormat::iso9660}) {
        axk::MediaBuildManifest manifest;
        manifest.schema_version = "1.0";
        manifest.format = format;
        manifest.authored_volume = volume;
        manifest.iso_volume_id = "AXK_TEST";
        manifest.raw_group = "GROUP";
        manifest.group_name = "Group";
        manifest.raw_volume = "F001";
        manifest.volume_name = "Volume";
        const auto path = root_ / (format == axk::MediaImageFormat::fat12_floppy ? "modes.ima" : "modes.iso");
        const auto result = axk::write_media_image(manifest, path);
        ASSERT_TRUE(result) << result.error().message;
        paths.push_back(path);
    }
    auto expected = source.pcm;
    expected.insert(expected.end(), source.pcm.begin(), source.pcm.begin() + 8);
    for (const auto &path : paths) {
        SCOPED_TRACE(path.string());
        const auto media = axk::open_media(path);
        ASSERT_TRUE(media) << media.error().message;
        const auto objects = media->objects(axk::MediaObjectReadMode::complete);
        ASSERT_TRUE(objects) << objects.error().message;
        std::size_t count{};
        for (const auto &item : *objects) {
            if (item.decoded.header.type != axk::ObjectType::smpl)
                continue;
            ++count;
            const auto spec = std::ranges::find(volume.waveforms, item.decoded.header.name, &axk::WaveformSpec::name);
            ASSERT_NE(spec, volume.waveforms.end());
            expect_metadata(item.decoded, *spec, 3U, 9U);
            const auto waveform = axk::decode_waveform(item);
            ASSERT_TRUE(waveform) << waveform.error().message;
            EXPECT_EQ(waveform->pcm, expected);
            EXPECT_EQ(waveform->frame_count, 20U);
        }
        EXPECT_EQ(count, 6U);
    }
}
