#include "axklib/program_parameter_codec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>

#include "axklib/bytes.hpp"

namespace axk::detail {

bool has_program_parameter_values(const ProgramParameters &value) {
    const auto any = [](const auto &values) {
        return std::ranges::any_of(values, [](const auto &leaf) { return leaf.has_value(); });
    };
    const auto ad = [](const ProgramAdChannelParameters &channel) {
        return channel.pan || channel.output1.destination || channel.output1.level || channel.output2.destination ||
               channel.output2.level;
    };
    return value.level || value.transpose || any(value.controller_reset.a) || any(value.controller_reset.b) ||
           any(value.note_toggle.a) || any(value.note_toggle.b) || value.portamento.type || value.portamento.rate ||
           value.portamento.time || value.lfo.cycle || value.lfo.sync || value.lfo.wave || value.lfo.initial_phase ||
           value.lfo.tempo || value.lfo.reset_channel || value.lfo.reset_note || value.lfo.sample_hold_speed ||
           value.step_wave.step_count || value.step_wave.slope || any(value.step_wave.values) || value.ad.enabled ||
           value.ad.source || ad(value.ad.left) || ad(value.ad.right) || any(value.effect_connections) ||
           std::ranges::any_of(value.controllers,
                               [](const auto &controller) {
                                   return controller.device || controller.function || controller.type ||
                                          controller.range;
                               }) ||
           std::ranges::any_of(value.effects, [&](const auto &effect) {
               return effect.enabled || effect.input_level || effect.output_level || effect.pan || effect.width ||
                      effect.destination || effect.type || any(effect.parameters);
           });
}

namespace {

constexpr std::array<std::uint8_t, 7> step_counts{2, 3, 4, 6, 8, 12, 16};

template <typename T> bool outside(const std::optional<T> &value, int minimum, int maximum) {
    return value && (static_cast<int>(*value) < minimum || static_cast<int>(*value) > maximum);
}

bool any_channel(const std::array<std::optional<bool>, 16> &values) {
    return std::ranges::any_of(values, [](const auto &value) { return value.has_value(); });
}

Result<void> validate(const ProgramParameters &value, ASeriesModel model) {
    if (model != ASeriesModel::a3000 && model != ASeriesModel::a4000 && model != ASeriesModel::a5000)
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Program parameter target model is unsupported")};
    const auto native = model == ASeriesModel::a3000;
    const auto extended = model == ASeriesModel::a5000;
    const auto &lfo = value.lfo;
    const auto &portamento = value.portamento;
    if (outside(value.level, 0, 127) || outside(value.transpose, -127, 127) || outside(portamento.type, 0, 3) ||
        outside(portamento.rate, 1, 127) || outside(portamento.time, 1, 127) || outside(lfo.cycle, 0, 6) ||
        outside(lfo.sync, 0, extended ? 2 : 1) || outside(lfo.wave, 0, native ? 5 : 6) ||
        outside(lfo.initial_phase, 0, 3) || outside(lfo.tempo, 25, 250) ||
        outside(lfo.reset_channel, -2, extended ? 32 : 16) || outside(lfo.reset_note, -1, 127) ||
        outside(lfo.sample_hold_speed, 0, 127) || outside(value.step_wave.slope, 0, 3) ||
        (value.step_wave.step_count &&
         std::ranges::find(step_counts, *value.step_wave.step_count) == step_counts.end()) ||
        std::ranges::any_of(value.step_wave.values, [](const auto &step) { return outside(step, 0, 127); })) {
        return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                          "Program parameter is outside its supported domain")};
    }
    if (!extended && (any_channel(value.controller_reset.b) || any_channel(value.note_toggle.b)))
        return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                          "Program MIDI port B parameters require an A5000 target model")};
    if (native && (value.step_wave.step_count || value.step_wave.slope ||
                   std::ranges::any_of(value.step_wave.values, [](const auto &step) { return step.has_value(); }) ||
                   value.ad.right.pan || value.ad.right.output1.destination || value.ad.right.output1.level ||
                   value.ad.right.output2.destination || value.ad.right.output2.level))
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "A3000 registered Programs have no StepWave or independent right A/D route")};
    if (outside(value.ad.source, 0, 2) || outside(value.effect_connections[0], 0, 4) ||
        outside(value.effect_connections[1], 0, 4) || (!extended && value.effect_connections[1]))
        return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                          "Program A/D source or effect connection is not supported for the model")};
    for (const auto *channel : {&value.ad.left, &value.ad.right}) {
        if (outside(channel->pan, -63, 63) ||
            outside(channel->output1.destination, 0,
                    native     ? 4
                    : extended ? 12
                               : 9) ||
            outside(channel->output2.destination, 0,
                    native     ? 5
                    : extended ? 12
                               : 9) ||
            outside(channel->output1.level, 0, 127) || outside(channel->output2.level, 0, 127))
            return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                              "Program A/D parameter is outside its supported domain")};
    }
    for (const auto &controller : value.controllers) {
        if (outside(controller.device, 0, native ? 125 : 126) ||
            outside(controller.function, 0,
                    native     ? 63
                    : extended ? 128
                               : 71) ||
            outside(controller.type, 0, 3) || outside(controller.range, -63, 63))
            return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                              "Program controller parameter is outside its supported domain")};
    }
    return {};
}

