#include "axklib/writer.hpp"

#include "audio_import_internal.hpp"

#include <algorithm>
#include <array>
#include <ranges>

namespace axk {
namespace {

constexpr std::array<std::uint32_t, 12> rates{
    4'000, 5'512, 6'000, 8'000, 11'025, 12'000,
    16'000, 22'050, 24'000, 32'000, 44'100, 48'000};

Error audio_import_error(std::string message) {
  return make_error(ErrorCode::audio_unsupported_format, ErrorCategory::audio, std::move(message));
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
  const auto source = audio_import_detail::inspect_sndfile(path, options.expected_channels);
  if (!source)
    return std::unexpected{source.error()};
  const auto output_rate =
      choose_sampler_sample_rate(source->sample_rate, options.target_sample_rate);
  if (!output_rate) return std::unexpected{output_rate.error()};
  const bool resampled = *output_rate != source->sample_rate;
  const bool native_pcm16 = source->is_pcm16 && !resampled;
  const bool dither = resampled || source->reduces_precision;
  std::vector<std::int16_t> quantized;
  std::uint64_t clipped{};
  if (native_pcm16) {
    auto decoded = audio_import_detail::decode_sndfile_pcm16(path, *source);
    if (!decoded)
      return std::unexpected{decoded.error()};
    quantized = std::move(*decoded);
  } else {
    auto decoded = audio_import_detail::decode_sndfile_float64(path, *source);
    if (!decoded)
      return std::unexpected{decoded.error()};
    auto floating = std::move(*decoded);
    if (resampled) {
      auto converted = audio_import_detail::resample_vhq(
          floating, source->channels, source->sample_rate, *output_rate);
      if (!converted)
        return std::unexpected{converted.error()};
      floating = std::move(*converted);
    }
    auto converted = audio_import_detail::quantize_pcm16(floating, dither);
    if (!converted)
      return std::unexpected{converted.error()};
    quantized = std::move(converted->samples);
    clipped = converted->clipped_samples;
  }
  ImportedAudio result;
  result.source_path = path;
  result.source_format = source->format;
  result.source_subtype = source->subtype;
  result.source_channels = static_cast<std::uint8_t>(source->channels);
  result.source_sample_rate = source->sample_rate;
  result.output_sample_rate = *output_rate;
  result.output_frames = quantized.size() / source->channels;
  result.pcm_channels = audio_import_detail::split_pcm16(quantized, source->channels);
  result.resampled = resampled;
  result.quantized = !native_pcm16;
  if (dither)
    result.dither_algorithm = std::string{audio_import_detail::dither_algorithm};
  result.clipped_samples = clipped;
  return result;
}

}  // namespace axk
