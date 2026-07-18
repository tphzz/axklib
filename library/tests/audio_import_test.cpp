#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <sndfile.h>

#include "audio_import_internal.hpp"
#include "axklib/audio.hpp"
#include "axklib/writer.hpp"

namespace detail = axk::audio_import_detail;

namespace {

std::uint64_t pcm_hash(std::span<const std::int16_t> samples) {
    constexpr std::uint64_t offset_basis = 14'695'981'039'346'656'037ULL;
    constexpr std::uint64_t prime = 1'099'511'628'211ULL;
    std::uint64_t hash = offset_basis;
    for (const auto sample : samples) {
        const auto value = static_cast<std::uint16_t>(sample);
        hash = (hash ^ (value & 0xffU)) * prime;
        hash = (hash ^ (value >> 8U)) * prime;
    }
    return hash;
}

std::vector<double> deterministic_signal(std::size_t frames, std::size_t channels) {
    std::vector<double> result(frames * channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const auto numerator = static_cast<std::int32_t>((frame * 37U + channel * 11U) % 257U) - 128;
            result[frame * channels + channel] = static_cast<double>(numerator) / 160.0;
        }
    }
    return result;
}

bool write_sndfile(const std::filesystem::path &path, int format, std::uint32_t sample_rate, std::size_t channels,
                   std::span<const double> samples) {
    SF_INFO info{};
    info.channels = static_cast<int>(channels);
    info.samplerate = static_cast<int>(sample_rate);
    info.format = format;
    auto *file = sf_open(path.string().c_str(), SFM_WRITE, &info);
    if (file == nullptr)
        return false;
    const auto frames = static_cast<sf_count_t>(samples.size() / channels);
    const auto written = sf_writef_double(file, samples.data(), frames);
    return sf_close(file) == 0 && written == frames;
}

} // namespace

TEST(Pcm16Quantizer, UsesStableAxkTpdfPcg32V1Sequence) {
    const std::array<double, 32> silence{};
    const auto quantized = detail::quantize_pcm16(silence, true);
    ASSERT_TRUE(quantized);
    EXPECT_EQ(quantized->samples, (std::vector<std::int16_t>{0, -1, 0, 0, 0,  0, 0, 1, 0, 0, 0, 0,  0, 0,  1,  -1,
                                                             0, 0,  1, 0, -1, 0, 0, 0, 0, 0, 0, -1, 0, -1, -1, -1}));
    EXPECT_EQ(detail::dither_algorithm, "axk-tpdf-pcg32-v1");
}

TEST(Pcm16Quantizer, RoundsClipsAndRejectsNonFiniteInput) {
    const std::array input{0.0, 0.5 / 32'768.0, -0.5 / 32'768.0, 1.25, -1.25};
    const auto quantized = detail::quantize_pcm16(input, false);
    ASSERT_TRUE(quantized);
    EXPECT_EQ(quantized->samples, (std::vector<std::int16_t>{0, 1, 0, 32'767, -32'768}));
    EXPECT_EQ(quantized->clipped_samples, 2U);

    const std::array invalid{0.0, std::numeric_limits<double>::infinity()};
    EXPECT_FALSE(detail::quantize_pcm16(invalid, false));
}

TEST(Pcm16Quantizer, IsRepeatableAndKeepsTpdfWithinOneQuantizationStep) {
    const std::array<double, 4096> silence{};
    const auto first = detail::quantize_pcm16(silence, true);
    const auto second = detail::quantize_pcm16(silence, true);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first->samples, second->samples);
    EXPECT_TRUE(std::ranges::all_of(first->samples, [](std::int16_t value) { return value >= -1 && value <= 1; }));
    EXPECT_EQ(first->clipped_samples, 0U);
}

TEST(AudioChannels, SplitsInterleavedPcm16IntoLittleEndianMonoStreams) {
    const std::array<std::int16_t, 4> stereo{0x1234, -2, -32'768, 32'767};
    const auto channels = detail::split_pcm16(stereo, 2U);
    ASSERT_EQ(channels.size(), 2U);
    EXPECT_EQ(channels[0],
              (std::vector<std::byte>{std::byte{0x34}, std::byte{0x12}, std::byte{0x00}, std::byte{0x80}}));
    EXPECT_EQ(channels[1],
              (std::vector<std::byte>{std::byte{0xfe}, std::byte{0xff}, std::byte{0xff}, std::byte{0x7f}}));
}