template <typename T> void put(std::span<std::byte> bytes, std::size_t offset, const std::optional<T> &value) {
    if (value)
        bytes[offset] = static_cast<std::byte>(static_cast<std::uint8_t>(*value));
}

template <typename T>
void put_bits(std::span<std::byte> bytes, std::size_t offset, unsigned mask, unsigned shift,
              const std::optional<T> &value) {
    if (value) {
        const auto original = std::to_integer<unsigned>(bytes[offset]);
        bytes[offset] = static_cast<std::byte>((original & ~mask) | ((static_cast<unsigned>(*value) << shift) & mask));
    }
}

void put_channels(std::span<std::byte> bytes, std::size_t offset, const std::array<std::optional<bool>, 16> &values) {
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto byte = offset + (index < 8U ? 1U : 0U);
        const auto bit = static_cast<unsigned>(index % 8U);
        put_bits(bytes, byte, 1U << bit, bit, values[index]);
    }
}

void read_channels(const ByteReader &reader, std::size_t offset, std::array<std::optional<bool>, 16> &values) {
    const auto bits = *reader.be16(offset);
    for (std::size_t index = 0; index < values.size(); ++index)
        values[index] = (bits & (1U << index)) != 0U;
}

template <typename T> std::optional<T> known(T value, int minimum, int maximum) {
    return static_cast<int>(value) < minimum || static_cast<int>(value) > maximum ? std::nullopt
                                                                                  : std::optional<T>{value};
}

void write_ad_outputs(std::span<std::byte> bytes, std::size_t offset, const ProgramAdChannelParameters &value) {
    put(bytes, offset, value.output1.destination);
    put(bytes, offset + 1U, value.output1.level);
    put(bytes, offset + 2U, value.output2.destination);
    put(bytes, offset + 3U, value.output2.level);
}

void read_ad_outputs(const ByteReader &reader, std::size_t offset, ProgramAdChannelParameters &value) {
    value.output1.destination = known(*reader.u8(offset), 0, 12);
    value.output1.level = known(*reader.u8(offset + 1U), 0, 127);
    value.output2.destination = known(*reader.u8(offset + 2U), 0, 12);
    value.output2.level = known(*reader.u8(offset + 3U), 0, 127);
}

void project_left_ad_outputs(std::span<std::byte> common, std::span<const std::byte, 4> current) {
    const auto output1 = std::to_integer<unsigned>(current[0]);
    const auto output2 = std::to_integer<unsigned>(current[2]);
    common[0x07U] = common[0x09U] = std::byte{};
    // Output 1 wins a shared legacy bucket. Unmatched legacy levels stay intact.
    if (output2 >= 1U && output2 <= 5U) {
        common[0x09U] = current[2];
        common[0x0aU] = current[3];
    } else if (output2 >= 6U && output2 <= 9U) {
        common[0x07U] = static_cast<std::byte>(output2 - 5U);
        common[0x08U] = current[3];
    }
    if (output1 >= 1U && output1 <= 4U) {
        common[0x07U] = current[0];
        common[0x08U] = current[1];
    } else if (output1 >= 5U && output1 <= 9U) {
        common[0x09U] = static_cast<std::byte>(output1 - 4U);
        common[0x0aU] = current[1];
    }
}

} // namespace

ProgramParameters decode_program_parameters(std::span<const std::byte> payload, const ProgLayout &layout) {
    payload = payload.first(layout.logical_size);
    const auto controls = layout.parameter_tail_offset ? *layout.parameter_tail_offset + 0x78U : 0x110U;
    ProgramParameterBlocks blocks{payload.subspan<0x80U, 0x16U>(),
                                  std::span<const std::byte, 0x10>{payload.subspan(controls, 0x10U)}, std::nullopt};
    if (layout.parameter_tail_offset)
        blocks.extended =
            std::span<const std::byte, 0x28>{payload.subspan(*layout.parameter_tail_offset + 0x88U, 0x28U)};
    return decode_program_parameter_blocks(blocks);
}

