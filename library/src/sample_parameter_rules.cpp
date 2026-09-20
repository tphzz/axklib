#include "axklib/sample_parameter_rules.hpp"

#include <array>
#include <type_traits>

#include "axklib/bytes.hpp"

namespace axk {
namespace {
using Encoding = SampleParameterEncoding;
using Location = SampleParameterLocation;

constexpr Location byte(std::size_t offset, int low, int high, int bias = 0) {
    return {offset,      low < 0 && bias == 0 ? Encoding::s8 : Encoding::u8, low, high, 255U, 0U, bias, std::nullopt,
            std::nullopt};
}
constexpr Location bits(std::size_t offset, std::uint8_t mask, std::uint8_t shift, int high = 1) {
    return {offset, Encoding::bits, 0, high, mask, shift, 0, std::nullopt, std::nullopt};
}
constexpr Location wide(std::size_t offset, Encoding encoding, std::int64_t low, std::int64_t high) {
    return {offset, encoding, low, high, 255U, 0U, 0, std::nullopt, std::nullopt};
}
constexpr Location output(std::size_t offset) {
    auto result = byte(offset, 0, 12);
    result.a5000_minimum = 10;
    return result;
}
constexpr Location key_low() {
    auto result = byte(0x3bU, 0, 127);
    result.extra_value = 255;
    return result;
}

#define FIELD_NAMED(key, member, older, later)                                                                         \
    SampleParameterRule {                                                                                              \
        key, older, later,                                                                                             \
            [](const SampleParameters &p) -> std::optional<std::int64_t> {                                             \
                return p.member ? std::optional<std::int64_t>{static_cast<std::int64_t>(*p.member)} : std::nullopt;    \
            },                                                                                                         \
            [](SampleParameters &p, std::int64_t v) {                                                                  \
                p.member = static_cast<typename std::remove_cvref_t<decltype(p.member)>::value_type>(v);               \
            },                                                                                                         \
            std::is_same_v<typename decltype(SampleParameters{}.member)::value_type, bool>                             \
    }
#define FIELD(member, older, later) FIELD_NAMED(#member, member, older, later)
#define COMMON(member, location) FIELD(member, location, location)
#define BYTE(member, offset, low, high) COMMON(member, byte(offset, low, high))
#define CONTROL(slot, number)                                                                                          \
    FIELD_NAMED("controls." #number ".device", controls[slot].device, byte(4U * slot, 0, 125),                         \
                byte(0xbcU + 4U * slot, 0, 126)),                                                                      \
        FIELD_NAMED("controls." #number ".function", controls[slot].function, byte(4U * slot + 1U, 0, 21),             \
                    byte(0xbdU + 4U * slot, 0, 36)),                                                                   \
        FIELD_NAMED("controls." #number ".type", controls[slot].type, byte(4U * slot + 2U, 0, 3),                      \
                    byte(0xbeU + 4U * slot, 0, 3)),                                                                    \
        FIELD_NAMED("controls." #number ".range", controls[slot].range, byte(4U * slot + 3U, -63, 63),                 \
                    byte(0xbfU + 4U * slot, -63, 63))

const std::array rules{
    CONTROL(0, 1),
    CONTROL(1, 2),
    CONTROL(2, 3),
    CONTROL(3, 4),
    CONTROL(4, 5),
    CONTROL(5, 6),
    COMMON(fixed_pitch, bits(0x29U, 16U, 4U)),
    COMMON(key_crossfade, bits(0x29U, 4U, 2U)),
    COMMON(mono_mode, bits(0x29U, 2U, 1U)),
    FIELD(velocity_crossfade, bits(0x29U, 8U, 3U), std::nullopt),
    FIELD(sample_eq_type, std::nullopt, bits(0x29U, 192U, 6U, 2)),
    BYTE(midi_receive_channel, 0x2aU, 0, 16),
    FIELD(pitch_bend_type, byte(0x2bU, 0, 13), byte(0x2bU, 0, 12)),
    BYTE(pitch_bend_range, 0x2cU, 0, 24),
    FIELD(coarse_tune, byte(0x2dU, -127, 127), byte(0x2dU, -64, 63)),
    BYTE(root_key, 0x2eU, 0, 127),
    BYTE(fine_tune_cents, 0x34U, -63, 63),
    BYTE(key_high, 0x3aU, 0, 128),
    COMMON(key_low, key_low()),
    BYTE(loop_mode, 0x3dU, 0, 5),
    COMMON(loop_tempo_hundredths, wide(0x3eU, Encoding::be16, 8000, 15999)),
    COMMON(loop_start_frame, wide(0x50U, Encoding::be32, 0, 0xffffffffLL)),
    COMMON(loop_length_frames, wide(0x58U, Encoding::be32, 0, 0xffffffffLL)),
    BYTE(wave_start_velocity_sensitivity, 0x60U, -63, 63),
    BYTE(filter_type, 0x61U, 0, 16),
    BYTE(filter_cutoff, 0x62U, 0, 127),
    BYTE(filter_q_width, 0x63U, 0, 31),
    BYTE(filter_scaling_break1, 0x64U, 0, 127),
    BYTE(filter_scaling_break2, 0x65U, 0, 127),
    BYTE(filter_scaling_cutoff1, 0x66U, -127, 127),
    BYTE(filter_scaling_cutoff2, 0x67U, -127, 127),
    BYTE(filter_velocity_to_cutoff, 0x68U, -63, 68),
    BYTE(filter_velocity_to_q_width, 0x69U, -63, 68),
    BYTE(expand_detune, 0x6aU, -7, 7),
    BYTE(expand_dephase, 0x6bU, -63, 63),
    BYTE(expand_width, 0x6cU, -63, 63),
    BYTE(random_pitch, 0x6dU, 0, 63),
    BYTE(level, 0x6eU, 0, 127),
    BYTE(pan, 0x6fU, -64, 63),
    BYTE(velocity_low_limit, 0x70U, 0, 127),
    BYTE(velocity_offset, 0x71U, -127, 127),
    BYTE(velocity_high, 0x72U, 0, 127),
    BYTE(velocity_low, 0x73U, 0, 127),
    BYTE(level_scaling_break1, 0x74U, 0, 127),
    BYTE(level_scaling_break2, 0x75U, 0, 127),
    BYTE(level_scaling_level1, 0x76U, 0, 127),
    BYTE(level_scaling_level2, 0x77U, 0, 127),
    BYTE(velocity_sensitivity, 0x78U, -127, 127),
    BYTE(alternate_group, 0x79U, 0, 16),
    BYTE(sample_eq_frequency, 0x7aU, 4, 58),
    COMMON(sample_eq_gain_db, byte(0x7bU, -12, 12, -64)),
    BYTE(sample_eq_width_tenths, 0x7cU, 10, 120),
    BYTE(filter_cutoff_distance, 0x7dU, -63, 63),
    BYTE(feg.attack_rate, 0x7eU, 0, 127),
    BYTE(feg.decay_rate, 0x7fU, 0, 127),
    BYTE(feg.release_rate, 0x80U, 0, 127),
    BYTE(feg.init_level, 0x81U, -127, 127),
    BYTE(feg.attack_level, 0x82U, -127, 127),
    BYTE(feg.sustain_level, 0x83U, -127, 127),
    BYTE(feg.release_level, 0x84U, -127, 127),
    BYTE(feg.rate_key_scaling, 0x85U, -7, 7),
    BYTE(feg.rate_velocity_sensitivity, 0x86U, -63, 63),
    BYTE(feg.attack_level_velocity_sensitivity, 0x87U, -63, 63),
    BYTE(feg.level_velocity_sensitivity, 0x88U, -63, 63),
    BYTE(peg.attack_rate, 0x89U, 0, 127),
    BYTE(peg.decay_rate, 0x8aU, 0, 127),
    BYTE(peg.release_rate, 0x8bU, 0, 127),
    BYTE(peg.init_level, 0x8cU, -127, 127),
    BYTE(peg.attack_level, 0x8dU, -127, 127),
    BYTE(peg.sustain_level, 0x8eU, -127, 127),
    BYTE(peg.release_level, 0x8fU, -127, 127),
    BYTE(peg.rate_key_scaling, 0x90U, -7, 7),
    BYTE(peg.rate_velocity_sensitivity, 0x91U, -63, 63),
    BYTE(peg.level_velocity_sensitivity, 0x92U, -63, 63),
    BYTE(peg.range, 0x93U, -63, 63),
    BYTE(aeg.attack_rate, 0x94U, 0, 127),
    BYTE(aeg.decay_rate, 0x95U, 0, 127),
    BYTE(aeg.release_rate, 0x96U, 0, 127),
    BYTE(aeg.sustain_level, 0x99U, 0, 127),
    FIELD(aeg.attack_mode, byte(0x9bU, 0, 1), byte(0x9bU, 0, 2)),
    BYTE(aeg.rate_key_scaling, 0x9cU, -7, 7),
    BYTE(aeg.rate_velocity_sensitivity, 0x9dU, -63, 63),
    BYTE(lfo.wave, 0x9eU, 0, 3),
    COMMON(lfo.speed, byte(0x9fU, 1, 128, 1)),
    BYTE(lfo.delay_time, 0xa0U, 0, 127),
    COMMON(lfo.key_on_sync, bits(0xa1U, 1U, 0U)),
    COMMON(lfo.cutoff_mod_phase_invert, bits(0xa1U, 2U, 1U)),
    COMMON(lfo.pitch_mod_phase_invert, bits(0xa1U, 4U, 2U)),
    BYTE(lfo.cutoff_mod_depth, 0xa2U, 0, 127),
    BYTE(lfo.pitch_mod_depth, 0xa3U, 0, 127),
    BYTE(lfo.amp_mod_depth, 0xa4U, 0, 127),
    BYTE(filter_gain, 0xa9U, -31, 31),
    FIELD(velocity_xfade_high, std::nullopt, byte(0xd4U, 0, 127)),
    FIELD(velocity_xfade_low, std::nullopt, byte(0xd5U, 0, 127)),
    FIELD(output1_destination, byte(0xa5U, 0, 4), output(0xd6U)),
    FIELD(output1_level, byte(0xa6U, 0, 127), byte(0xd7U, 0, 127)),
    FIELD(output2_destination, byte(0xa7U, 0, 5), output(0xd8U)),
    FIELD(output2_level, byte(0xa8U, 0, 127), byte(0xd9U, 0, 127)),
    FIELD(portamento_type, bits(0x29U, 1U, 0U), byte(0xdaU, 0, 5)),
    FIELD(portamento_rate, std::nullopt, byte(0xdbU, 1, 127)),
    FIELD(portamento_time, std::nullopt, byte(0xdcU, 1, 127)),
};
#undef CONTROL
#undef BYTE
#undef COMMON
#undef FIELD
#undef FIELD_NAMED
} // namespace

std::span<const SampleParameterRule> sample_parameter_rules() { return rules; }

std::optional<SampleParameterLocation> sample_parameter_location(const SampleParameterRule &rule,
                                                                 SampleParameterGeneration generation) {
    switch (generation) {
    case SampleParameterGeneration::a3000:
        return rule.a3000;
    case SampleParameterGeneration::a4000_a5000:
        return rule.a4000_a5000;
    }
    return std::nullopt;
}

bool sample_parameter_value_allowed(const SampleParameterLocation &location, std::int64_t value) {
    return (value >= location.minimum && value <= location.maximum) || location.extra_value == value;
}

std::optional<std::int64_t> read_sample_parameter_value(std::span<const std::byte> block,
                                                        const SampleParameterLocation &location) {
    const ByteReader reader{block};
    const auto offset = location.offset;
    switch (location.encoding) {
    case Encoding::u8:
    case Encoding::bits:
        if (const auto value = reader.u8(offset))
            return ((*value & location.mask) >> location.shift) + location.bias;
        break;
    case Encoding::s8:
        if (const auto value = reader.s8(offset))
            return *value + location.bias;
        break;
    case Encoding::be16:
        if (const auto value = reader.be16(offset))
            return *value;
        break;
    case Encoding::be32:
        if (const auto value = reader.be32(offset))
            return *value;
        break;
    }
    return std::nullopt;
}

Result<void> write_sample_parameter_value(std::span<std::byte> block, const SampleParameterLocation &location,
                                          std::int64_t value) {
    if (!sample_parameter_value_allowed(location, value) || !read_sample_parameter_value(block, location))
        return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                          "Sample parameter is outside its stored format's range")};
    ByteWriter writer{block};
    if (location.encoding == Encoding::be32)
        return writer.write_be32(location.offset, static_cast<std::uint32_t>(value));
    if (location.encoding == Encoding::be16)
        return writer.write_be16(location.offset, static_cast<std::uint16_t>(value));
    const auto original = std::to_integer<std::uint8_t>(block[location.offset]);
    const auto encoded = static_cast<std::uint8_t>(value - location.bias);
    block[location.offset] =
        static_cast<std::byte>((original & ~location.mask) | ((encoded << location.shift) & location.mask));
    return {};
}

std::vector<SampleParameterIssue> assess_sample_parameter_block(std::span<const std::byte> block,
                                                                SampleParameterGeneration generation) {
    std::vector<SampleParameterIssue> issues;
    for (const auto &rule : rules) {
        if (const auto location = sample_parameter_location(rule, generation)) {
            const auto value = read_sample_parameter_value(block, *location);
            if (!value || !sample_parameter_value_allowed(*location, *value))
                issues.push_back(
                    {std::string{rule.key}, "Stored value is outside this format's supported range.", value});
        }
    }
    return issues;
}

} // namespace axk
