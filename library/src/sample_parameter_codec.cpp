#include "axklib/writer_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "axklib/bytes.hpp"

namespace axk::detail {
namespace {

constexpr std::size_t sample_parameter_block_size = 0xe0U;

Error invalid(std::string message) {
    return make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest, std::move(message));
}

template <typename T> bool outside(const std::optional<T> &value, int minimum, int maximum) {
    return value && (static_cast<int>(*value) < minimum || static_cast<int>(*value) > maximum);
}

bool invalid_envelope(const SampleFilterEnvelopeParameters &value) {
    return outside(value.attack_rate, 0, 127) || outside(value.decay_rate, 0, 127) ||
           outside(value.release_rate, 0, 127) || outside(value.init_level, -127, 127) ||
           outside(value.attack_level, -127, 127) || outside(value.sustain_level, -127, 127) ||
           outside(value.release_level, -127, 127) || outside(value.rate_key_scaling, -7, 7) ||
           outside(value.rate_velocity_sensitivity, -63, 63) ||
           outside(value.attack_level_velocity_sensitivity, -63, 63) ||
           outside(value.level_velocity_sensitivity, -63, 63);
}

bool invalid_envelope(const SamplePitchEnvelopeParameters &value) {
    return outside(value.attack_rate, 0, 127) || outside(value.decay_rate, 0, 127) ||
           outside(value.release_rate, 0, 127) || outside(value.init_level, -127, 127) ||
           outside(value.attack_level, -127, 127) || outside(value.sustain_level, -127, 127) ||
           outside(value.release_level, -127, 127) || outside(value.rate_key_scaling, -7, 7) ||
           outside(value.rate_velocity_sensitivity, -63, 63) || outside(value.level_velocity_sensitivity, -63, 63) ||
           outside(value.range, -63, 63);
}

bool invalid_envelope(const SampleAmplitudeEnvelopeParameters &value) {
    return outside(value.attack_rate, 0, 127) || outside(value.decay_rate, 0, 127) ||
           outside(value.release_rate, 0, 127) || outside(value.sustain_level, 0, 127) ||
           outside(value.attack_mode, 0, 2) || outside(value.rate_key_scaling, -7, 7) ||
           outside(value.rate_velocity_sensitivity, -63, 63);
}

void put_u8(std::span<std::byte> block, std::size_t offset, const std::optional<std::uint8_t> &value) {
    if (value)
        block[offset] = static_cast<std::byte>(*value);
}

void put_s8(std::span<std::byte> block, std::size_t offset, const std::optional<std::int8_t> &value) {
    if (value)
        block[offset] = static_cast<std::byte>(static_cast<std::uint8_t>(*value));
}

void set_masked_bit(std::byte &destination, std::uint8_t mask, const std::optional<bool> &value) {
    if (!value)
        return;
    auto raw = std::to_integer<std::uint8_t>(destination);
    raw = *value ? static_cast<std::uint8_t>(raw | mask) : static_cast<std::uint8_t>(raw & ~mask);
    destination = static_cast<std::byte>(raw);
}

template <typename T> void merge_optional(std::optional<T> &destination, const std::optional<T> &source) {
    if (source)
        destination = source;
}

template <typename Envelope> void merge_envelope(Envelope &destination, const Envelope &source) {
    merge_optional(destination.attack_rate, source.attack_rate);
    merge_optional(destination.decay_rate, source.decay_rate);
    merge_optional(destination.release_rate, source.release_rate);
    if constexpr (requires { destination.init_level; }) {
        merge_optional(destination.init_level, source.init_level);
        merge_optional(destination.attack_level, source.attack_level);
    }
    merge_optional(destination.sustain_level, source.sustain_level);
    if constexpr (requires { destination.release_level; })
        merge_optional(destination.release_level, source.release_level);
    merge_optional(destination.rate_key_scaling, source.rate_key_scaling);
    merge_optional(destination.rate_velocity_sensitivity, source.rate_velocity_sensitivity);
    if constexpr (requires { destination.attack_level_velocity_sensitivity; })
        merge_optional(destination.attack_level_velocity_sensitivity, source.attack_level_velocity_sensitivity);
    if constexpr (requires { destination.level_velocity_sensitivity; })
        merge_optional(destination.level_velocity_sensitivity, source.level_velocity_sensitivity);
    if constexpr (requires { destination.range; })
        merge_optional(destination.range, source.range);
    if constexpr (requires { destination.attack_mode; })
        merge_optional(destination.attack_mode, source.attack_mode);
}

template <typename T> bool set(const std::optional<T> &value) { return value.has_value(); }

template <typename Envelope> bool envelope_set(const Envelope &value) {
    auto result = set(value.attack_rate) || set(value.decay_rate) || set(value.release_rate) ||
                  set(value.sustain_level) || set(value.rate_key_scaling) || set(value.rate_velocity_sensitivity);
    if constexpr (requires { value.init_level; })
        result = result || set(value.init_level) || set(value.attack_level);
    if constexpr (requires { value.release_level; })
        result = result || set(value.release_level);
    if constexpr (requires { value.attack_level_velocity_sensitivity; })
        result = result || set(value.attack_level_velocity_sensitivity);
    if constexpr (requires { value.level_velocity_sensitivity; })
        result = result || set(value.level_velocity_sensitivity);
    if constexpr (requires { value.range; })
        result = result || set(value.range);
    if constexpr (requires { value.attack_mode; })
        result = result || set(value.attack_mode);
    return result;
}

} // namespace

