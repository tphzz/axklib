#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "axklib/error.hpp"

namespace axk::typhoon {
struct Wave {
    std::uint32_t frames{};
    std::uint32_t sample_rate{};
    std::uint16_t bits{};
    std::optional<std::uint8_t> root_key;
    std::int8_t detune_cents{};
    std::optional<std::pair<std::uint32_t, std::uint32_t>> loop; // Inclusive frame indices.
};
[[nodiscard]] Result<Wave> inspect_wave(std::span<const std::byte> bytes);
[[nodiscard]] Result<std::vector<std::int16_t>> read_pcm16(std::span<const std::byte> bytes, std::uint32_t first_frame,
                                                           std::uint32_t frame_count);
} // namespace axk::typhoon
