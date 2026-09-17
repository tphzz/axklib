#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/object.hpp"
#include "axklib/wav_stream.hpp"

#include "media_test_fixtures.hpp"

namespace {

std::uint32_t wav_le32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t result{};
    for (std::size_t index = 0U; index < 4U; ++index)
        result |= std::to_integer<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    return result;
}

std::size_t wav_chunk_offset(std::span<const std::byte> bytes, std::string_view id) {
    for (std::size_t offset = 12U; offset + 8U <= bytes.size();) {
        if (id.size() == 4U &&
            std::equal(id.begin(), id.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                       [](char left, std::byte right) { return static_cast<std::byte>(left) == right; })) {
            return offset;
        }
        const auto payload_size = static_cast<std::size_t>(wav_le32(bytes, offset + 4U));
        const auto padded_size = payload_size + payload_size % 2U;
        if (padded_size > bytes.size() - offset - 8U)
            break;
        offset += 8U + padded_size;
    }
    return bytes.size();
}

} // namespace

TEST(Audio, DecodesExactCurrentPcmAndWritesDeterministicWave) {
    const auto path = std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/images/sampler-authored/"
                                                               "HD00_512_single_sbnk_authored.hds";
    const auto container = axk::open_image(path);
    ASSERT_TRUE(container);
    const auto catalog = axk::build_object_catalog(*container);
    ASSERT_TRUE(catalog);
    const auto sample = std::ranges::find(catalog->objects, std::string{"p0:sfs9"}, &axk::ObjectSnapshot::key);
    ASSERT_NE(sample, catalog->objects.end());
    const auto waveform = axk::decode_waveform(*container, *sample);
    ASSERT_TRUE(waveform);
    EXPECT_EQ(waveform->format.sample_rate, 48000U);
    EXPECT_EQ(waveform->format.sample_width_bytes, 2U);
    EXPECT_EQ(waveform->frame_count, 132U);
    EXPECT_EQ(waveform->stored_payload_transform, "byteswap16");
    const auto wav = axk::wav_bytes(*waveform);
    ASSERT_TRUE(wav);
    EXPECT_EQ((*wav)[0], std::byte{'R'});
    const auto data = wav_chunk_offset(*wav, "data");
    ASSERT_LT(data, wav->size());
    EXPECT_EQ(wav_le32(*wav, data + 4U), static_cast<std::uint32_t>(waveform->pcm.size()));
    EXPECT_TRUE(std::ranges::equal(std::span{*wav}.subspan(data + 8U, waveform->pcm.size()), std::span{waveform->pcm}));
    const auto preview = axk::build_preview_envelope(*waveform, 16);
    ASSERT_TRUE(preview);
    EXPECT_EQ(preview->bins.size(), 16U);
}

TEST(Audio, RespectsDeclaredSixteenBitWidthForMarkerLikePcm) {
    auto bytes = smpl_object("MARKER LIKE");
    bytes[0xac] = std::byte{0x00};
    bytes[0xad] = std::byte{0x55};
    bytes[0xae] = std::byte{0x80};
    bytes[0xaf] = std::byte{0xaa};
    auto object = axk::decode_object(bytes);
    ASSERT_TRUE(object);
    const axk::ObjectSnapshot snapshot{"p0:sfs9",          axk::PartitionIndex{0}, axk::SfsId{9},   "fixture",
                                       std::move(*object), std::nullopt,           std::move(bytes)};

    const auto waveform = axk::decode_waveform(snapshot, "marker-like.obj");
    ASSERT_TRUE(waveform);
    EXPECT_EQ(waveform->format.sample_width_bytes, 2U);
    EXPECT_EQ(waveform->frame_count, 2U);
    EXPECT_EQ(waveform->stored_payload_transform, "byteswap16");
    EXPECT_EQ(waveform->pcm,
              (std::vector<std::byte>{std::byte{0x55}, std::byte{0x00}, std::byte{0xaa}, std::byte{0x80}}));
}

