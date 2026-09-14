#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace axk {

enum class ProgramMode : std::uint8_t { single, multi };

struct SystemGlobalEqBandParameters {
    std::optional<std::uint8_t> frequency_selection;
    std::optional<std::uint8_t> gain_selection;
    std::optional<std::uint8_t> width_selection;
};

// Numeric selections use stored encodings, not display units. Absent decoded
// values are invalid or unavailable in this file family. Reading does not
// apply load-time normalization, active-hardware limits or edit dependencies.
// For targeted patches, absent leaves mean retain, subject to the documented
// dependent writes. Readable fields may have stricter write availability.
struct SystemGlobalParameters {
    std::optional<std::int8_t> master_fine_tune;
    std::optional<std::int8_t> master_coarse_tune;
    std::optional<std::int8_t> master_transpose;
    std::optional<std::uint8_t> velocity_curve_selection;
    std::optional<std::uint8_t> basic_receive_channel_selection;
    std::optional<bool> omni;
    std::optional<bool> program_change_enabled;
    // Basic Receive is 16; current B01..B16 are 17..32. Knobs also allow -1
    // for audition. These differ from the Basic Receive field's 0..31 encoding.
    std::array<std::optional<std::int8_t>, 4> knob_transmit_channels;
    std::array<std::optional<std::uint8_t>, 4> knob_control_devices;
    std::array<std::optional<std::uint8_t>, 6> function_key_transmit_channels;
    std::array<std::optional<std::uint8_t>, 6> function_key_notes;
    std::array<std::optional<std::uint8_t>, 6> function_key_velocities;
    // Low boost, low, mid, high. Low boost has no width; gain 64 is neutral.
    std::array<SystemGlobalEqBandParameters, 4> total_eq;
    std::optional<std::uint8_t> stereo_to_assignable_selection;
    std::optional<std::uint8_t> stereo_output_level_offset;
    std::optional<bool> wave_length_lock;
    std::optional<bool> wave_auto_zero;
    std::optional<bool> wave_auto_snap;
    std::optional<bool> audition_with_easy_edit;
    std::optional<bool> audition_with_effects;
    std::optional<bool> play_and_load;

    // Stored selections: SYSTEM type 0..4 / variation 0..3;
    // SYSTEM2 type 0..9 / variation 0..7. Writes support presets 0..4 and
    // SYSTEM2 User1..User3 (5..7) only with a supported stored recipe.
    std::optional<std::uint8_t> remix_type_selection;
    std::optional<std::uint8_t> remix_variation_selection;

    // SYSTEM2 only. All 32 parts are retained without inferring A4000/A5000
    // hardware from the storage revision. Program numbers are directly 1..128.
    std::optional<ProgramMode> program_mode;
    std::array<std::optional<std::uint8_t>, 32> part_program_numbers;
    std::optional<bool> remix_auto_audition;
    std::optional<bool> knob_midi_out;
    std::optional<bool> function_key_midi_out;
    std::optional<std::uint8_t> remix_zone_start;
    std::optional<std::uint8_t> remix_zone_end;
    std::array<std::optional<std::uint8_t>, 5> assignable_output_level_offsets;
};

// Stored recipe lanes, including bytes after any duration terminator. This is
// a lossless diagnostic view, not validation or permission to execute a recipe.
struct SystemRegisteredRemix {
    std::array<std::uint8_t, 24> duration_codes{};
    std::array<std::uint8_t, 24> random_choices{};
    std::array<std::uint8_t, 24> processing_flags{};
};

struct DecodedSystemGlobal {
    SystemGlobalParameters parameters;
    std::optional<std::array<SystemRegisteredRemix, 5>> registered_remix;
    // SYSTEM2 working state, cleared on load; not a startup-Program preference.
    std::optional<std::uint8_t> working_program_marker;
    // 46 bytes for SYSTEM, 448 for SYSTEM2; includes all undecoded bytes.
    std::vector<std::byte> raw_bytes;
};

} // namespace axk
