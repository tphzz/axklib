#include <gtest/gtest.h>

#include "axklib/audio_export_wav_source.hpp"

namespace {

axk::PhysicalWaveformExport waveform(std::string key) {
    axk::Waveform audio;
    audio.name = key;
    audio.format = {1U, 2U, 44'100U};
    audio.frame_count = 200U;
    audio.pcm.resize(400U);
    return {key, key, key + ".wav", std::move(audio)};
}

axk::SampleExport sample() {
    axk::SampleExport result;
    result.display_name = "Mapped Sample";
    result.key_low = 36U;
    result.key_high = 84U;
    result.decoded.velocity_range_low = 12U;
    result.decoded.velocity_range_high = 110U;
    result.decoded.loop_mode = 2U;
    result.decoded.left.root_key = 60U;
    result.decoded.left.fine_tune_cents = -25;
    result.decoded.left.wave_start_frame = 10U;
    result.decoded.left.wave_length_frames = 100U;
    result.decoded.left.loop_start_frame = 20U;
    result.decoded.left.loop_length_frames = 40U;
    return result;
}

} // namespace

TEST(AudioExportWavSource, UsesSampleMemberPitchRangesAndLoopWithFullPhysicalPcm) {
    const auto physical = waveform("left");
    const auto logical = sample();

    const auto source = axk::audio_export_detail::sample_wav_source(logical, physical, "left");

    EXPECT_EQ(source.physical, &physical.waveform);
    ASSERT_TRUE(source.sampler.smpl);
    ASSERT_TRUE(source.sampler.inst);
    ASSERT_EQ(source.sampler.smpl->loops.size(), 1U);
    EXPECT_EQ(source.sampler.smpl->loops[0].start, 20U);
    EXPECT_EQ(source.sampler.smpl->loops[0].inclusive_end, 59U);
    EXPECT_EQ(source.sampler.inst->root_key, 60U);
    EXPECT_EQ(source.sampler.inst->fine_tune_cents, -25);
    EXPECT_EQ(source.sampler.inst->key_low, 36U);
    EXPECT_EQ(source.sampler.inst->key_high, 84U);
    EXPECT_EQ(source.sampler.inst->velocity_low, 12U);
    EXPECT_EQ(source.sampler.inst->velocity_high, 110U);
    ASSERT_EQ(source.warnings.size(), 1U);
    EXPECT_EQ(source.warnings[0].code, "wav_sampler_release_tail_approximated");
}

TEST(AudioExportWavSource, OmitsAmbiguousStereoMetadataInsteadOfChoosingOneMember) {
    auto left = waveform("left");
    auto right = waveform("right");
    auto logical = sample();
    logical.decoded.right = logical.decoded.left;
    logical.decoded.right->root_key = 61U;

    const auto source = axk::audio_export_detail::stereo_sample_wav_source(logical, left, right);

    EXPECT_FALSE(source.sampler.smpl);
    EXPECT_FALSE(source.sampler.inst);
    ASSERT_EQ(source.warnings.size(), 1U);
    EXPECT_EQ(source.warnings[0].code, "wav_stereo_sample_metadata_omitted");
}
