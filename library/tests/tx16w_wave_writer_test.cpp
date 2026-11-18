#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/formats/tx16w/codec.hpp"
#include "axklib/formats/tx16w/wave_writer.hpp"

namespace {
using axk::tx16w::encode_wave;
using axk::tx16w::WaveEncoding;

struct Reference {
    WaveEncoding encoding;
    std::array<std::uint8_t, 10> header_tail;
    std::array<std::int16_t, 2> first_frames;
    std::array<std::uint8_t, 3> first_group;
    std::size_t file_size;
};

TEST(FormatGeneralization, W1) {
    constexpr std::array<std::uint8_t, 22> prefix{0x4c, 0x4d, 0x38, 0x39, 0x35, 0x33, 0, 0,    0,    0,    0,
                                                  0,    0,    0,    0,    0,    0,    0, 0x7f, 0x7f, 0x7f, 0x7f};
    constexpr std::array refs{
        Reference{{33333, 12096, 64, false},
                  {0xc9, 1, 0x40, 0x2f, 6, 0x40, 0, 0x52, 0, 0},
                  {-96, -80},
                  {0xff, 0xab, 0xff},
                  18432},
        Reference{{33333, 27769, 383, true},
                  {0x49, 1, 0x79, 0x6c, 6, 0x7f, 1, 0x52, 0, 0},
                  {-688, -736},
                  {0xfd, 0x52, 0xfd},
                  42496},
        Reference{{50000, 1344, 17856, true},
                  {0x49, 2, 0x40, 5, 0x10, 0xc0, 0x45, 0, 0, 0},
                  {-464, -448},
                  {0xfe, 0x34, 0xfe},
                  29184},
    };
    for (const auto &ref : refs) {
        const auto &e = ref.encoding;
        SCOPED_TRACE(e.attack_frames);
        std::vector<std::int16_t> pcm(e.attack_frames + e.repeat_frames);
        std::ranges::copy(ref.first_frames, pcm.begin());
        pcm.back() = -16;
        const auto file = encode_wave(e, pcm);
        ASSERT_TRUE(file) << file.error().message;
        ASSERT_EQ(file->size(), ref.file_size);
        const std::span bytes{*file};
        EXPECT_TRUE(std::ranges::equal(bytes.first(22), std::as_bytes(std::span{prefix})));
        EXPECT_TRUE(std::ranges::equal(bytes.subspan(22, 10), std::as_bytes(std::span{ref.header_tail})));
        EXPECT_TRUE(std::ranges::equal(bytes.subspan(32, 3), std::as_bytes(std::span{ref.first_group})));
        const auto tail = bytes.subspan(32 + ((pcm.size() + 1) / 2) * 3);
        EXPECT_TRUE(std::ranges::all_of(tail, [](std::byte value) { return value == std::byte{}; }));
        const auto decoded = axk::tx16w::decode_wave(bytes);
        ASSERT_TRUE(decoded) << decoded.error().message;
        EXPECT_EQ(decoded->pcm, pcm);
    }
    const std::vector<std::int16_t> sample(128);
    EXPECT_FALSE(encode_wave({44100, 64, 64, false}, sample));
}

TEST(Tx16wWaveWriter, PreservesTheSeventeenthLengthBitAndOddFrameWindow) {
    for (const auto attack : {65535U, 65536U, 131071U}) {
        for (const auto repeat : {65535U, 65536U, 131071U}) {
            const std::size_t count = attack + repeat;
            std::vector<std::int16_t> pcm(count);
            pcm[0] = 4656;
            pcm[1] = -32;
            pcm[2] = -32768;
            pcm[3] = 32752;
            pcm.back() = -16;
            const auto file = encode_wave({33333, attack, repeat, true}, pcm);
            ASSERT_TRUE(file) << file.error().message;
            const std::span bytes{*file};
            EXPECT_EQ(bytes[26], attack < 65536U ? std::byte{6} : std::byte{7});
            EXPECT_EQ(bytes[29], repeat < 65536U ? std::byte{0x52} : std::byte{0x53});
            constexpr std::array<std::uint8_t, 6> pairs{0x12, 0x3e, 0xff, 0x80, 0x0f, 0x7f};
            EXPECT_TRUE(std::ranges::equal(bytes.subspan(32, 6), std::as_bytes(std::span{pairs})));
            const auto window = axk::tx16w::decode_packed_pcm(bytes.subspan(32), 1, 1);
            ASSERT_TRUE(window);
            EXPECT_EQ(*window, (std::vector<std::int16_t>{-32}));
            if (count % 2 == 1) {
                const auto last = 32 + (count / 2) * 3;
                EXPECT_EQ(bytes[last], std::byte{0xff});
                EXPECT_EQ(bytes[last + 1], std::byte{0xf0});
                EXPECT_EQ(bytes[last + 2], std::byte{0});
            }
            const auto decoded = axk::tx16w::decode_wave(bytes);
            ASSERT_TRUE(decoded) << decoded.error().message;
            EXPECT_EQ(decoded->attack_frames, attack);
            EXPECT_EQ(decoded->repeat_frames, repeat);
            EXPECT_EQ(decoded->pcm, pcm);
        }
    }
}

TEST(Tx16wWaveWriter, RejectsUnqualifiedProfilesInvalidLengthsAndPrecisionLoss) {
    std::vector<std::int16_t> pcm(128);
    for (const auto &e : {WaveEncoding{16667, 64, 64, true}, WaveEncoding{50000, 64, 64, false}}) {
        const auto result = encode_wave(e, pcm);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, axk::ErrorCode::unsupported_profile);
    }
    for (const auto &e :
         {WaveEncoding{33333, 63, 65, false}, WaveEncoding{33333, 65, 63, true}, WaveEncoding{33333, 65, 64, true}}) {
        EXPECT_FALSE(encode_wave(e, pcm));
    }
    const std::vector<std::int16_t> too_long(131072U + 64U);
    EXPECT_FALSE(encode_wave({33333, 131072, 64, true}, too_long));
    EXPECT_FALSE(encode_wave({33333, 64, 131072, true}, too_long));
    EXPECT_FALSE(encode_wave({33333, 0, 0, false}, {}));
    for (const auto sample : std::array<std::int16_t, 3>{-1, 1, 32767}) {
        pcm.back() = sample;
        EXPECT_FALSE(encode_wave({33333, 64, 64, true}, pcm));
    }
}

TEST(Tx16wWaveWriter, DoesNotAddAnExtraSectorAtAnExactBoundary) {
    std::vector<std::int16_t> pcm(320);
    const auto exact = encode_wave({33333, 256, 64, false}, pcm);
    ASSERT_TRUE(exact);
    EXPECT_EQ(exact->size(), std::size_t{512});
    pcm.push_back(0);
    const auto crossed = encode_wave({33333, 257, 64, false}, pcm);
    ASSERT_TRUE(crossed);
    EXPECT_EQ(crossed->size(), std::size_t{1024});
}
} // namespace
