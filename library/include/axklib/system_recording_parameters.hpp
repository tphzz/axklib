#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "axklib/program_parameters.hpp"

namespace axk {

// Numeric selections retain their stored encoding. Frequency depends on the
// selected input; it is not a fixed sample rate. Absent decoded leaves are
// undecoded; absent patch leaves retain their stored values unless a documented
// input/type dependency requires an adjustment.
struct SystemRecordingParameters {
    std::optional<std::uint8_t> record_type;
    std::optional<bool> stereo;
    std::optional<std::uint8_t> input;
    std::optional<std::uint8_t> frequency_selection;
    std::optional<std::uint8_t> pre_trigger_time;
    std::optional<std::uint8_t> start_trigger;
    std::optional<std::uint8_t> stop_trigger;
    std::optional<std::uint8_t> start_edge_level;
    std::optional<std::uint8_t> stop_edge_level;
    std::optional<std::int8_t> map_destination;
    std::optional<std::int8_t> key_low;
    std::optional<std::uint8_t> key_high;
    std::optional<std::int8_t> original_key;
    std::optional<bool> auto_normalize;
    std::optional<std::int8_t> external_scsi_id;
    std::optional<std::uint8_t> external_track;
    std::optional<std::uint8_t> external_index;
    std::optional<std::uint8_t> monitor_output;
    std::optional<std::uint8_t> monitor_level;
    std::optional<std::uint8_t> click_level;
    std::optional<std::uint16_t> click_tempo_hundredths;
    std::optional<std::uint8_t> click_beat;
    std::optional<bool> monitor_enabled;
    std::optional<bool> map_auto;
    std::optional<std::uint8_t> map_original_key;
    std::optional<bool> map_all_keys;
    std::optional<std::uint8_t> ad_input_gain;
};

struct SystemRecordingPatch {
    SystemRecordingParameters configuration;
    std::array<ProgramEffectParameters, 3> effects;
};

struct DecodedSystemRecording {
    SystemRecordingParameters parameters;
    std::array<ProgramEffectParameters, 3> effects;
    // Three 40-byte effect blocks, followed by 34 native or 56 current bytes.
    std::vector<std::byte> raw_bytes;
};

} // namespace axk