ProgramParameters decode_program_parameter_blocks(const ProgramParameterBlocks &blocks,
                                                  ProgramParameterGeneration generation) {
    const auto a3000 = generation == ProgramParameterGeneration::a3000;
    const ByteReader reader{blocks.common};
    ProgramParameters result;
    result.level = known(*reader.u8(0x0bU), 0, 127);
    result.transpose = known(*reader.s8(0x0eU), -127, 127);
    result.portamento.type = known(*reader.u8(0x10U), 0, 3);
    result.portamento.rate = known(*reader.u8(0x11U), 1, 127);
    result.portamento.time = known(*reader.u8(0x12U), 1, 127);
    read_channels(reader, 0x02U, result.controller_reset.a);
    read_channels(reader, 0x04U, result.note_toggle.a);
    const auto flags = *reader.u8(0x00U);
    const auto lfo = *reader.u8(0x01U);
    result.lfo.sync = known(static_cast<std::uint8_t>((flags >> 6U) & (a3000 ? 1U : 3U)), 0, a3000 ? 1 : 2);
    result.lfo.cycle = known(static_cast<std::uint8_t>(lfo & 7U), 0, 6);
    result.lfo.wave = known(static_cast<std::uint8_t>((lfo >> 3U) & 7U), 0, a3000 ? 5 : 6);
    result.lfo.initial_phase = static_cast<std::uint8_t>(lfo >> 6U);
    result.lfo.reset_channel = known(*reader.s8(0x0fU), -2, a3000 ? 16 : 32);
    result.lfo.sample_hold_speed = known(*reader.u8(0x13U), 0, 127);
    result.lfo.tempo = known(*reader.u8(0x14U), 25, 250);
    result.lfo.reset_note = known(*reader.s8(0x15U), -1, 127);
    result.ad.enabled = (flags & 1U) != 0U;
    result.ad.source = known(static_cast<std::uint8_t>((flags >> 1U) & 3U), 0, 2);
    result.ad.left.pan = known(*reader.s8(0x06U), -63, 63);
    if (a3000) {
        result.ad.left.output1.destination = known(*reader.u8(0x07U), 0, 4);
        result.ad.left.output1.level = known(*reader.u8(0x08U), 0, 127);
        result.ad.left.output2.destination = known(*reader.u8(0x09U), 0, 5);
        result.ad.left.output2.level = known(*reader.u8(0x0aU), 0, 127);
    }
    result.effect_connections[0] = known(static_cast<std::uint8_t>((flags >> 3U) & 7U), 0, 4);
    const ByteReader controls{blocks.controllers};
    for (std::size_t index = 0; index < result.controllers.size(); ++index) {
        const auto offset = index * 4U;
        auto &controller = result.controllers[index];
        controller.device = known(*controls.u8(offset), 0, a3000 ? 125 : 126);
        controller.function = known(*controls.u8(offset + 1U), 0, !a3000 && blocks.extended ? 128 : 63);
        controller.type = known(*controls.u8(offset + 2U), 0, 3);
        controller.range = known(*controls.s8(offset + 3U), -63, 63);
    }
    if (!a3000 && blocks.extended) {
        const ByteReader extended{*blocks.extended};
        read_channels(extended, 0x00U, result.controller_reset.b);
        read_channels(extended, 0x02U, result.note_toggle.b);
        read_ad_outputs(extended, 0x05U, result.ad.left);
        read_ad_outputs(extended, 0x0aU, result.ad.right);
        result.ad.right.pan = known(*extended.s8(0x09U), -63, 63);
        result.effect_connections[1] = known(static_cast<std::uint8_t>(*extended.u8(0x04U) & 7U), 0, 4);
        const auto step = *extended.u8(0x1eU);
        if ((step & 7U) < step_counts.size())
            result.step_wave.step_count = step_counts[step & 7U];
        result.step_wave.slope = static_cast<ProgramStepWaveSlope>((step >> 3U) & 3U);
        for (std::size_t index = 0; index < result.step_wave.values.size(); ++index)
            result.step_wave.values[index] = known(*extended.u8(0x0eU + index), 0, 127);
    }
    return result;
}

