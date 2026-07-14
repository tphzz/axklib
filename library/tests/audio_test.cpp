#include <algorithm>
#include <filesystem>
#include <ranges>

#include <gtest/gtest.h>

#include "axklib/audio.hpp"

TEST(Audio, DecodesExactCurrentPcmAndWritesDeterministicWave) {
    const auto path = std::filesystem::path{AXK_SOURCE_ROOT} /
                      "tests/fixtures/images/sampler-authored/"
                      "HD00_512_single_sbnk_authored.hds";
    const auto container = axk::open_image(path);
    ASSERT_TRUE(container);
    const auto catalog = axk::build_object_catalog(*container);
    ASSERT_TRUE(catalog);
    const auto sample = std::ranges::find(
        catalog->objects, std::string{"p0:sfs9"}, &axk::ObjectSnapshot::key);
    ASSERT_NE(sample, catalog->objects.end());
    const auto waveform = axk::decode_waveform(*container, *sample);
    ASSERT_TRUE(waveform);
    EXPECT_EQ(waveform->format.sample_rate, 48000U);
    EXPECT_EQ(waveform->format.sample_width_bytes, 2U);
    EXPECT_EQ(waveform->frame_count, 132U);
    EXPECT_EQ(waveform->stored_payload_transform, "byteswap16");
    const auto wav = axk::wav_bytes(*waveform);
    ASSERT_TRUE(wav);
    EXPECT_EQ(wav->size(), 44U + waveform->pcm.size());
    EXPECT_EQ((*wav)[0], std::byte{'R'});
    EXPECT_TRUE(std::ranges::equal(std::span{*wav}.subspan(44),
                                   std::span<const std::byte>{waveform->pcm}));
    const auto preview = axk::build_preview_envelope(*waveform, 16);
    ASSERT_TRUE(preview);
    EXPECT_EQ(preview->bins.size(), 16U);
}

TEST(Audio, PadsShorterStereoMemberAndRejectsFormatMismatch) {
    axk::Waveform left;
    left.format = {1, 2, 44100};
    left.frame_count = 2;
    left.pcm = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    auto right = left;
    right.frame_count = 1;
    right.pcm.resize(2);
    const auto decision = axk::stereo_render_decision(left, right);
    EXPECT_TRUE(decision.renderable);
    EXPECT_EQ(decision.right_padding_frames, 1U);
    const auto stereo = axk::render_stereo(left, right);
    ASSERT_TRUE(stereo);
    EXPECT_EQ(stereo->pcm.size(), 8U);
    EXPECT_EQ(stereo->pcm[6], std::byte{});
    right.format.sample_rate = 48000;
    EXPECT_FALSE(axk::stereo_render_decision(left, right).renderable);
}

TEST(Audio, RejectsInconsistentPcmBeforeWavePreviewOrStereoAccess) {
    axk::Waveform malformed;
    malformed.format = {1, 2, 44100};
    malformed.frame_count = 2;
    malformed.pcm = {std::byte{}, std::byte{}};
    EXPECT_FALSE(axk::wav_bytes(malformed));
    EXPECT_FALSE(axk::build_preview_envelope(malformed, 1));

    auto valid = malformed;
    valid.frame_count = 1;
    EXPECT_FALSE(axk::render_stereo(valid, malformed));

    valid.format.channels = 3;
    EXPECT_FALSE(axk::wav_bytes(valid));
}
