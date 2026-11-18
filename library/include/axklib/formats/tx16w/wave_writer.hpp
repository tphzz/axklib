#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "axklib/error.hpp"

namespace axk::tx16w {

struct WaveEncoding {
    std::uint32_t sample_rate{};
    std::uint32_t attack_frames{};
    std::uint32_t repeat_frames{};
    bool looped{};
};

struct WaveFileSize {
    std::uint64_t native_bytes{};
    std::uint64_t padded_bytes{};
};
[[nodiscard]] WaveFileSize wave_file_size(std::uint32_t frames) noexcept;

// Encodes native mono frames on the decoder's signed PCM16 scale, without quantizing or resampling.
[[nodiscard]] Result<std::vector<std::byte>> encode_wave(const WaveEncoding &encoding,
                                                         std::span<const std::int16_t> pcm);

} // namespace axk::tx16w
