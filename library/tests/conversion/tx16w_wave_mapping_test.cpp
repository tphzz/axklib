#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include "axklib/sample_parameters.hpp"
#include "axklib/tx16w_wave_mapping.hpp"

namespace {
using namespace axk::conversion::a_series_to_tx16w;

TEST(Tx16wWaveMapping, QuantizesSignedTiesAwayFromZeroAndSaturatesPeaks) {
    const std::array<double, 10> input{-32768, -9, -8, -7, 0, 7, 8, 9, 32767, 34000};
    const std::array<std::int16_t, 10> expected{-32768, -16, -16, 0, 0, 0, 16, 16, 32752, 32752};
    for (std::size_t i = 0; i < input.size(); ++i)
        EXPECT_EQ(quantize_pcm12(input[i]), expected[i]) << input[i];
}

TEST(Tx16wWaveMapping, LoopsAreRelativeToPlaybackNotTheRecordOrigin) {
    WaveSource source{};
    source.rate = 48000U;
    source.first_frame = 12000U;
    source.frame_count = 48000U;
    source.loop_mode = static_cast<unsigned>(axk::AudioSamplerLoopMode::forward_loop);
    source.loop_start = 36000U;
    source.loop_length = 24000U;
    auto mapped = map_playback(source);
    ASSERT_TRUE(mapped);
    EXPECT_TRUE(mapped->looped);
    EXPECT_EQ(mapped->sample_rate, 50000U);
    EXPECT_EQ(mapped->attack_frames, 25000U);
    EXPECT_EQ(mapped->repeat_frames, 25000U);
    --source.loop_length;
    EXPECT_FALSE(map_playback(source));
    ++source.loop_length;
    source.loop_mode = static_cast<unsigned>(axk::AudioSamplerLoopMode::forward_loop_release);
    EXPECT_FALSE(map_playback(source));
}

TEST(Tx16wWaveMapping, OneShotNeverPadsTruncatesOrWrapsNativePartLengths) {
    WaveSource source{};
    source.rate = 33333U;
    source.loop_mode = static_cast<unsigned>(axk::AudioSamplerLoopMode::forward_one_shot);
    source.frame_count = 127U;
    EXPECT_FALSE(map_playback(source));
    source.frame_count = 128U;
    auto mapped = map_playback(source);
    ASSERT_TRUE(mapped);
    EXPECT_EQ(mapped->attack_frames, 64U);
    EXPECT_EQ(mapped->repeat_frames, 64U);
    source.frame_count = 262142U;
    mapped = map_playback(source);
    ASSERT_TRUE(mapped);
    EXPECT_EQ(mapped->attack_frames, 131071U);
    EXPECT_EQ(mapped->repeat_frames, 131071U);
    ++source.frame_count;
    EXPECT_FALSE(map_playback(source));
    EXPECT_EQ(wave_filename("long name 1", 1U), "LONG_NAM.W01");
    EXPECT_EQ(wave_filename("long name 2", 64U), "LONG_NAM.W64");
}
} // namespace
