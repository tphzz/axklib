#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/wav_sampler_mapping.hpp"
#include "axklib/wav_stream.hpp"

namespace {

std::uint32_t le32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t result{};
    for (std::size_t index = 0; index < 4U; ++index)
        result |= std::to_integer<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    return result;
}

std::size_t chunk_offset(std::span<const std::byte> bytes, std::string_view id) {
    for (std::size_t offset = 12U; offset + 8U <= bytes.size();) {
        if (std::equal(id.begin(), id.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                       [](char left, std::byte right) { return static_cast<std::byte>(left) == right; })) {
            return offset;
        }
        const auto size = le32(bytes, offset + 4U);
        offset += 8U + size + (size % 2U);
    }
    return bytes.size();
}

std::vector<std::byte> collect(const axk::audio_internal::WavSource &source) {
    std::vector<std::byte> result;
    const auto streamed = axk::audio_internal::stream_wav(source, [&](std::span<const std::byte> bytes) {
        result.insert(result.end(), bytes.begin(), bytes.end());
        return axk::Result<void>{};
    });
    EXPECT_TRUE(streamed) << (streamed ? "" : streamed.error().message);
    return result;
}

axk::Waveform waveform() {
    axk::Waveform result;
    result.format = {1U, 1U, 44'100U};
    result.frame_count = 3U;
    result.pcm = {std::byte{1U}, std::byte{2U}, std::byte{3U}};
    return result;
}

} // namespace

