#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace axk {

struct SystemEffectFavorites {
    // Raw effect ID, not the displayed effect number or physical favorites row.
    std::uint16_t raw_type{};
    std::uint8_t parameter_count{};
    std::uint8_t editable_position_count{};
    // Zero-based visible parameter indices. Retained values >= parameter_count
    // are unavailable, not normalized. Positions beyond editable_position_count
    // remain stored but cannot be targeted by the typed writer.
    std::array<std::uint8_t, 4> selections{};
};

struct DecodedSystemFavorites {
    // Ordinary effects in raw-ID order: 0..54 for SYSTEM, 0..96 for SYSTEM2.
    std::vector<SystemEffectFavorites> effects;
    // Complete region including unused rows: 128 or 256 bytes respectively.
    std::vector<std::byte> raw_bytes;
};

struct SystemEffectFavoritesPatch {
    std::uint16_t raw_type{};
    // Absent positions retain their exact nibble. Repeated parameter selections
    // are legal; multiple patches for the same raw_type are rejected.
    std::array<std::optional<std::uint8_t>, 4> selections;
};

} // namespace axk
