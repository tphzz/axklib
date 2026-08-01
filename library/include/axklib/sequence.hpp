#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/export.hpp"
#include "axklib/object.hpp"

namespace axk {

enum class SequenceSystemExclusivePolicy : std::uint8_t {
    reject,
    exclude,
    preserve,
};

struct SequenceMidiControllerCount {
    std::uint8_t controller{};
    std::uint64_t event_count{};

    friend bool operator==(const SequenceMidiControllerCount &, const SequenceMidiControllerCount &) = default;
};

struct SequenceMidiInspection {
    std::uint16_t format{};
    std::uint16_t track_count{};
    std::uint16_t ticks_per_quarter_note{};
    std::uint32_t end_tick{};
    std::uint64_t event_count{};
    std::uint64_t channel_event_count{};
    std::uint64_t meta_event_count{};
    std::uint64_t system_exclusive_event_count{};
    std::uint64_t system_exclusive_data_bytes{};
    std::vector<SequenceMidiControllerCount> controllers;
    std::vector<std::string> system_exclusive_manufacturer_ids;
    bool system_exclusive_preservation_supported{};
};

AXK_API Result<CurrentSequence> decode_current_sequence(std::span<const std::byte> payload);

// Converts the native current SEQU timeline to Standard MIDI File
// format 0. The result contains one MTrk chunk and preserves native ticks.
AXK_API Result<std::vector<std::byte>> sequence_to_smf0(const CurrentSequence &sequence);

// Inspects a bounded Standard MIDI File format 0 without interpreting or
// returning opaque System Exclusive payload bytes.
AXK_API Result<SequenceMidiInspection> inspect_smf0(std::span<const std::byte> smf);

// Imports a bounded Standard MIDI File format 0 into the current SEQU profile.
// Input timing is normalized to the A-series 96 PPQN timeline.
AXK_API Result<std::vector<std::byte>> smf0_to_current_sequence(std::span<const std::byte> smf,
                                                                std::string_view sequence_name,
                                                                SequenceSystemExclusivePolicy system_exclusive_policy);

} // namespace axk
