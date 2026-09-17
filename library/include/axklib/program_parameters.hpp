#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "axklib/sampler_model.hpp"

namespace axk {

struct ProgramChannelMap {
    std::array<std::optional<bool>, 16> a{};
    std::array<std::optional<bool>, 16> b{};
};

struct ProgramPortamentoParameters {
    std::optional<std::uint8_t> type{};
    std::optional<std::uint8_t> rate{};
    std::optional<std::uint8_t> time{};
};

struct ProgramLfoParameters {
    std::optional<std::uint8_t> cycle{};
    std::optional<std::uint8_t> sync{};
    std::optional<std::uint8_t> wave{};
    std::optional<std::uint8_t> initial_phase{};
    std::optional<std::uint8_t> tempo{};
    std::optional<std::int8_t> reset_channel{};
    std::optional<std::int8_t> reset_note{};
    std::optional<std::uint8_t> sample_hold_speed{};
};

enum class ProgramStepWaveSlope : std::uint8_t { none, rising, falling, both };

struct ProgramStepWaveParameters {
    std::optional<std::uint8_t> step_count{};
    std::optional<ProgramStepWaveSlope> slope{};
    std::array<std::optional<std::uint8_t>, 16> values{};
};

struct ProgramAdOutputParameters {
    std::optional<std::uint8_t> destination{};
    std::optional<std::uint8_t> level{};
};

struct ProgramAdChannelParameters {
    std::optional<std::int8_t> pan{};
    ProgramAdOutputParameters output1;
    ProgramAdOutputParameters output2;
};

struct ProgramAdParameters {
    std::optional<bool> enabled{};
    std::optional<std::uint8_t> source{};
    ProgramAdChannelParameters left;
    ProgramAdChannelParameters right;
};

struct ProgramControllerParameters {
    std::optional<std::uint8_t> device{};
    std::optional<std::uint8_t> function{};
    std::optional<std::uint8_t> type{};
    std::optional<std::int8_t> range{};
};

struct ProgramEffectParameters {
    std::optional<bool> enabled{};
    std::optional<std::uint8_t> input_level{};
    std::optional<std::uint8_t> output_level{};
    std::optional<std::int8_t> pan{};
    std::optional<std::int8_t> width{};
    std::optional<std::uint8_t> destination{};
    std::optional<std::uint8_t> type{};
    std::array<std::optional<std::uint16_t>, 16> parameters{};
};

// Omitted leaves preserve stored values in updates and use defaults in fresh Programs.
struct ProgramParameters {
    std::optional<std::uint8_t> level{};
    std::optional<std::int8_t> transpose{};
    ProgramChannelMap controller_reset;
    ProgramChannelMap note_toggle;
    ProgramPortamentoParameters portamento;
    ProgramLfoParameters lfo;
    ProgramStepWaveParameters step_wave;
    ProgramAdParameters ad;
    std::array<ProgramControllerParameters, 4> controllers{};
    std::array<std::optional<std::uint8_t>, 2> effect_connections{};
    std::array<ProgramEffectParameters, 6> effects{};
};

} // namespace axk