TEST(WavStream, SerializesCompleteSamplerChunksWithRiffPaddingAndKnownLoopTypes) {
    const auto audio = waveform();
    auto source = axk::audio_internal::WavSource::from_physical(audio);
    source.sampler.smpl = axk::audio_internal::WavSmplChunk{
        .manufacturer = 0x01000041U,
        .product = 7U,
        .sample_period_nanoseconds = 22'675U,
        .midi_unity_note = 60U,
        .midi_pitch_fraction = 0x80000000U,
        .smpte_format = 25U,
        .smpte_offset = 0x01020304U,
        .loops = {{11U, 0U, 1U, 2U, 0U, 0U}, {12U, 1U, 0U, 1U, 0x40000000U, 3U}, {13U, 2U, 1U, 2U, 0U, 1U}},
        .sampler_specific_data = {std::byte{0xaaU}, std::byte{0xbbU}},
    };
    source.sampler.inst = axk::audio_internal::WavInstChunk{60U, -25, -3, 12U, 96U, 4U, 120U};

    const auto bytes = collect(source);
    ASSERT_EQ(le32(bytes, 4U), bytes.size() - 8U);
    const auto smpl = chunk_offset(bytes, "smpl");
    const auto inst = chunk_offset(bytes, "inst");
    const auto data = chunk_offset(bytes, "data");
    ASSERT_LT(smpl, bytes.size());
    ASSERT_LT(inst, bytes.size());
    ASSERT_LT(data, bytes.size());
    EXPECT_EQ(le32(bytes, smpl + 4U), 110U);
    EXPECT_EQ(le32(bytes, smpl + 8U), 0x01000041U);
    EXPECT_EQ(le32(bytes, smpl + 16U), 22'675U);
    EXPECT_EQ(le32(bytes, smpl + 36U), 3U);
    EXPECT_EQ(le32(bytes, smpl + 40U), 2U);
    EXPECT_EQ(le32(bytes, smpl + 48U), 0U);
    EXPECT_EQ(le32(bytes, smpl + 72U), 1U);
    EXPECT_EQ(le32(bytes, smpl + 96U), 2U);
    EXPECT_EQ(le32(bytes, inst + 4U), 7U);
    EXPECT_EQ(std::to_integer<std::uint8_t>(bytes[inst + 8U]), 60U);
    EXPECT_EQ(std::to_integer<std::uint8_t>(bytes[inst + 9U]), static_cast<std::uint8_t>(-25));
    EXPECT_EQ(data % 2U, 0U);
    EXPECT_EQ(le32(bytes, data + 4U), 3U);
    EXPECT_EQ(bytes.size() % 2U, 0U);
}

TEST(WavStream, SamplerMetadataParticipatesInExactWaveEquality) {
    const auto audio = waveform();
    auto first = axk::audio_internal::WavSource::from_physical(audio);
    first.sampler.inst = axk::audio_internal::WavInstChunk{60U, 0, 0, 0U, 127U, 0U, 127U};
    auto equal = first;
    auto distinct = first;
    distinct.sampler.inst->root_key = 61U;

    const auto same = axk::audio_internal::equal_wav(first, equal);
    ASSERT_TRUE(same);
    EXPECT_TRUE(*same);
    const auto different = axk::audio_internal::equal_wav(first, distinct);
    ASSERT_TRUE(different);
    EXPECT_FALSE(*different);
}

TEST(WavSamplerMapping, MapsForwardLoopAndInstrumentMetadata) {
    const axk::audio_internal::ASeriesSamplerParameters parameters{
        .sample_rate = 44'100U,
        .frame_count = 200U,
        .root_key = 60U,
        .fine_tune_cents = -25,
        .key_low = 36U,
        .key_high = 84U,
        .velocity_low = 12U,
        .velocity_high = 110U,
        .loop_mode = 1U,
        .loop_start = 20U,
        .loop_length = 40U,
        .context = "Sample Piano",
    };

    const auto mapped = axk::audio_internal::map_a_series_sampler_metadata(parameters);

    ASSERT_TRUE(mapped.chunks.smpl);
    ASSERT_TRUE(mapped.chunks.inst);
    ASSERT_EQ(mapped.chunks.smpl->loops.size(), 1U);
    EXPECT_EQ(mapped.chunks.smpl->sample_period_nanoseconds, 22'675U);
    EXPECT_EQ(mapped.chunks.smpl->midi_unity_note, 59U);
    EXPECT_EQ(mapped.chunks.smpl->midi_pitch_fraction, 3'221'225'472U);
    EXPECT_EQ(mapped.chunks.smpl->loops[0].type, 0U);
    EXPECT_EQ(mapped.chunks.smpl->loops[0].start, 20U);
    EXPECT_EQ(mapped.chunks.smpl->loops[0].inclusive_end, 59U);
    EXPECT_EQ(mapped.chunks.smpl->loops[0].play_count, 0U);
    EXPECT_EQ(mapped.chunks.inst->root_key, 60U);
    EXPECT_EQ(mapped.chunks.inst->fine_tune_cents, -25);
    EXPECT_EQ(mapped.chunks.inst->key_low, 36U);
    EXPECT_EQ(mapped.chunks.inst->key_high, 84U);
    EXPECT_EQ(mapped.chunks.inst->velocity_low, 12U);
    EXPECT_EQ(mapped.chunks.inst->velocity_high, 110U);
    EXPECT_TRUE(mapped.warnings.empty());
}

TEST(WavSamplerMapping, ApproximatesReleaseTailWithoutChangingLoopBounds) {
    auto parameters = axk::audio_internal::ASeriesSamplerParameters{
        .sample_rate = 32'000U,
        .frame_count = 100U,
        .root_key = 64U,
        .key_high = 127U,
        .velocity_high = 127U,
        .loop_mode = 2U,
        .loop_start = 10U,
        .loop_length = 30U,
    };

    const auto mapped = axk::audio_internal::map_a_series_sampler_metadata(parameters);

    ASSERT_TRUE(mapped.chunks.smpl);
    ASSERT_EQ(mapped.chunks.smpl->loops.size(), 1U);
    EXPECT_EQ(mapped.chunks.smpl->loops[0].type, 0U);
    EXPECT_EQ(mapped.chunks.smpl->loops[0].start, 10U);
    EXPECT_EQ(mapped.chunks.smpl->loops[0].inclusive_end, 39U);
    ASSERT_EQ(mapped.warnings.size(), 1U);
    EXPECT_EQ(mapped.warnings[0].code, "wav_sampler_release_tail_approximated");
}

TEST(WavSamplerMapping, DoesNotMisrepresentReversePlaybackAsABackwardLoop) {
    auto parameters = axk::audio_internal::ASeriesSamplerParameters{
        .sample_rate = 32'000U,
        .frame_count = 100U,
        .root_key = 64U,
        .key_high = 127U,
        .velocity_high = 127U,
        .loop_mode = 3U,
    };

    const auto mapped = axk::audio_internal::map_a_series_sampler_metadata(parameters);

    ASSERT_TRUE(mapped.chunks.smpl);
    EXPECT_TRUE(mapped.chunks.smpl->loops.empty());
    ASSERT_EQ(mapped.warnings.size(), 1U);
    EXPECT_EQ(mapped.warnings[0].code, "wav_sampler_reverse_playback_omitted");
}

TEST(WavSamplerMapping, OmitsInvalidRangesButPreservesPitchAndLoopMetadata) {
    const auto mapped = axk::audio_internal::map_a_series_sampler_metadata({
        .sample_rate = 44'100U,
        .frame_count = 50U,
        .root_key = 127U,
        .fine_tune_cents = 63,
        .key_low = 80U,
        .key_high = 20U,
        .velocity_low = 100U,
        .velocity_high = 10U,
        .loop_mode = 1U,
        .loop_start = 5U,
        .loop_length = 10U,
    });

    ASSERT_TRUE(mapped.chunks.smpl);
    EXPECT_EQ(mapped.chunks.smpl->midi_unity_note, 127U);
    EXPECT_FALSE(mapped.chunks.inst);
    ASSERT_EQ(mapped.warnings.size(), 1U);
    EXPECT_EQ(mapped.warnings[0].code, "wav_instrument_metadata_omitted");
}