Result<void> apply_program_parameter_blocks(const MutableProgramParameterBlocks &blocks, const ProgramParameters &value,
                                            ASeriesModel model) {
    const bool native = model == ASeriesModel::a3000;
    if (native ? (blocks.extended || blocks.legacy_controllers) : (!blocks.extended || !blocks.legacy_controllers))
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Program parameter blocks do not match the target model")};
    if (auto valid = validate(value, model); !valid)
        return valid;
    const auto common = blocks.common;
    put_bits(common, 0x00U, 1U, 0U, value.ad.enabled);
    put_bits(common, 0x00U, 6U, 1U, value.ad.source);
    put_bits(common, 0x00U, 0x38U, 3U, value.effect_connections[0]);
    put(common, 0x06U, value.ad.left.pan);
    if (native)
        write_ad_outputs(common, 0x07U, value.ad.left);
    for (std::size_t index = 0; index < value.controllers.size(); ++index) {
        const auto current = blocks.controllers.subspan(index * 4U, 4U);
        std::array<std::byte, 4> original;
        std::ranges::copy(current, original.begin());
        const auto &controller = value.controllers[index];
        put(current, 0, controller.device);
        put(current, 1, controller.function);
        put(current, 2, controller.type);
        put(current, 3, controller.range);
        if (blocks.legacy_controllers && !std::ranges::equal(current, original)) {
            const auto legacy = blocks.legacy_controllers->subspan(index * 4U, 4U);
            std::ranges::copy(current, legacy.begin());
            if (std::to_integer<unsigned>(legacy[1]) > 63U)
                legacy[1] = std::byte{};
        }
    }
    put(common, 0x0bU, value.level);
    put(common, 0x0eU, value.transpose);
    put(common, 0x10U, value.portamento.type);
    put(common, 0x11U, value.portamento.rate);
    put(common, 0x12U, value.portamento.time);
    put_channels(common, 0x02U, value.controller_reset.a);
    put_channels(common, 0x04U, value.note_toggle.a);
    put_bits(common, 0x00U, native ? 0x40U : 0xc0U, 6U, value.lfo.sync);
    put_bits(common, 0x01U, 0x07U, 0U, value.lfo.cycle);
    put_bits(common, 0x01U, 0x38U, 3U, value.lfo.wave);
    put_bits(common, 0x01U, 0xc0U, 6U, value.lfo.initial_phase);
    put(common, 0x0fU, value.lfo.reset_channel);
    put(common, 0x13U, value.lfo.sample_hold_speed);
    put(common, 0x14U, value.lfo.tempo);
    put(common, 0x15U, value.lfo.reset_note);
    if (blocks.extended) {
        const auto extended = *blocks.extended;
        std::array<std::byte, 4> original;
        std::ranges::copy(extended.subspan<5, 4>(), original.begin());
        write_ad_outputs(extended, 0x05U, value.ad.left);
        write_ad_outputs(extended, 0x0aU, value.ad.right);
        put(extended, 0x09U, value.ad.right.pan);
        if (!std::ranges::equal(extended.subspan<5, 4>(), original))
            project_left_ad_outputs(common, extended.subspan<5, 4>());
        put_bits(extended, 0x04U, 7U, 0U, value.effect_connections[1]);
        put_channels(extended, 0x00U, value.controller_reset.b);
        put_channels(extended, 0x02U, value.note_toggle.b);
        if (value.step_wave.step_count) {
            const auto found = std::ranges::find(step_counts, *value.step_wave.step_count);
            put_bits(extended, 0x1eU, 7U, 0U,
                     std::optional<std::uint8_t>{static_cast<std::uint8_t>(found - step_counts.begin())});
        }
        put_bits(extended, 0x1eU, 0x18U, 3U, value.step_wave.slope);
        for (std::size_t index = 0; index < value.step_wave.values.size(); ++index)
            put(extended, 0x0eU + index, value.step_wave.values[index]);
    }
    return {};
}

Result<void> apply_program_parameters(std::vector<std::byte> &payload, const ProgramParameters &value,
                                      ASeriesModel model, ProgramParameterWriteMode mode) {
    const auto decoded = decode_object(payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto *program = std::get_if<CurrentProg>(&decoded->payload);
    if (!program || !program->layout.parameter_tail_offset)
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Program parameter writes require the current Program layout")};
    const auto tail = *program->layout.parameter_tail_offset;
    auto bytes = payload;
    const auto span = std::span{bytes};
    const MutableProgramParameterBlocks blocks{span.subspan<0x80U, 0x16U>(), span.subspan(tail + 0x78U).first<0x10U>(),
                                               span.subspan<0x110U, 0x10U>(),
                                               span.subspan(tail + 0x88U).first<0x28U>()};
    if (auto fields = apply_program_parameter_blocks(blocks, value, model); !fields)
        return fields;
    if (auto effects = apply_program_effect_parameters(bytes, program->layout, value, model, mode); !effects)
        return effects;
    payload = std::move(bytes);
    return {};
}

} // namespace axk::detail
