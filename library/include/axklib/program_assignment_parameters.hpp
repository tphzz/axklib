#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "axklib/sampler_model.hpp"

namespace axk {

struct ProgramReceiveInherit {
    friend bool operator==(const ProgramReceiveInherit &, const ProgramReceiveInherit &) = default;
};
struct ProgramReceiveBasic {
    friend bool operator==(const ProgramReceiveBasic &, const ProgramReceiveBasic &) = default;
};
struct ProgramReceiveChannel {
    MidiPort port{MidiPort::a};
    std::uint8_t channel{1};
    friend bool operator==(const ProgramReceiveChannel &, const ProgramReceiveChannel &) = default;
};
using ProgramReceiveSetting = std::variant<ProgramReceiveInherit, ProgramReceiveBasic, ProgramReceiveChannel>;

enum class ProgramInheritableSwitch : std::int8_t { inherit = -1, off = 0, on = 1 };

struct ProgramAssignmentParameters {
    std::optional<ProgramReceiveSetting> receive{};
    std::optional<std::int8_t> level_offset{};
    std::optional<std::int8_t> velocity_sensitivity_offset{};
    std::optional<std::int8_t> pan_offset{};
    std::optional<std::int8_t> high_velocity_crossfade_offset{};
    std::optional<std::int8_t> fine_tune_offset{};
    std::optional<std::int8_t> low_velocity_crossfade_offset{};
    std::optional<std::int8_t> coarse_tune_offset{};
    // Output and alternate-group replacements use -1 for explicit inheritance.
    std::optional<std::int8_t> output1{};
    std::optional<std::int8_t> output2{};
    std::optional<std::uint8_t> key_high{};
    std::optional<std::uint8_t> key_low{};
    std::optional<std::int8_t> key_shift{};
    std::optional<std::uint8_t> velocity_high{};
    std::optional<std::uint8_t> velocity_low{};
    std::optional<ProgramInheritableSwitch> portamento{};
    std::optional<ProgramInheritableSwitch> mono{};
    std::optional<ProgramInheritableSwitch> key_crossfade{};
    std::optional<std::int8_t> alternate_group{};
    std::optional<std::int8_t> amp_attack_offset{};
    std::optional<std::int8_t> amp_decay_offset{};
    std::optional<std::int8_t> amp_release_offset{};
    std::optional<std::int8_t> filter_cutoff_offset{};
    std::optional<std::int8_t> filter_gain_offset{};
    std::optional<std::int8_t> filter_q_offset{};
    std::optional<std::int8_t> filter_cutoff_distance_offset{};
    std::optional<std::int8_t> output1_level_offset{};
    std::optional<std::int8_t> output2_level_offset{};
    std::optional<bool> midi_control{};
};

struct ProgramAssignmentParameterPatch {
    std::size_t ordinal{};
    std::string expected_target_kind;
    std::string expected_target_name;
    ProgramAssignmentParameters parameters;
};

} // namespace axk
