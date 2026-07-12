#include "audio_import_internal.hpp"

namespace axk::audio_import_detail {

std::vector<std::vector<std::byte>> split_pcm16(std::span<const std::int16_t> samples,
                                                std::size_t channels) {
  const auto frames = samples.size() / channels;
  std::vector<std::vector<std::byte>> result(channels);
  for (auto &channel : result)
    channel.reserve(frames * 2U);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    for (std::size_t channel = 0; channel < channels; ++channel) {
      const auto value = static_cast<std::uint16_t>(samples[frame * channels + channel]);
      result[channel].push_back(static_cast<std::byte>(value & 0xffU));
      result[channel].push_back(static_cast<std::byte>(value >> 8U));
    }
  }
  return result;
}

} // namespace axk::audio_import_detail