bool has_sample_parameter_values(const SampleParameters &value) {
    const auto lfo_set = set(value.lfo.wave) || set(value.lfo.speed) || set(value.lfo.delay_time) ||
                         set(value.lfo.key_on_sync) || set(value.lfo.cutoff_mod_phase_invert) ||
                         set(value.lfo.pitch_mod_phase_invert) || set(value.lfo.cutoff_mod_depth) ||
                         set(value.lfo.pitch_mod_depth) || set(value.lfo.amp_mod_depth);
    const auto controls_set = std::ranges::any_of(value.controls, [](const auto &control) {
        return set(control.device) || set(control.function) || set(control.type) || set(control.range);
    });
#define AXK_SET(member) set(value.member)
    return AXK_SET(fixed_pitch) || AXK_SET(key_crossfade) || AXK_SET(mono_mode) || AXK_SET(sample_eq_type) ||
           AXK_SET(midi_receive_channel) || AXK_SET(pitch_bend_type) || AXK_SET(pitch_bend_range) ||
           AXK_SET(coarse_tune) || AXK_SET(root_key) || AXK_SET(fine_tune_cents) || AXK_SET(key_low) ||
           AXK_SET(key_high) || AXK_SET(loop_mode) || AXK_SET(loop_tempo_hundredths) || AXK_SET(loop_start_frame) ||
           AXK_SET(loop_length_frames) || AXK_SET(wave_start_velocity_sensitivity) || AXK_SET(filter_type) ||
           AXK_SET(filter_cutoff) || AXK_SET(filter_q_width) || AXK_SET(filter_scaling_break1) ||
           AXK_SET(filter_scaling_break2) || AXK_SET(filter_scaling_cutoff1) || AXK_SET(filter_scaling_cutoff2) ||
           AXK_SET(filter_velocity_to_cutoff) || AXK_SET(filter_velocity_to_q_width) || AXK_SET(expand_detune) ||
           AXK_SET(expand_dephase) || AXK_SET(expand_width) || AXK_SET(random_pitch) || AXK_SET(level) ||
           AXK_SET(pan) || AXK_SET(velocity_low_limit) || AXK_SET(velocity_offset) || AXK_SET(velocity_high) ||
           AXK_SET(velocity_low) || AXK_SET(level_scaling_break1) || AXK_SET(level_scaling_break2) ||
           AXK_SET(level_scaling_level1) || AXK_SET(level_scaling_level2) || AXK_SET(velocity_sensitivity) ||
           AXK_SET(alternate_group) || AXK_SET(sample_eq_frequency) || AXK_SET(sample_eq_gain_db) ||
           AXK_SET(sample_eq_width_tenths) || AXK_SET(filter_cutoff_distance) || envelope_set(value.feg) ||
           envelope_set(value.peg) || envelope_set(value.aeg) || lfo_set || AXK_SET(filter_gain) || controls_set ||
           AXK_SET(velocity_xfade_high) || AXK_SET(velocity_xfade_low) || AXK_SET(output1_destination) ||
           AXK_SET(output1_level) || AXK_SET(output2_destination) || AXK_SET(output2_level) ||
           AXK_SET(portamento_type) || AXK_SET(portamento_rate) || AXK_SET(portamento_time);
#undef AXK_SET
}

