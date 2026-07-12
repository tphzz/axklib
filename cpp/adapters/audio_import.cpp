#include "axklib/writer.hpp"

// Optional codec and resampler integration stays outside the format core.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <ranges>

#include <sndfile.h>
#include <soxr.h>

namespace axk {
namespace {

constexpr std::array<std::uint32_t, 12> rates{
    4'000, 5'512, 6'000, 8'000, 11'025, 12'000,
    16'000, 22'050, 24'000, 32'000, 44'100, 48'000};

Error audio_import_error(std::string message) {
  return make_error(ErrorCode::audio_unsupported_format, ErrorCategory::audio, std::move(message));
}

struct Uint128 {
  std::uint64_t high;
  std::uint64_t low;

  friend constexpr bool operator==(const Uint128&, const Uint128&) = default;
};

constexpr Uint128 multiply64(std::uint64_t left, std::uint64_t right) {
  constexpr auto mask = std::uint64_t{0xffff'ffff};
  const auto left_low = left & mask;
  const auto left_high = left >> 32U;
  const auto right_low = right & mask;
  const auto right_high = right >> 32U;
  const auto low_product = left_low * right_low;
  const auto middle = left_high * right_low + (low_product >> 32U);
  const auto middle_low = middle & mask;
  const auto middle_high = middle >> 32U;
  const auto combined_middle = middle_low + left_low * right_high;
  return {
      left_high * right_high + middle_high + (combined_middle >> 32U),
      (combined_middle << 32U) | (low_product & mask)};
}

constexpr Uint128 multiply128(Uint128 left, Uint128 right) {
  const auto low_product = multiply64(left.low, right.low);
  return {
      low_product.high + left.low * right.high + left.high * right.low,
      low_product.low};
}

constexpr Uint128 add128(Uint128 left, Uint128 right) {
  const auto low = left.low + right.low;
  return {left.high + right.high + static_cast<std::uint64_t>(low < left.low), low};
}

static_assert(add128(
                  multiply128(
                      {0xf80e7df0f5677911ULL, 0x3576e00ef62465a1ULL},
                      {0x2360ed051fc65da4ULL, 0x4385df649fccf645ULL}),
                  {0xf848e8b8d244377cULL, 0x73d97bf8c3ba49dfULL}) ==
              Uint128{0x568f2f6f52fa604bULL, 0xb962f28c107e6444ULL});

class NumpyPcg64 {
 public:
  double next_double() {
    state_ = add128(multiply128(state_, multiplier_), increment_);
    const auto rotation = static_cast<int>(state_.high >> 58U);
    const auto raw = std::rotr(state_.high ^ state_.low, rotation);
    return static_cast<double>(raw >> 11U) * (1.0 / 9'007'199'254'740'992.0);
  }

 private:
  static constexpr Uint128 multiplier_{
      0x2360ed051fc65da4ULL, 0x4385df649fccf645ULL};
  static constexpr Uint128 increment_{
      0xf848e8b8d244377cULL, 0x73d97bf8c3ba49dfULL};
  Uint128 state_{0xf80e7df0f5677911ULL, 0x3576e00ef62465a1ULL};
};

std::string format_name(int format) {
  switch (format & SF_FORMAT_TYPEMASK) {
    case SF_FORMAT_WAV: return "WAV";
    case SF_FORMAT_AIFF: return "AIFF";
    case SF_FORMAT_FLAC: return "FLAC";
    default: return "UNKNOWN";
  }
}

std::string subtype_name(int format) {
  switch (format & SF_FORMAT_SUBMASK) {
    case SF_FORMAT_PCM_U8: return "PCM_U8";
    case SF_FORMAT_PCM_S8: return "PCM_S8";
    case SF_FORMAT_PCM_16: return "PCM_16";
    case SF_FORMAT_PCM_24: return "PCM_24";
    case SF_FORMAT_PCM_32: return "PCM_32";
    case SF_FORMAT_FLOAT: return "FLOAT";
    case SF_FORMAT_DOUBLE: return "DOUBLE";
    default: return "UNKNOWN";
  }
}

std::vector<std::vector<std::byte>> split_pcm16(
    const std::vector<std::int16_t>& samples, std::size_t channels) {
  const auto frames = samples.size() / channels;
  std::vector<std::vector<std::byte>> result(channels);
  for (auto& channel : result) channel.reserve(frames * 2U);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    for (std::size_t channel = 0; channel < channels; ++channel) {
      const auto value = static_cast<std::uint16_t>(samples[frame * channels + channel]);
      result[channel].push_back(static_cast<std::byte>(value & 0xffU));
      result[channel].push_back(static_cast<std::byte>(value >> 8U));
    }
  }
  return result;
}

}  // namespace

Result<std::uint32_t> choose_sampler_sample_rate(
    std::uint32_t source_rate, std::optional<std::uint32_t> target_sample_rate) {
  if (target_sample_rate) {
    if (!std::ranges::contains(rates, *target_sample_rate)) {
      return std::unexpected{audio_import_error("target sample rate is not supported by A-series hardware")};
    }
    return *target_sample_rate;
  }
  return std::ranges::contains(rates, source_rate) ? source_rate : 44'100U;
}

Result<ImportedAudio> import_sampler_audio(
    const std::filesystem::path& path, const AudioImportOptions& options) {
  if (options.expected_channels != 1U && options.expected_channels != 2U) {
    return std::unexpected{audio_import_error("expected channel count must be one or two")};
  }
  SF_INFO info{};
  const auto path_text = path.string();
  std::unique_ptr<SNDFILE, decltype(&sf_close)> source{
      sf_open(path_text.c_str(), SFM_READ, &info), &sf_close};
  if (!source) {
    return std::unexpected{audio_import_error(
        "cannot decode audio source " + path.generic_string() + ": " + sf_strerror(nullptr))};
  }
  if (info.channels != options.expected_channels || info.channels < 1 || info.channels > 2) {
    return std::unexpected{audio_import_error("audio source channel count does not match the import")};
  }
  if (info.frames <= 0 || info.samplerate <= 0) {
    return std::unexpected{audio_import_error("audio source must contain frames at a valid sample rate")};
  }
  const auto output_rate = choose_sampler_sample_rate(
      static_cast<std::uint32_t>(info.samplerate), options.target_sample_rate);
  if (!output_rate) return std::unexpected{output_rate.error()};
  const bool resampled = *output_rate != static_cast<std::uint32_t>(info.samplerate);
  const bool native_pcm16 = (info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_PCM_16 && !resampled;
  const auto channels = static_cast<std::size_t>(info.channels);
  if (static_cast<std::uint64_t>(info.frames) >
      std::numeric_limits<std::size_t>::max() / channels) {
    return std::unexpected{audio_import_error("audio source is too large")};
  }
  std::vector<std::int16_t> quantized;
  std::uint64_t clipped{};
  if (native_pcm16) {
    quantized.resize(static_cast<std::size_t>(info.frames) * channels);
    const auto read = sf_readf_short(source.get(), quantized.data(), info.frames);
    if (read != info.frames) return std::unexpected{audio_import_error("audio source ended before its declared frame count")};
  } else {
    std::vector<double> floating(static_cast<std::size_t>(info.frames) * channels);
    const auto read = sf_readf_double(source.get(), floating.data(), info.frames);
    if (read != info.frames) return std::unexpected{audio_import_error("audio source ended before its declared frame count")};
    if (!std::ranges::all_of(floating, [](double value) { return std::isfinite(value); })) {
      return std::unexpected{audio_import_error("source audio contains NaN or infinite samples")};
    }
    if (resampled) {
      const auto estimated = static_cast<std::size_t>(
          std::ceil(static_cast<double>(info.frames) * *output_rate / info.samplerate)) + 16U;
      std::vector<double> converted(estimated * channels);
      std::size_t input_done{};
      std::size_t output_done{};
      const auto io = soxr_io_spec(SOXR_FLOAT64_I, SOXR_FLOAT64_I);
      const auto quality = soxr_quality_spec(SOXR_VHQ, 0);
      const auto error = soxr_oneshot(
          static_cast<double>(info.samplerate), static_cast<double>(*output_rate),
          static_cast<unsigned>(channels),
          floating.data(), static_cast<std::size_t>(info.frames), &input_done,
          converted.data(), estimated, &output_done, &io, &quality, nullptr);
      if (error != nullptr || input_done != static_cast<std::size_t>(info.frames)) {
        return std::unexpected{audio_import_error(
            std::string{"VHQ sample-rate conversion failed: "} + (error == nullptr ? "incomplete input" : error))};
      }
      converted.resize(output_done * channels);
      floating = std::move(converted);
    }
    const bool reduces_precision = (info.format & SF_FORMAT_SUBMASK) != SF_FORMAT_PCM_U8 &&
                                   (info.format & SF_FORMAT_SUBMASK) != SF_FORMAT_PCM_S8 &&
                                   (info.format & SF_FORMAT_SUBMASK) != SF_FORMAT_PCM_16;
    std::vector<double> dither_first;
    std::vector<double> dither_second;
    if (resampled || reduces_precision) {
      NumpyPcg64 random;
      dither_first.reserve(floating.size());
      dither_second.reserve(floating.size());
      for (std::size_t index = 0; index < floating.size(); ++index) {
        dither_first.push_back(random.next_double());
      }
      for (std::size_t index = 0; index < floating.size(); ++index) {
        dither_second.push_back(random.next_double());
      }
    }
    quantized.reserve(floating.size());
    for (std::size_t index = 0; index < floating.size(); ++index) {
      const auto sample = floating[index];
      auto scaled = sample * 32'768.0;
      if (scaled < -32'768.0 || scaled > 32'767.0) ++clipped;
      if (resampled || reduces_precision) {
        scaled += dither_first[index] - dither_second[index];
      }
      scaled = std::floor(scaled + 0.5);
      scaled = std::clamp(scaled, -32'768.0, 32'767.0);
      quantized.push_back(static_cast<std::int16_t>(scaled));
    }
  }
  ImportedAudio result;
  result.source_path = path;
  result.source_format = format_name(info.format);
  result.source_subtype = subtype_name(info.format);
  result.source_channels = static_cast<std::uint8_t>(info.channels);
  result.source_sample_rate = static_cast<std::uint32_t>(info.samplerate);
  result.output_sample_rate = *output_rate;
  result.output_frames = quantized.size() / channels;
  result.pcm_channels = split_pcm16(quantized, channels);
  result.resampled = resampled;
  result.quantized = !native_pcm16;
  result.clipped_samples = clipped;
  return result;
}

}  // namespace axk
