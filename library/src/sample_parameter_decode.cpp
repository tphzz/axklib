#include "axklib/sample_parameters.hpp"

#include <algorithm>
#include <bit>

#include "axklib/bytes.hpp"

namespace axk {
namespace {

template <typename T> std::optional<T> known(T value, int minimum, int maximum) {
    const auto number = static_cast<int>(value);
    return number >= minimum && number <= maximum ? std::optional<T>{value} : std::nullopt;
}

void read_envelopes(const ByteReader &r, SampleParameters &p, bool native) {
    p.feg.attack_rate = known(*r.u8(0x7e), 0, 127);
    p.feg.decay_rate = known(*r.u8(0x7f), 0, 127);
    p.feg.release_rate = known(*r.u8(0x80), 0, 127);
    p.feg.init_level = known(*r.s8(0x81), -127, 127);
    p.feg.attack_level = known(*r.s8(0x82), -127, 127);
    p.feg.sustain_level = known(*r.s8(0x83), -127, 127);
    p.feg.release_level = known(*r.s8(0x84), -127, 127);
    p.feg.rate_key_scaling = known(*r.s8(0x85), -7, 7);
    p.feg.rate_velocity_sensitivity = known(*r.s8(0x86), -63, 63);
    p.feg.attack_level_velocity_sensitivity = known(*r.s8(0x87), -63, 63);
    p.feg.level_velocity_sensitivity = known(*r.s8(0x88), -63, 63);
    p.peg.attack_rate = known(*r.u8(0x89), 0, 127);
    p.peg.decay_rate = known(*r.u8(0x8a), 0, 127);
    p.peg.release_rate = known(*r.u8(0x8b), 0, 127);
    p.peg.init_level = known(*r.s8(0x8c), -127, 127);
    p.peg.attack_level = known(*r.s8(0x8d), -127, 127);
    p.peg.sustain_level = known(*r.s8(0x8e), -127, 127);
    p.peg.release_level = known(*r.s8(0x8f), -127, 127);
    p.peg.rate_key_scaling = known(*r.s8(0x90), -7, 7);
    p.peg.rate_velocity_sensitivity = known(*r.s8(0x91), -63, 63);
    p.peg.level_velocity_sensitivity = known(*r.s8(0x92), -63, 63);
    p.peg.range = known(*r.s8(0x93), -63, 63);
    p.aeg.attack_rate = known(*r.u8(0x94), 0, 127);
    p.aeg.decay_rate = known(*r.u8(0x95), 0, 127);
    p.aeg.release_rate = known(*r.u8(0x96), 0, 127);
    p.aeg.sustain_level = known(*r.u8(0x99), 0, 127);
    p.aeg.attack_mode = known(*r.u8(0x9b), 0, native ? 1 : 2);
    p.aeg.rate_key_scaling = known(*r.s8(0x9c), -7, 7);
    p.aeg.rate_velocity_sensitivity = known(*r.s8(0x9d), -63, 63);
    p.lfo.wave = known(*r.u8(0x9e), 0, 3);
    if (const auto speed = known(*r.u8(0x9f), 0, 127))
        p.lfo.speed = static_cast<std::uint8_t>(*speed + 1U);
    p.lfo.delay_time = known(*r.u8(0xa0), 0, 127);
    const auto flags = *r.u8(0xa1);
    p.lfo.key_on_sync = (flags & 1U) != 0U;
    p.lfo.cutoff_mod_phase_invert = (flags & 2U) != 0U;
    p.lfo.pitch_mod_phase_invert = (flags & 4U) != 0U;
    p.lfo.cutoff_mod_depth = known(*r.u8(0xa2), 0, 127);
    p.lfo.pitch_mod_depth = known(*r.u8(0xa3), 0, 127);
    p.lfo.amp_mod_depth = known(*r.u8(0xa4), 0, 127);
}

void read_scalars(const ByteReader &r, SampleParameters &p, bool native) {
    const auto mapout = *r.u8(0x29);
    p.fixed_pitch = (mapout & 0x10U) != 0U;
    p.key_crossfade = (mapout & 4U) != 0U;
    p.mono_mode = (mapout & 2U) != 0U;
    if (!native)
        p.sample_eq_type = known(static_cast<std::uint8_t>(mapout >> 6U), 0, 2);
    p.midi_receive_channel = known(*r.u8(0x2a), 0, 16);
    p.pitch_bend_type = known(*r.u8(0x2b), 0, native ? 13 : 12);
    p.pitch_bend_range = known(*r.u8(0x2c), 0, 24);
    p.coarse_tune = known(*r.s8(0x2d), native ? -127 : -64, native ? 127 : 63);
    p.key_high = known(*r.u8(0x3a), 0, 128);
    const auto low = *r.u8(0x3b);
    p.key_low = low == 255U ? std::optional{low} : known(low, 0, 127);
    if (const auto mode = known(*r.u8(0x3d), 0, 5))
        p.loop_mode = static_cast<AudioSamplerLoopMode>(*mode);
    p.loop_tempo_hundredths = known(*r.be16(0x3e), 8000, 15999);
    p.wave_start_velocity_sensitivity = known(*r.s8(0x60), -63, 63);
    p.filter_type = known(*r.u8(0x61), 0, 16);
    p.filter_cutoff = known(*r.u8(0x62), 0, 127);
    p.filter_q_width = known(*r.u8(0x63), 0, 31);
    p.filter_scaling_break1 = known(*r.u8(0x64), 0, 127);
    p.filter_scaling_break2 = known(*r.u8(0x65), 0, 127);
    p.filter_scaling_cutoff1 = known(*r.s8(0x66), -127, 127);
    p.filter_scaling_cutoff2 = known(*r.s8(0x67), -127, 127);
    p.filter_velocity_to_cutoff = known(*r.s8(0x68), -63, 68);
    p.filter_velocity_to_q_width = known(*r.s8(0x69), -63, 68);
    p.expand_detune = known(*r.s8(0x6a), -7, 7);
    p.expand_dephase = known(*r.s8(0x6b), -63, 63);
    p.expand_width = known(*r.s8(0x6c), -63, 63);
    p.random_pitch = known(*r.u8(0x6d), 0, 63);
    p.level = known(*r.u8(0x6e), 0, 127);
    p.pan = known(*r.s8(0x6f), -64, 63);
    p.velocity_low_limit = known(*r.u8(0x70), 0, 127);
    p.velocity_offset = known(*r.s8(0x71), -127, 127);
    p.velocity_high = known(*r.u8(0x72), 0, 127);
    p.velocity_low = known(*r.u8(0x73), 0, 127);
    p.level_scaling_break1 = known(*r.u8(0x74), 0, 127);
    p.level_scaling_break2 = known(*r.u8(0x75), 0, 127);
    p.level_scaling_level1 = known(*r.u8(0x76), 0, 127);
    p.level_scaling_level2 = known(*r.u8(0x77), 0, 127);
    p.velocity_sensitivity = known(*r.s8(0x78), -127, 127);
    p.alternate_group = known(*r.u8(0x79), 0, 16);
    p.sample_eq_frequency = known(*r.u8(0x7a), 4, 58);
    if (const auto gain = known(*r.u8(0x7b), 52, 76))
        p.sample_eq_gain_db = static_cast<std::int8_t>(static_cast<int>(*gain) - 64);
    p.sample_eq_width_tenths = known(*r.u8(0x7c), 10, 120);
    p.filter_cutoff_distance = known(*r.s8(0x7d), -63, 63);
    p.filter_gain = known(*r.s8(0xa9), -31, 31);
}

} // namespace

Result<DecodedSampleParameters> decode_sample_parameter_block(std::span<const std::byte> bytes,
                                                              SampleParameterGeneration generation) {
    if (generation != SampleParameterGeneration::a3000 && generation != SampleParameterGeneration::current)
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "Sample parameter generation is unsupported")};
    const auto native = generation == SampleParameterGeneration::a3000;
    if (bytes.size() != (native ? 0xbcU : 0xe0U))
        return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                          "Sample parameter block has the wrong size for its generation")};
    const ByteReader r{bytes};
    DecodedSampleParameters result;
    result.generation = generation;
    result.raw_bytes.assign(bytes.begin(), bytes.end());
    result.sample_flags = *r.u8(0x28);
    result.mapout_flags = *r.u8(0x29);
    for (std::size_t index = 0; index < 4U; ++index)
        result.linked_program_bitmap_words[index] = *r.be32(0x18U + index * 4U);
    for (std::size_t lane = 0; lane < result.members.size(); ++lane) {
        result.members[lane] = {*r.u8(0x2eU + lane),        *r.be16(0x30U + 2U * lane), *r.s8(0x34U + lane),
                                *r.be16(0x36U + 2U * lane), *r.be32(0x40U + 4U * lane), *r.be32(0x48U + 4U * lane),
                                *r.be32(0x50U + 4U * lane), *r.be32(0x58U + 4U * lane)};
    }
    for (std::size_t index = 0; index < result.eq_coefficients.size(); ++index)
        result.eq_coefficients[index] = std::bit_cast<std::int16_t>(*r.be16(0xaaU + index * 2U));
    result.cached_wave_end = *r.be32(0xb4);
    result.cached_loop_end = *r.be32(0xb8);
    auto &p = result.parameters;
    p.root_key = known(result.members[0].root_key, 0, 127);
    p.fine_tune_cents = known(result.members[0].fine_tune_cents, -63, 63);
    p.loop_start_frame = result.members[0].loop_start_frame;
    p.loop_length_frames = result.members[0].loop_length_frames;
    read_scalars(r, p, native);
    read_envelopes(r, p, native);
    for (std::size_t index = 0; index < p.controls.size(); ++index) {
        const auto offset = (native ? 0U : 0xbcU) + index * 4U;
        p.controls[index] = {known(*r.u8(offset), 0, native ? 125 : 126),
                             known(*r.u8(offset + 1U), 0, native ? 21 : 36), known(*r.u8(offset + 2U), 0, 3),
                             known(*r.s8(offset + 3U), -63, 63)};
    }
    const auto outputs = native ? 0xa5U : 0xd6U;
    p.output1_destination = known(*r.u8(outputs), 0, native ? 4 : 12);
    p.output1_level = known(*r.u8(outputs + 1U), 0, 127);
    p.output2_destination = known(*r.u8(outputs + 2U), 0, native ? 5 : 12);
    p.output2_level = known(*r.u8(outputs + 3U), 0, 127);
    if (native) {
        p.portamento_type = static_cast<std::uint8_t>(result.mapout_flags & 1U);
        p.velocity_crossfade = (result.mapout_flags & 8U) != 0U;
    } else {
        result.controller_copies_match = std::ranges::equal(bytes.first(0x18), bytes.subspan(0xbc, 0x18));
        p.velocity_xfade_high = known(*r.u8(0xd4), 0, 127);
        p.velocity_xfade_low = known(*r.u8(0xd5), 0, 127);
        p.portamento_type = known(*r.u8(0xda), 0, 5);
        p.portamento_rate = known(*r.u8(0xdb), 1, 127);
        p.portamento_time = known(*r.u8(0xdc), 1, 127);
    }
    return result;
}

} // namespace axk