Result<void> validate_sample_parameters(const SampleParameters &value) {
    const auto root_key = value.root_key.value_or(60U);
    const auto key_low = value.key_low.value_or(0U);
    const auto key_high = value.key_high.value_or(127U);
    const auto velocity_low = value.velocity_low.value_or(0U);
    const auto velocity_high = value.velocity_high.value_or(127U);
    const auto effective_low = key_low == sampler_original_key_low_limit ? root_key : key_low;
    const auto effective_high = key_high == sampler_original_key_high_limit ? root_key : key_high;
    if (outside(value.sample_eq_type, 0, 2) || outside(value.midi_receive_channel, 0, 16) ||
        outside(value.pitch_bend_type, 0, 12) || outside(value.pitch_bend_range, 0, 24) ||
        outside(value.coarse_tune, -64, 63) || outside(value.root_key, 0, 127) ||
        outside(value.fine_tune_cents, -63, 63) || (key_low > 127U && key_low != sampler_original_key_low_limit) ||
        key_high > sampler_original_key_high_limit || effective_high < effective_low ||
        (value.loop_mode && static_cast<std::uint8_t>(*value.loop_mode) >
                                static_cast<std::uint8_t>(AudioSamplerLoopMode::reverse_one_shot)) ||
        outside(value.loop_tempo_hundredths, 8000, 15999) || outside(value.wave_start_velocity_sensitivity, -63, 63) ||
        outside(value.filter_type, 0, 16) || outside(value.filter_cutoff, 0, 127) ||
        outside(value.filter_q_width, 0, 31) || outside(value.filter_scaling_break1, 0, 127) ||
        outside(value.filter_scaling_break2, 0, 127) ||
        value.filter_scaling_break1.value_or(0U) > value.filter_scaling_break2.value_or(127U) ||
        outside(value.filter_scaling_cutoff1, -127, 127) || outside(value.filter_scaling_cutoff2, -127, 127) ||
        outside(value.filter_velocity_to_cutoff, -63, 68) || outside(value.filter_velocity_to_q_width, -63, 68) ||
        outside(value.expand_detune, -7, 7) || outside(value.expand_dephase, -63, 63) ||
        outside(value.expand_width, -63, 63) || outside(value.random_pitch, 0, 63) || outside(value.level, 0, 127) ||
        outside(value.pan, -64, 63) || outside(value.velocity_low_limit, 0, 127) ||
        outside(value.velocity_offset, -127, 127) || velocity_high < velocity_low ||
        outside(value.level_scaling_break1, 0, 127) || outside(value.level_scaling_break2, 0, 127) ||
        value.level_scaling_break1.value_or(0U) > value.level_scaling_break2.value_or(127U) ||
        outside(value.level_scaling_level1, 0, 127) || outside(value.level_scaling_level2, 0, 127) ||
        outside(value.velocity_sensitivity, -127, 127) || outside(value.alternate_group, 0, 16) ||
        outside(value.sample_eq_frequency, 4, 58) || outside(value.sample_eq_gain_db, -12, 12) ||
        outside(value.sample_eq_width_tenths, 10, 120) || outside(value.filter_cutoff_distance, -63, 63) ||
        invalid_envelope(value.feg) || invalid_envelope(value.peg) || invalid_envelope(value.aeg) ||
        outside(value.lfo.wave, 0, 3) || outside(value.lfo.speed, 1, 128) || outside(value.lfo.delay_time, 0, 127) ||
        outside(value.lfo.cutoff_mod_depth, 0, 127) || outside(value.lfo.pitch_mod_depth, 0, 127) ||
        outside(value.lfo.amp_mod_depth, 0, 127) || outside(value.filter_gain, -31, 31) ||
        outside(value.velocity_xfade_high, 0, 127) || outside(value.velocity_xfade_low, 0, 127) ||
        outside(value.output1_destination, 0, 12) || outside(value.output1_level, 0, 127) ||
        outside(value.output2_destination, 0, 12) || outside(value.output2_level, 0, 127) ||
        outside(value.portamento_type, 0, 5) || outside(value.portamento_rate, 1, 127) ||
        outside(value.portamento_time, 1, 127)) {
        return std::unexpected{invalid("Sample parameters are outside their supported ranges")};
    }
    for (const auto &control : value.controls) {
        if (outside(control.device, 0, 126) || outside(control.function, 0, 36) || outside(control.type, 0, 3) ||
            outside(control.range, -63, 63)) {
            return std::unexpected{invalid("Sample controller parameters are outside their supported ranges")};
        }
    }
    return {};
}

