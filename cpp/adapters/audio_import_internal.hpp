#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/error.hpp"

namespace axk::audio_import_detail {

inline constexpr std::string_view dither_algorithm{"axk-tpdf-pcg32-v1"};

struct QuantizedPcm16 {
  std::vector<std::int16_t> samples;
  std::uint64_t clipped_samples{};
};

struct SourceAudioInfo {
  std::string format;
  std::string subtype;
  std::size_t channels{};
  std::size_t frames{};
  std::uint32_t sample_rate{};
  bool is_pcm16{};
  bool reduces_precision{};
};

Result<SourceAudioInfo> inspect_sndfile(const std::filesystem::path& path,
                                       std::size_t expected_channels);

Result<std::vector<std::int16_t>> decode_sndfile_pcm16(
    const std::filesystem::path& path, const SourceAudioInfo& info);

Result<std::vector<double>> decode_sndfile_float64(const std::filesystem::path& path,
                                                   const SourceAudioInfo& info);

Result<std::vector<double>> resample_vhq(std::span<const double> samples,
                                         std::size_t channels,
                                         std::uint32_t source_rate,
                                         std::uint32_t output_rate);

Result<QuantizedPcm16> quantize_pcm16(std::span<const double> samples, bool dither);

std::vector<std::vector<std::byte>> split_pcm16(std::span<const std::int16_t> samples,
                                                std::size_t channels);

}  // namespace axk::audio_import_detail
