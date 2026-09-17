#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace axk {

enum class SystemMlanMidiInput : std::uint8_t { midi, mlan_a, mlan_b };
enum class SystemMlanAudioInput : std::uint8_t { ad_in, mlan };

// Revision-1 SYSTEM2 routing preferences. An absent decoded value is invalid
// stored data; an absent patch field preserves its byte. mlan_b is A5000-only.
struct SystemMlanParameters {
    std::optional<SystemMlanMidiInput> midi_input;
    std::optional<SystemMlanAudioInput> audio_input;

    friend bool operator==(const SystemMlanParameters &, const SystemMlanParameters &) = default;
};

struct DecodedSystemMlan {
    SystemMlanParameters parameters;
    // Includes interface initialization state, not writable routing preferences.
    std::array<std::byte, 16> raw_bytes{};

    friend bool operator==(const DecodedSystemMlan &, const DecodedSystemMlan &) = default;
};

} // namespace axk