Result<void> apply_sample_parameters_to_block(std::span<std::byte> block, const SampleParameters &value) {
    if (block.size() < sample_parameter_block_size)
        return std::unexpected{invalid("Sample parameter block is truncated")};
    if (auto valid = validate_sample_parameters(value); !valid)
        return valid;

    auto &mapout = block[0x29U];
    set_masked_bit(mapout, 0x10U, value.fixed_pitch);
    set_masked_bit(mapout, 0x04U, value.key_crossfade);
    set_masked_bit(mapout, 0x02U, value.mono_mode);
    if (value.sample_eq_type) {
        const auto preserved = static_cast<std::uint8_t>(std::to_integer<std::uint8_t>(mapout) & 0x3fU);
        mapout = static_cast<std::byte>(preserved | static_cast<std::uint8_t>(*value.sample_eq_type << 6U));
    }
    put_u8(block, 0x2aU, value.midi_receive_channel);
    put_u8(block, 0x2bU, value.pitch_bend_type);
    put_u8(block, 0x2cU, value.pitch_bend_range);
    put_s8(block, 0x2dU, value.coarse_tune);
    put_u8(block, 0x2eU, value.root_key);
    put_s8(block, 0x34U, value.fine_tune_cents);
    put_u8(block, 0x3aU, value.key_high);
    put_u8(block, 0x3bU, value.key_low);
    if (value.loop_mode)
        block[0x3dU] = static_cast<std::byte>(*value.loop_mode);
    ByteWriter writer{block};
    if (value.loop_tempo_hundredths) {
        if (auto written = writer.write_be16(0x3eU, *value.loop_tempo_hundredths); !written)
            return std::unexpected{written.error()};
    }
    if (value.loop_start_frame) {
        if (auto written = writer.write_be32(0x50U, *value.loop_start_frame); !written)
            return std::unexpected{written.error()};
    }
    if (value.loop_length_frames) {
        if (auto written = writer.write_be32(0x58U, *value.loop_length_frames); !written)
            return std::unexpected{written.error()};
    }

    put_s8(block, 0x60U, value.wave_start_velocity_sensitivity);
    put_u8(block, 0x61U, value.filter_type);
    put_u8(block, 0x62U, value.filter_cutoff);
    put_u8(block, 0x63U, value.filter_q_width);
    put_u8(block, 0x64U, value.filter_scaling_break1);
    put_u8(block, 0x65U, value.filter_scaling_break2);
    put_s8(block, 0x66U, value.filter_scaling_cutoff1);
    put_s8(block, 0x67U, value.filter_scaling_cutoff2);
    put_s8(block, 0x68U, value.filter_velocity_to_cutoff);
    put_s8(block, 0x69U, value.filter_velocity_to_q_width);
    put_s8(block, 0x6aU, value.expand_detune);
    put_s8(block, 0x6bU, value.expand_dephase);
    put_s8(block, 0x6cU, value.expand_width);
    put_u8(block, 0x6dU, value.random_pitch);
    put_u8(block, 0x6eU, value.level);
    put_s8(block, 0x6fU, value.pan);
    put_u8(block, 0x70U, value.velocity_low_limit);
    put_s8(block, 0x71U, value.velocity_offset);
    put_u8(block, 0x72U, value.velocity_high);
    put_u8(block, 0x73U, value.velocity_low);
    put_u8(block, 0x74U, value.level_scaling_break1);
    put_u8(block, 0x75U, value.level_scaling_break2);
    put_u8(block, 0x76U, value.level_scaling_level1);
    put_u8(block, 0x77U, value.level_scaling_level2);
    put_s8(block, 0x78U, value.velocity_sensitivity);
    put_u8(block, 0x79U, value.alternate_group);
    put_u8(block, 0x7aU, value.sample_eq_frequency);
    if (value.sample_eq_gain_db)
        block[0x7bU] = static_cast<std::byte>(static_cast<std::uint8_t>(*value.sample_eq_gain_db + 64));
    put_u8(block, 0x7cU, value.sample_eq_width_tenths);
    put_s8(block, 0x7dU, value.filter_cutoff_distance);

    put_u8(block, 0x7eU, value.feg.attack_rate);
    put_u8(block, 0x7fU, value.feg.decay_rate);
    put_u8(block, 0x80U, value.feg.release_rate);
    put_s8(block, 0x81U, value.feg.init_level);
    put_s8(block, 0x82U, value.feg.attack_level);
    put_s8(block, 0x83U, value.feg.sustain_level);
    put_s8(block, 0x84U, value.feg.release_level);
    put_s8(block, 0x85U, value.feg.rate_key_scaling);
    put_s8(block, 0x86U, value.feg.rate_velocity_sensitivity);
    put_s8(block, 0x87U, value.feg.attack_level_velocity_sensitivity);
    put_s8(block, 0x88U, value.feg.level_velocity_sensitivity);
    put_u8(block, 0x89U, value.peg.attack_rate);
    put_u8(block, 0x8aU, value.peg.decay_rate);
    put_u8(block, 0x8bU, value.peg.release_rate);
    put_s8(block, 0x8cU, value.peg.init_level);
    put_s8(block, 0x8dU, value.peg.attack_level);
    put_s8(block, 0x8eU, value.peg.sustain_level);
    put_s8(block, 0x8fU, value.peg.release_level);
    put_s8(block, 0x90U, value.peg.rate_key_scaling);
    put_s8(block, 0x91U, value.peg.rate_velocity_sensitivity);
    put_s8(block, 0x92U, value.peg.level_velocity_sensitivity);
    put_s8(block, 0x93U, value.peg.range);
    put_u8(block, 0x94U, value.aeg.attack_rate);
    put_u8(block, 0x95U, value.aeg.decay_rate);
    put_u8(block, 0x96U, value.aeg.release_rate);
    put_u8(block, 0x99U, value.aeg.sustain_level);
    put_u8(block, 0x9bU, value.aeg.attack_mode);
    put_s8(block, 0x9cU, value.aeg.rate_key_scaling);
    put_s8(block, 0x9dU, value.aeg.rate_velocity_sensitivity);
    put_u8(block, 0x9eU, value.lfo.wave);
    if (value.lfo.speed)
        block[0x9fU] = static_cast<std::byte>(*value.lfo.speed - 1U);
    put_u8(block, 0xa0U, value.lfo.delay_time);
    auto &lfo_flags = block[0xa1U];
    set_masked_bit(lfo_flags, 0x01U, value.lfo.key_on_sync);
    set_masked_bit(lfo_flags, 0x02U, value.lfo.cutoff_mod_phase_invert);
    set_masked_bit(lfo_flags, 0x04U, value.lfo.pitch_mod_phase_invert);
    put_u8(block, 0xa2U, value.lfo.cutoff_mod_depth);
    put_u8(block, 0xa3U, value.lfo.pitch_mod_depth);
    put_u8(block, 0xa4U, value.lfo.amp_mod_depth);
    put_s8(block, 0xa9U, value.filter_gain);

    for (std::size_t index = 0; index < value.controls.size(); ++index) {
        const auto put_control = [&](std::size_t offset) {
            put_u8(block, offset, value.controls[index].device);
            put_u8(block, offset + 1U, value.controls[index].function);
            put_u8(block, offset + 2U, value.controls[index].type);
            put_s8(block, offset + 3U, value.controls[index].range);
        };
        put_control(index * 4U);
        put_control(0xbcU + index * 4U);
    }
    put_u8(block, 0xd4U, value.velocity_xfade_high);
    put_u8(block, 0xd5U, value.velocity_xfade_low);
    put_u8(block, 0xd6U, value.output1_destination);
    put_u8(block, 0xd7U, value.output1_level);
    put_u8(block, 0xd8U, value.output2_destination);
    put_u8(block, 0xd9U, value.output2_level);
    put_u8(block, 0xdaU, value.portamento_type);
    put_u8(block, 0xdbU, value.portamento_rate);
    put_u8(block, 0xdcU, value.portamento_time);
    if (value.portamento_type) {
        const auto raw = std::to_integer<std::uint8_t>(mapout);
        mapout =
            *value.portamento_type == 1U ? static_cast<std::byte>(raw | 0x01U) : static_cast<std::byte>(raw & 0xfeU);
    }
    return {};
}

