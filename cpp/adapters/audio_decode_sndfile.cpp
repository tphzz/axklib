#include "audio_import_internal.hpp"

#include "axklib/utf8.hpp"

#include <limits>
#include <memory>

#include <sndfile.h>

namespace axk::audio_import_detail {
namespace {

Error audio_error(std::string message) {
  return make_error(ErrorCode::audio_unsupported_format, ErrorCategory::audio, std::move(message));
}

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

using SndfileHandle = std::unique_ptr<SNDFILE, decltype(&sf_close)>;

Result<SndfileHandle> open_sndfile(const std::filesystem::path& path, SF_INFO& info) {
#ifdef _WIN32
  SndfileHandle source{sf_wchar_open(path.c_str(), SFM_READ, &info), &sf_close};
#else
  const auto path_text = path.string();
  SndfileHandle source{sf_open(path_text.c_str(), SFM_READ, &info), &sf_close};
#endif
  if (!source) {
    return std::unexpected{audio_error("cannot decode audio source " +
                                       text::path_to_utf8(path) +
                                       ": " + sf_strerror(nullptr))};
  }
  return source;
}

Result<SndfileHandle> open_verified(const std::filesystem::path& path,
                                    const SourceAudioInfo& expected, SF_INFO& info) {
  auto source = open_sndfile(path, info);
  if (!source)
    return std::unexpected{source.error()};
  if (info.frames < 0 || static_cast<std::uint64_t>(info.frames) != expected.frames ||
      info.channels < 0 || static_cast<std::size_t>(info.channels) != expected.channels ||
      info.samplerate < 0 || static_cast<std::uint32_t>(info.samplerate) != expected.sample_rate) {
    return std::unexpected{audio_error("audio source metadata changed while it was being read")};
  }
  return source;
}

}  // namespace

Result<SourceAudioInfo> inspect_sndfile(const std::filesystem::path& path,
                                       std::size_t expected_channels) {
  SF_INFO info{};
  auto source = open_sndfile(path, info);
  if (!source)
    return std::unexpected{source.error()};
  if (info.channels < 1 || info.channels > 2 ||
      static_cast<std::size_t>(info.channels) != expected_channels) {
    return std::unexpected{audio_error("audio source channel count does not match the import")};
  }
  if (info.frames <= 0 || info.samplerate <= 0) {
    return std::unexpected{audio_error("audio source must contain frames at a valid sample rate")};
  }
  const auto channels = static_cast<std::size_t>(info.channels);
  if (static_cast<std::uint64_t>(info.frames) >
      std::numeric_limits<std::size_t>::max() / channels) {
    return std::unexpected{audio_error("audio source is too large")};
  }
  const auto subtype = info.format & SF_FORMAT_SUBMASK;
  return SourceAudioInfo{
      .format = format_name(info.format),
      .subtype = subtype_name(info.format),
      .channels = channels,
      .frames = static_cast<std::size_t>(info.frames),
      .sample_rate = static_cast<std::uint32_t>(info.samplerate),
      .is_pcm16 = subtype == SF_FORMAT_PCM_16,
      .reduces_precision =
          subtype != SF_FORMAT_PCM_U8 && subtype != SF_FORMAT_PCM_S8 && subtype != SF_FORMAT_PCM_16,
  };
}

Result<std::vector<std::int16_t>> decode_sndfile_pcm16(
    const std::filesystem::path& path, const SourceAudioInfo& expected) {
  SF_INFO info{};
  auto source = open_verified(path, expected, info);
  if (!source)
    return std::unexpected{source.error()};
  std::vector<std::int16_t> samples(expected.frames * expected.channels);
  const auto read = sf_readf_short(source->get(), samples.data(), info.frames);
  if (read != info.frames) {
    return std::unexpected{audio_error("audio source ended before its declared frame count")};
  }
  return samples;
}

Result<std::vector<double>> decode_sndfile_float64(const std::filesystem::path& path,
                                                   const SourceAudioInfo& expected) {
  SF_INFO info{};
  auto source = open_verified(path, expected, info);
  if (!source)
    return std::unexpected{source.error()};
  std::vector<double> samples(expected.frames * expected.channels);
  const auto read = sf_readf_double(source->get(), samples.data(), info.frames);
  if (read != info.frames) {
    return std::unexpected{audio_error("audio source ended before its declared frame count")};
  }
  return samples;
}

}  // namespace axk::audio_import_detail
