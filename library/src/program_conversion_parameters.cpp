#include "axklib/program_format_conversion_internal.hpp"

#include <array>
#include <format>
#include <string>
#include <utility>

#include "axklib/bytes.hpp"
#include "axklib/effects.hpp"

namespace axk::detail {
namespace {

void range(ProgramFormatConversionPlan &plan, std::string key, std::string label, int value, int low, int high) {
    if (value < low || value > high)
        plan.blockers.push_back(
            {std::move(key), std::move(label) + " is outside the target format's supported range.", value});
}

void assess_effect(ProgramFormatConversionPlan &plan, std::span<const std::byte> block, std::size_t slot, bool native,
                   bool version_one) {
    const ByteReader reader{block};
    const auto key = std::format("effects.{}", slot + 1U);
    const auto label = std::format("Effect {}", slot + 1U);
    auto type = static_cast<unsigned>(*reader.u8(native ? 7U : 6U));
    if (version_one) {
        if (type >= 47U && type <= 51U) {
            plan.blockers.push_back(
                {key + ".type", label + " uses an A3000 V1 type that would be replaced by Through.", type});
            return;
        }
        if (type >= 52U)
            type -= 5U;
    }
    range(plan, key + ".enabled", label + " bypass", *reader.u8(0), 0, 1);
    range(plan, key + ".input", label + " input level", *reader.u8(1), 0, 127);
    range(plan, key + ".output", label + " output level", *reader.u8(2), 0, 127);
    range(plan, key + ".pan", label + " pan", *reader.s8(3), -63, 63);
    range(plan, key + ".destination", label + " output destination", *reader.u8(4), 0, !native && slot < 3U ? 8 : 5);
    range(plan, key + ".width", label + " width", *reader.s8(5), -126, 0);
    const auto info =
        effect_write_info(static_cast<std::uint16_t>(type), native ? EffectProfile::a3000 : EffectProfile::a5000);
    if (!info) {
        plan.blockers.push_back({key + ".type", label + " type cannot be represented by this format.", type});
        return;
    }
    for (std::size_t word = 0; word < info->parameters.size(); ++word) {
        const auto &domain = info->parameters[word];
        if (domain.kind == EffectParameterKind::stored_value)
            range(plan, key + std::format(".parameters.{}", word + 1U), label + std::format(" parameter {}", word + 1U),
                  *reader.be16(8U + word * 2U), domain.minimum, domain.maximum);
    }
}

void assess_row(ProgramFormatConversionPlan &plan, std::span<const std::byte> row, std::size_t index, bool native) {
    const ByteReader reader{row};
    const auto key = std::format("assignments.{}", index + 1U);
    const auto label = std::format("Assignment {}", index + 1U);
    const auto receive = *reader.u8(0x15U);
    if (receive != 0xffU)
        range(plan, key + ".receive", label + " receive channel", receive, 0, native ? 16 : 32);
    for (const auto offset : {0x16U, 0x18U, 0x1aU, 0x1cU, 0x20U, 0x25U, 0x26U, 0x27U, 0x29U, 0x2cU})
        range(plan, key + std::format(".offset.{:02x}", offset), label + " parameter offset", *reader.s8(offset), -127,
              127);
    range(plan, key + ".alternate_group", label + " alternate group", *reader.s8(0x24U), -1, 16);
    range(plan, key + ".filter_gain", label + " filter gain", *reader.s8(0x2aU), -63, 63);
    range(plan, key + ".filter_q", label + " filter resonance", *reader.s8(0x2bU), -31, 31);
    for (const auto offset : {0x1eU, 0x1fU, 0x21U, 0x22U})
        range(plan, key + ".limits", label + " key/velocity limit", *reader.u8(offset), 0, 127);
    if (*reader.u8(0x1fU) > *reader.u8(0x1eU) || *reader.u8(0x22U) > *reader.u8(0x21U))
        plan.blockers.push_back({key + ".limits", label + " has reversed key or velocity limits.", std::nullopt});
    for (unsigned shift = 0; shift < (native ? 8U : 6U); shift += 2U)
        if (((*reader.u8(0x23U) >> shift) & 3U) == 2U)
            plan.blockers.push_back({key + ".switch", label + " contains an invalid inheritance switch.", 2});
    range(plan, key + ".midi_control", label + " MIDI control", *reader.u8(0x33U), 0, 1);
    range(plan, key + ".output1", label + " output 1", *reader.s8(native ? 0x2dU : 0x1dU), -1, native ? 4 : 12);
    range(plan, key + ".output2", label + " output 2", *reader.s8(native ? 0x30U : 0x28U), -1, native ? 5 : 12);
    for (const auto offset : {native ? 0x2eU : 0x2fU, native ? 0x31U : 0x32U})
        range(plan, key + ".output_level", label + " output level offset", *reader.s8(offset), -127, 127);
    if (!native)
        for (const auto offset : {0x17U, 0x19U, 0x1bU})
            range(plan, key + ".velocity", label + " velocity offset", *reader.s8(offset), -127, 127);
}
} // namespace

void assess_program_conversion_parameters(ProgramFormatConversionPlan &plan, std::span<const std::byte> payload,
                                          const ProgramStorageInfo &storage) {
    const bool native = storage.format == ProgramStorageFormat::a3000;
    const ByteReader reader{payload};
    const auto flags = *reader.u8(0x80U);
    const auto lfo = *reader.u8(0x81U);
    range(plan, "lfo.sync", "Program LFO sync", flags >> 6U, 0, native ? 1 : 2);
    range(plan, "lfo.cycle", "Program LFO cycle", lfo & 7U, 0, 6);
    range(plan, "lfo.wave", "Program LFO waveform", (lfo >> 3U) & 7U, 0, native ? 5 : 6);
    range(plan, "lfo.reset_channel", "Program LFO reset channel", *reader.s8(0x8fU), -2, native ? 16 : 32);
    range(plan, "lfo.reset_note", "Program LFO reset note", *reader.s8(0x95U), -1, 127);
    range(plan, "lfo.tempo", "Program LFO tempo", *reader.u8(0x94U), 25, 250);
    range(plan, "lfo.sample_hold", "Program LFO sample-and-hold speed", *reader.u8(0x93U), 0, 127);
    range(plan, "level", "Program level", *reader.u8(0x8bU), 0, 127);
    range(plan, "transpose", "Program transpose", *reader.s8(0x8eU), -127, 127);
    range(plan, "portamento.type", "Portamento type", *reader.u8(0x90U), 0, 3);
    range(plan, "portamento.rate", "Portamento rate", *reader.u8(0x91U), 1, 127);
    range(plan, "portamento.time", "Portamento time", *reader.u8(0x92U), 1, 127);
    range(plan, "ad.source", "A/D source", (flags >> 1U) & 3U, 0, 2);
    range(plan, "ad.pan", "A/D left pan", *reader.s8(0x86U), -63, 63);
    range(plan, "effects.connection", "Effect connection", (flags >> 3U) & 7U, 0, 4);
    if (native) {
        range(plan, "ad.output1", "A/D output 1", *reader.u8(0x87U), 0, 4);
        range(plan, "ad.output2", "A/D output 2", *reader.u8(0x89U), 0, 5);
        for (const auto offset : {0x88U, 0x8aU})
            range(plan, "ad.level", "A/D output level", *reader.u8(offset), 0, 127);
    }
    const auto tail = storage.logical_size - storage.parameter_tail_bytes;
    const auto controls = native ? 0x110U : tail + 0x78U;
    for (std::size_t controller = 0; controller < 4U; ++controller) {
        const auto offset = controls + controller * 4U;
        const auto key = std::format("controllers.{}", controller + 1U);
        const auto label = std::format("Controller {}", controller + 1U);
        range(plan, key + ".device", label + " device", *reader.u8(offset), 0, native ? 125 : 126);
        range(plan, key + ".function", label + " function", *reader.u8(offset + 1U), 0, native ? 63 : 128);
        range(plan, key + ".type", label + " type", *reader.u8(offset + 2U), 0, 3);
        range(plan, key + ".range", label + " range", *reader.s8(offset + 3U), -63, 63);
    }
    for (std::size_t slot = 0; slot < (native ? 3U : 6U); ++slot) {
        const auto offset = slot < 3U ? 0x98U + slot * 0x28U : tail + (slot - 3U) * 0x28U;
        assess_effect(plan, payload.subspan(offset, 0x28U), slot, native, storage.header_revision == 1U);
    }
    for (std::size_t row = 0; row < storage.stored_assignment_count; ++row)
        assess_row(plan, payload.subspan(0x120U + row * 0x38U, 0x38U), row, native);
}

} // namespace axk::detail