TEST(AudioResampler, ConvertsCompleteFramesAndRejectsPartialFrames) {
    constexpr std::size_t input_frames = 480U;
    std::vector<double> stereo(input_frames * 2U);
    for (std::size_t frame = 0; frame < input_frames; ++frame) {
        stereo[frame * 2U] = std::sin(static_cast<double>(frame) * 0.1);
        stereo[frame * 2U + 1U] = std::cos(static_cast<double>(frame) * 0.1);
    }
    const auto converted = detail::resample_vhq(stereo, 2U, 48'000U, 44'100U);
    ASSERT_TRUE(converted);
    EXPECT_EQ(converted->size() % 2U, 0U);
    EXPECT_NEAR(static_cast<double>(converted->size() / 2U), 441.0, 1.0);

    EXPECT_FALSE(detail::resample_vhq(std::span{stereo}.first(3U), 2U, 48'000U, 44'100U));
}

TEST(AudioResampler, CoversIntegerNonIntegerShortAndLongRatiosDeterministically) {
    struct Case {
        std::size_t frames;
        std::size_t channels;
        std::uint32_t source_rate;
        std::uint32_t output_rate;
        std::uint64_t expected_hash;
    };
    constexpr std::array cases{
        Case{960U, 1U, 96'000U, 44'100U, 11'324'634'534'566'153'592ULL},
        Case{480U, 2U, 48'000U, 22'050U, 168'989'938'245'749'797ULL},
        Case{17U, 1U, 44'100U, 32'000U, 17'998'663'411'573'447'457ULL},
        Case{4096U, 2U, 44'100U, 24'000U, 1'900'267'954'302'172'028ULL},
    };
    for (const auto &item : cases) {
        const auto input = deterministic_signal(item.frames, item.channels);
        const auto first = detail::resample_vhq(input, item.channels, item.source_rate, item.output_rate);
        const auto second = detail::resample_vhq(input, item.channels, item.source_rate, item.output_rate);
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
        EXPECT_EQ(*first, *second);
        const auto expected_frames =
            static_cast<double>(item.frames) * item.output_rate / static_cast<double>(item.source_rate);
        EXPECT_NEAR(static_cast<double>(first->size() / item.channels), expected_frames, 1.0);
        const auto quantized = detail::quantize_pcm16(*first, true);
        ASSERT_TRUE(quantized);
        EXPECT_EQ(pcm_hash(quantized->samples), item.expected_hash)
            << item.frames << " frames, " << item.channels << " channels, " << item.source_rate << " -> "
            << item.output_rate;
    }
}

TEST(AudioImport, PreservesPcm16AtEverySupportedSamplerRate) {
    constexpr std::array rates{4'000U,  5'512U,  6'000U,  8'000U,  11'025U, 12'000U,
                               16'000U, 22'050U, 24'000U, 32'000U, 44'100U, 48'000U};
    const std::vector<std::byte> pcm{std::byte{0x00}, std::byte{0x80}, std::byte{0x34},
                                     std::byte{0x12}, std::byte{0xff}, std::byte{0x7f}};
    for (const auto rate : rates) {
        const auto path =
            std::filesystem::temp_directory_path() / ("axklib-audio-import-" + std::to_string(rate) + ".wav");
        std::error_code error;
        std::filesystem::remove(path, error);
        axk::Waveform waveform;
        waveform.format = {1U, 2U, rate};
        waveform.frame_count = 3U;
        waveform.pcm = pcm;
        ASSERT_TRUE(axk::write_wav_atomic(path, waveform));
        axk::AudioImportOptions options;
        options.expected_channels = 1U;
        const auto imported = axk::import_sampler_audio(path, options);
        ASSERT_TRUE(imported) << rate;
        EXPECT_EQ(imported->output_sample_rate, rate);
        EXPECT_FALSE(imported->resampled);
        EXPECT_FALSE(imported->quantized);
        ASSERT_EQ(imported->pcm_channels.size(), 1U);
        EXPECT_EQ(imported->pcm_channels.front(), pcm);
        std::filesystem::remove(path, error);
    }
}

TEST(AudioImport, DecodesPcm24AiffFlacAndFloatWaveWithStablePolicyMetadata) {
    struct Case {
        std::string_view extension;
        int format;
        std::string_view expected_format;
        std::string_view expected_subtype;
        std::uint32_t sample_rate;
        bool resampled;
    };
    constexpr std::array cases{
        Case{"aiff", SF_FORMAT_AIFF | SF_FORMAT_PCM_24, "AIFF", "PCM_24", 44'100U, false},
        Case{"flac", SF_FORMAT_FLAC | SF_FORMAT_PCM_24, "FLAC", "PCM_24", 44'100U, false},
        Case{"wav", SF_FORMAT_WAV | SF_FORMAT_FLOAT, "WAV", "FLOAT", 96'000U, true},
    };
    const auto samples = deterministic_signal(257U, 2U);
    for (const auto &item : cases) {
        const auto path =
            std::filesystem::temp_directory_path() / ("axklib-audio-import-converted." + std::string{item.extension});
        std::error_code error;
        std::filesystem::remove(path, error);
        ASSERT_TRUE(write_sndfile(path, item.format, item.sample_rate, 2U, samples));
        axk::AudioImportOptions options;
        options.expected_channels = 2U;
        const auto first = axk::import_sampler_audio(path, options);
        const auto second = axk::import_sampler_audio(path, options);
        ASSERT_TRUE(first) << item.extension;
        ASSERT_TRUE(second) << item.extension;
        EXPECT_EQ(first->source_format, item.expected_format);
        EXPECT_EQ(first->source_subtype, item.expected_subtype);
        EXPECT_EQ(first->resampled, item.resampled);
        EXPECT_TRUE(first->quantized);
        EXPECT_EQ(first->dither_algorithm, detail::dither_algorithm);
        EXPECT_EQ(first->pcm_channels, second->pcm_channels);
        ASSERT_EQ(first->pcm_channels.size(), 2U);
        EXPECT_EQ(first->pcm_channels[0].size(), first->pcm_channels[1].size());
        std::filesystem::remove(path, error);
    }
}

TEST(AudioImport, InspectsMonoAndStereoWithoutDecodingPcm) {
    const auto mono_path = std::filesystem::temp_directory_path() / "axklib-audio-inspect-mono.wav";
    const auto stereo_path = std::filesystem::temp_directory_path() / "axklib-audio-inspect-stereo.flac";
    std::error_code error;
    std::filesystem::remove(mono_path, error);
    std::filesystem::remove(stereo_path, error);

    ASSERT_TRUE(
        write_sndfile(mono_path, SF_FORMAT_WAV | SF_FORMAT_PCM_16, 44'100U, 1U, deterministic_signal(441U, 1U)));
    ASSERT_TRUE(
        write_sndfile(stereo_path, SF_FORMAT_FLAC | SF_FORMAT_PCM_24, 96'000U, 2U, deterministic_signal(960U, 2U)));

    const auto mono = axk::inspect_sampler_audio(mono_path);
    ASSERT_TRUE(mono) << mono.error().message;
    EXPECT_EQ(mono->source_format, "WAV");
    EXPECT_EQ(mono->source_subtype, "PCM_16");
    EXPECT_EQ(mono->channels, 1U);
    EXPECT_EQ(mono->frame_count, 441U);
    EXPECT_EQ(mono->source_sample_rate, 44'100U);
    EXPECT_EQ(mono->output_sample_rate, 44'100U);
    EXPECT_FALSE(mono->resampled);
    EXPECT_FALSE(mono->quantized);

    const auto stereo = axk::inspect_sampler_audio(stereo_path);
    ASSERT_TRUE(stereo) << stereo.error().message;
    EXPECT_EQ(stereo->source_format, "FLAC");
    EXPECT_EQ(stereo->source_subtype, "PCM_24");
    EXPECT_EQ(stereo->channels, 2U);
    EXPECT_EQ(stereo->frame_count, 960U);
    EXPECT_EQ(stereo->source_sample_rate, 96'000U);
    EXPECT_EQ(stereo->output_sample_rate, 44'100U);
    EXPECT_TRUE(stereo->resampled);
    EXPECT_TRUE(stereo->quantized);

    std::filesystem::remove(mono_path, error);
    std::filesystem::remove(stereo_path, error);
}

TEST(AudioImport, RejectsEmptyChannelMismatchAndSurroundSources) {
    const auto mono_path = std::filesystem::temp_directory_path() / "axklib-audio-import-mono.wav";
    const auto surround_path = std::filesystem::temp_directory_path() / "axklib-audio-import-surround.wav";
    const auto empty_path = std::filesystem::temp_directory_path() / "axklib-audio-import-empty.wav";
    std::error_code error;
    for (const auto &path : {mono_path, surround_path, empty_path})
        std::filesystem::remove(path, error);

    const auto mono = deterministic_signal(16U, 1U);
    const auto surround = deterministic_signal(16U, 3U);
    ASSERT_TRUE(write_sndfile(mono_path, SF_FORMAT_WAV | SF_FORMAT_FLOAT, 44'100U, 1U, mono));
    ASSERT_TRUE(write_sndfile(surround_path, SF_FORMAT_WAV | SF_FORMAT_FLOAT, 44'100U, 3U, surround));
    ASSERT_TRUE(write_sndfile(empty_path, SF_FORMAT_WAV | SF_FORMAT_FLOAT, 44'100U, 1U, {}));

    axk::AudioImportOptions stereo_options;
    stereo_options.expected_channels = 2U;
    EXPECT_FALSE(axk::import_sampler_audio(mono_path, stereo_options));
    EXPECT_FALSE(axk::import_sampler_audio(surround_path, stereo_options));
    axk::AudioImportOptions mono_options;
    mono_options.expected_channels = 1U;
    EXPECT_FALSE(axk::import_sampler_audio(empty_path, mono_options));

    for (const auto &path : {mono_path, surround_path, empty_path})
        std::filesystem::remove(path, error);
}