TEST(Audio, RejectsUnsupportedTransferControlsWithoutLosingMetadata) {
    for (unsigned control = 0U; control <= 0xffU; ++control) {
        SCOPED_TRACE(control);
        auto bytes = smpl_object();
        bytes[0x84] = static_cast<std::byte>(control);
        auto object = axk::decode_object(bytes);
        ASSERT_TRUE(object);
        const auto &smpl = std::get<axk::CurrentSmpl>(object->payload);
        EXPECT_EQ(smpl.pcm_transfer_control.value, control);
        const axk::ObjectSnapshot snapshot{"wave",          axk::PartitionIndex{0}, axk::SfsId{9},
                                           "fixture",       std::move(*object),     std::nullopt,
                                           std::move(bytes)};
        const auto waveform = axk::decode_waveform(snapshot, "transfer.obj");
        if (control == 0x30U) {
            ASSERT_TRUE(waveform);
        } else {
            ASSERT_FALSE(waveform);
            EXPECT_EQ(waveform.error().code, axk::ErrorCode::audio_unsupported_format);
            EXPECT_EQ(waveform.error().category, axk::ErrorCategory::audio);
            EXPECT_NE(waveform.error().message.find(std::format("0x{:02x}", control)), std::string::npos);
        }
    }
}

TEST(Audio, AcceptsCurrentTransferControlForBothStoredWidths) {
    for (const auto width : {std::uint16_t{1}, std::uint16_t{2}}) {
        auto bytes = smpl_object();
        be16(bytes, 0x2aU, width);
        auto object = axk::decode_object(bytes);
        ASSERT_TRUE(object);
        const axk::ObjectSnapshot snapshot{"wave",          axk::PartitionIndex{0}, axk::SfsId{9},
                                           "fixture",       std::move(*object),     std::nullopt,
                                           std::move(bytes)};
        const auto waveform = axk::decode_waveform(snapshot, "transfer.obj");
        ASSERT_TRUE(waveform);
        EXPECT_EQ(waveform->format.sample_width_bytes, width);
        EXPECT_EQ(waveform->frame_count, 4U / width);
        const auto expected =
            width == 1U ? std::vector<std::byte>{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}}
                        : std::vector<std::byte>{std::byte{0x34}, std::byte{0x12}, std::byte{0x78}, std::byte{0x56}};
        EXPECT_EQ(waveform->pcm, expected);
    }
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

TEST(Audio, StreamsPhysicalAndRenderedWaveBytesWithoutChangingTheirEncoding) {
    axk::Waveform left;
    left.format = {1, 2, 44100};
    left.frame_count = 3;
    left.pcm = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6}};
    auto right = left;
    right.frame_count = 2;
    right.pcm.resize(4);

    const auto collect = [](const axk::audio_internal::WavSource &source) {
        std::vector<std::byte> result;
        const auto streamed = axk::audio_internal::stream_wav(source, [&](std::span<const std::byte> bytes) {
            result.insert(result.end(), bytes.begin(), bytes.end());
            return axk::Result<void>{};
        });
        EXPECT_TRUE(streamed);
        return result;
    };

    const auto physical = axk::wav_bytes(left);
    ASSERT_TRUE(physical);
    EXPECT_EQ(collect(axk::audio_internal::WavSource::from_physical(left)), *physical);

    const auto rendered = axk::render_stereo(left, right);
    ASSERT_TRUE(rendered);
    const auto rendered_bytes = axk::wav_bytes(*rendered);
    ASSERT_TRUE(rendered_bytes);
    EXPECT_EQ(collect(axk::audio_internal::WavSource::from_stereo(left, right)), *rendered_bytes);
}

TEST(Audio, ComparesStreamedWaveBytesExactly) {
    axk::Waveform first;
    first.format = {1, 2, 44100};
    first.frame_count = 2;
    first.pcm = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    auto equal = first;
    auto distinct = first;
    distinct.pcm.back() = std::byte{5};

    const auto same = axk::audio_internal::equal_wav(axk::audio_internal::WavSource::from_physical(first),
                                                     axk::audio_internal::WavSource::from_physical(equal));
    ASSERT_TRUE(same);
    EXPECT_TRUE(*same);
    const auto different = axk::audio_internal::equal_wav(axk::audio_internal::WavSource::from_physical(first),
                                                          axk::audio_internal::WavSource::from_physical(distinct));
    ASSERT_TRUE(different);
    EXPECT_FALSE(*different);
}
