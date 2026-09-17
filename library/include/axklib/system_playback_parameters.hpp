#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "axklib/sampler_model.hpp"

namespace axk {

enum class SystemDigitalOutputBits : std::uint8_t { bits20 = 20, bits24 = 24 };

// Sequence port selects both the recording input and playback channel group.
// Absent decoded values are invalid stored data; absent patch values are unchanged.
struct SystemPlaybackParameters {
    std::optional<MidiPort> sequence_midi_port;
    std::optional<SystemDigitalOutputBits> digital_output_bits;

    friend bool operator==(const SystemPlaybackParameters &, const SystemPlaybackParameters &) = default;
};

struct DecodedSystemPlayback {
    SystemPlaybackParameters parameters;
    std::array<std::byte, 16> sequence_raw_bytes{};
    std::array<std::byte, 8> digital_output_raw_bytes{};

    friend bool operator==(const DecodedSystemPlayback &, const DecodedSystemPlayback &) = default;
};

} // namespace axk
