#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/formats/typhoon/wave.hpp"

namespace {
using Bytes = std::vector<std::byte>;

Bytes octets(std::initializer_list<std::uint8_t> bytes) {
    Bytes result;
    for (auto b : bytes)
        result.push_back(static_cast<std::byte>(b));
    return result;
}

void fourcc(Bytes &bytes, std::string_view id) {
    for (auto c : id)
        bytes.push_back(static_cast<std::byte>(c));
}

void be32(Bytes &bytes, std::uint32_t n) {
    for (const auto shift : {24U, 16U, 8U, 0U})
        bytes.push_back(static_cast<std::byte>((n >> shift) & 0xffU));
}

void chunk(Bytes &bytes, std::string_view id, const Bytes &body) {
    fourcc(bytes, id);
    be32(bytes, static_cast<std::uint32_t>(body.size()));
    bytes.insert(bytes.end(), body.begin(), body.end());
    if ((body.size() % 2U) != 0U)
        bytes.push_back(std::byte{});
}

// A fixed DWVW bitstream encoded independently from the signed 12-bit sequence
// 0, 1, -1, 0, 1023, 2047, -2048, -2047, -1, 0, 0, 0, 0, 0, 0, 0.
Bytes compressed() { return octets({0xa2, 0x58, 0x7f, 0xf2, 0x00, 0x04, 0x8f, 0xfe, 0x11, 0xfc}); }

Bytes fixture(bool dwvw, const Bytes &audio, std::uint32_t frames = 16U, bool looping = true,
              std::uint16_t rate_word = 33333U) {
    Bytes body;
    fourcc(body, dwvw ? "AIFC" : "AIFF");
    Bytes appl;
    fourcc(appl, "stoc");
    appl.push_back(std::byte{7});
    fourcc(appl, "Typhoon");
    chunk(appl, "VInf", Bytes(16U, std::byte{0x42}));
    chunk(body, "APPL", appl);
    if (dwvw)
        chunk(body, "FVER", octets({0xa2, 0x80, 0x51, 0x40}));
    Bytes common = octets({0, 1});
    be32(common, frames);
    const auto rate_and_depth =
        octets({0, static_cast<std::uint8_t>(dwvw ? 12 : 16), 0x40, 0x0e, static_cast<std::uint8_t>(rate_word >> 8U),
                static_cast<std::uint8_t>(rate_word & 0xffU), 0, 0, 0, 0, 0, 0});
    common.insert(common.end(), rate_and_depth.begin(), rate_and_depth.end());
    if (dwvw) {
        fourcc(common, "DWVW");
        common.insert(common.end(), 2U, std::byte{}); // empty Pascal string and pad
    }
    chunk(body, "COMM", common);
    const auto mode = static_cast<std::uint8_t>(looping ? 1U : 0U);
    chunk(body, "INST", octets({68, 0, 0, 127, 0, 127, 0, 0, 0, mode, 0, 9, 0, 5, 0, mode, 0, 9, 0, 5}));
    // Marker IDs are references, not array indexes; intentionally put the end first.
    Bytes markers = octets({0, 2, 0, 5});
    be32(markers, frames);
    const auto second = octets({1, 'e', 0, 9, 0, 0, 0, 3, 1, 's'});
    markers.insert(markers.end(), second.begin(), second.end());
    chunk(body, "MARK", markers);
    Bytes ssnd(8U, std::byte{});
    ssnd.insert(ssnd.end(), audio.begin(), audio.end());
    chunk(body, "SSND", ssnd);
    Bytes form;
    chunk(form, "FORM", body);
    return form;
}
} // namespace

TEST(TyphoonWave, DecodesSignedDwvwAndResolvesLoopMarkerIds) {
    const auto file = fixture(true, compressed());
    const auto wave = axk::typhoon::inspect_wave(file);
    ASSERT_TRUE(wave);
    EXPECT_EQ(wave->frames, 16U);
    EXPECT_EQ(wave->sample_rate, 33333U);
    ASSERT_TRUE(wave->root_key);
    EXPECT_EQ(*wave->root_key, std::uint8_t{68});
    ASSERT_TRUE(wave->loop);
    EXPECT_EQ(wave->loop->first, 3U);
    EXPECT_EQ(wave->loop->second, 15U);
    const auto all = axk::typhoon::read_pcm16(file, 0U, 16U);
    ASSERT_TRUE(all);
    EXPECT_EQ(*all, (std::vector<std::int16_t>{0, 16, -16, 0, 16368, 32752, -32768, -32752, -16, 0, 0, 0, 0, 0, 0, 0}));
    const auto window = axk::typhoon::read_pcm16(file, 5U, 4U);
    ASSERT_TRUE(window);
    EXPECT_EQ(*window, (std::vector<std::int16_t>{32752, -32768, -32752, -16}));
    EXPECT_FALSE(axk::typhoon::read_pcm16(file, 15U, 2U));
    EXPECT_FALSE(axk::typhoon::read_pcm16(file, std::numeric_limits<std::uint32_t>::max(), 2U));
}

TEST(TyphoonWave, ReadsPlainPcmAndIgnoresInactiveLoopMarkers) {
    const auto file = fixture(false, octets({0x80, 0, 0xff, 0xf0, 0, 0, 0x7f, 0xf0}), 4U, false);
    const auto wave = axk::typhoon::inspect_wave(file);
    ASSERT_TRUE(wave);
    EXPECT_FALSE(wave->loop);
    const auto pcm = axk::typhoon::read_pcm16(file, 0U, 4U);
    ASSERT_TRUE(pcm);
    EXPECT_EQ(*pcm, (std::vector<std::int16_t>{-32768, -16, 0, 32752}));
}

TEST(TyphoonWave, ReadsUnnormalizedPositiveExtendedRates) {
    for (auto rate : {std::uint16_t{16667}, std::uint16_t{27485}}) {
        const auto wave = axk::typhoon::inspect_wave(fixture(true, compressed(), 16U, true, rate));
        ASSERT_TRUE(wave);
        EXPECT_EQ(wave->sample_rate, static_cast<std::uint32_t>(rate));
    }
    EXPECT_FALSE(axk::typhoon::inspect_wave(fixture(true, compressed(), 16U, true, 0U)));
}

TEST(TyphoonWave, DistinguishesMostNegativeDeltaFromNextLowest) {
    // Four signed deltas: 0, -2048, -2047, 0. After wrapping the 12-bit
    // accumulator these give 0, -2048, 1, 1, not 0, -2047, 2, 2.
    const auto file = fixture(true, octets({0xbf, 0xff, 0xff, 0xf2}), 4U, false);
    const auto pcm = axk::typhoon::read_pcm16(file, 0U, 4U);
    ASSERT_TRUE(pcm);
    EXPECT_EQ(*pcm, (std::vector<std::int16_t>{0, -32768, 16, 16}));
}

TEST(TyphoonWave, RejectsTruncatedFormAndShortCompressedStream) {
    auto file = fixture(true, compressed());
    file.pop_back();
    EXPECT_FALSE(axk::typhoon::inspect_wave(file));
    // This form and its SSND chunk are bounded correctly. Only the compressed
    // stream is short: a reader must not fill missing bits with zeros.
    auto audio = compressed();
    audio.resize(4U);
    EXPECT_FALSE(axk::typhoon::read_pcm16(fixture(true, audio), 0U, 16U));
}