void merge_sample_parameters(SampleParameters &destination, const SampleParameters &source) {
#define AXK_MERGE(member) merge_optional(destination.member, source.member)
    AXK_MERGE(fixed_pitch);
    AXK_MERGE(key_crossfade);
    AXK_MERGE(mono_mode);
    AXK_MERGE(sample_eq_type);
    AXK_MERGE(midi_receive_channel);
    AXK_MERGE(pitch_bend_type);
    AXK_MERGE(pitch_bend_range);
    AXK_MERGE(coarse_tune);
    AXK_MERGE(root_key);
    AXK_MERGE(fine_tune_cents);
    AXK_MERGE(key_low);
    AXK_MERGE(key_high);
    AXK_MERGE(loop_mode);
    AXK_MERGE(loop_tempo_hundredths);
    AXK_MERGE(loop_start_frame);
    AXK_MERGE(loop_length_frames);
    AXK_MERGE(wave_start_velocity_sensitivity);
    AXK_MERGE(filter_type);
    AXK_MERGE(filter_cutoff);
    AXK_MERGE(filter_q_width);
    AXK_MERGE(filter_scaling_break1);
    AXK_MERGE(filter_scaling_break2);
    AXK_MERGE(filter_scaling_cutoff1);
    AXK_MERGE(filter_scaling_cutoff2);
    AXK_MERGE(filter_velocity_to_cutoff);
    AXK_MERGE(filter_velocity_to_q_width);
    AXK_MERGE(expand_detune);
    AXK_MERGE(expand_dephase);
    AXK_MERGE(expand_width);
    AXK_MERGE(random_pitch);
    AXK_MERGE(level);
    AXK_MERGE(pan);
    AXK_MERGE(velocity_low_limit);
    AXK_MERGE(velocity_offset);
    AXK_MERGE(velocity_high);
    AXK_MERGE(velocity_low);
    AXK_MERGE(level_scaling_break1);
    AXK_MERGE(level_scaling_break2);
    AXK_MERGE(level_scaling_level1);
    AXK_MERGE(level_scaling_level2);
    AXK_MERGE(velocity_sensitivity);
    AXK_MERGE(alternate_group);
    AXK_MERGE(sample_eq_frequency);
    AXK_MERGE(sample_eq_gain_db);
    AXK_MERGE(sample_eq_width_tenths);
    AXK_MERGE(filter_cutoff_distance);
    merge_envelope(destination.feg, source.feg);
    merge_envelope(destination.peg, source.peg);
    merge_envelope(destination.aeg, source.aeg);
    AXK_MERGE(lfo.wave);
    AXK_MERGE(lfo.speed);
    AXK_MERGE(lfo.delay_time);
    AXK_MERGE(lfo.key_on_sync);
    AXK_MERGE(lfo.cutoff_mod_phase_invert);
    AXK_MERGE(lfo.pitch_mod_phase_invert);
    AXK_MERGE(lfo.cutoff_mod_depth);
    AXK_MERGE(lfo.pitch_mod_depth);
    AXK_MERGE(lfo.amp_mod_depth);
    AXK_MERGE(filter_gain);
    for (std::size_t index = 0; index < destination.controls.size(); ++index) {
        merge_optional(destination.controls[index].device, source.controls[index].device);
        merge_optional(destination.controls[index].function, source.controls[index].function);
        merge_optional(destination.controls[index].type, source.controls[index].type);
        merge_optional(destination.controls[index].range, source.controls[index].range);
    }
    AXK_MERGE(velocity_xfade_high);
    AXK_MERGE(velocity_xfade_low);
    AXK_MERGE(output1_destination);
    AXK_MERGE(output1_level);
    AXK_MERGE(output2_destination);
    AXK_MERGE(output2_level);
    AXK_MERGE(portamento_type);
    AXK_MERGE(portamento_rate);
    AXK_MERGE(portamento_time);
#undef AXK_MERGE
}

} // namespace axk::detail
