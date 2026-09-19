#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/export.hpp"

namespace axk {

inline constexpr std::uint8_t sampler_original_key_high_limit = 0x80U;
inline constexpr std::uint8_t sampler_original_key_low_limit = 0xffU;

enum class AudioSamplerLoopMode : std::uint8_t {
    forward = 0,
    forward_loop = 1,
    forward_loop_release = 2,
    reverse = 3,
    forward_one_shot = 4,
    reverse_one_shot = 5,
};

struct SampleFilterEnvelopeParameters {
    std::optional<std::uint8_t> attack_rate;
    std::optional<std::uint8_t> decay_rate;
    std::optional<std::uint8_t> release_rate;
    std::optional<std::int8_t> init_level;
    std::optional<std::int8_t> attack_level;
    std::optional<std::int8_t> sustain_level;
    std::optional<std::int8_t> release_level;
    std::optional<std::int8_t> rate_key_scaling;
    std::optional<std::int8_t> rate_velocity_sensitivity;
    std::optional<std::int8_t> attack_level_velocity_sensitivity;
    std::optional<std::int8_t> level_velocity_sensitivity;
};

struct SamplePitchEnvelopeParameters {
    std::optional<std::uint8_t> attack_rate;
    std::optional<std::uint8_t> decay_rate;
    std::optional<std::uint8_t> release_rate;
    std::optional<std::int8_t> init_level;
    std::optional<std::int8_t> attack_level;
    std::optional<std::int8_t> sustain_level;
    std::optional<std::int8_t> release_level;
    std::optional<std::int8_t> rate_key_scaling;
    std::optional<std::int8_t> rate_velocity_sensitivity;
    std::optional<std::int8_t> level_velocity_sensitivity;
    std::optional<std::int8_t> range;
};

struct SampleAmplitudeEnvelopeParameters {
    std::optional<std::uint8_t> attack_rate;
    std::optional<std::uint8_t> decay_rate;
    std::optional<std::uint8_t> release_rate;
    std::optional<std::uint8_t> sustain_level;
    std::optional<std::uint8_t> attack_mode;
    std::optional<std::int8_t> rate_key_scaling;
    std::optional<std::int8_t> rate_velocity_sensitivity;
};

struct SampleLfoParameters {
    std::optional<std::uint8_t> wave;
    std::optional<std::uint8_t> speed;
    std::optional<std::uint8_t> delay_time;
    std::optional<bool> key_on_sync;
    std::optional<bool> cutoff_mod_phase_invert;
    std::optional<bool> pitch_mod_phase_invert;
    std::optional<std::uint8_t> cutoff_mod_depth;
    std::optional<std::uint8_t> pitch_mod_depth;
    std::optional<std::uint8_t> amp_mod_depth;
};

struct SampleControlParameters {
    std::optional<std::uint8_t> device;
    std::optional<std::uint8_t> function;
    std::optional<std::uint8_t> type;
    std::optional<std::int8_t> range;
};

struct SampleParameters {
    std::optional<bool> fixed_pitch;
    std::optional<bool> key_crossfade;
    std::optional<bool> mono_mode;
    std::optional<std::uint8_t> sample_eq_type;
    std::optional<std::uint8_t> midi_receive_channel;
    std::optional<std::uint8_t> pitch_bend_type;
    std::optional<std::uint8_t> pitch_bend_range;
    std::optional<std::int8_t> coarse_tune;
    std::optional<std::uint8_t> root_key;
    std::optional<std::int8_t> fine_tune_cents;
    std::optional<std::uint8_t> key_low;
    std::optional<std::uint8_t> key_high;
    std::optional<AudioSamplerLoopMode> loop_mode;
    std::optional<std::uint16_t> loop_tempo_hundredths;
    std::optional<std::uint32_t> loop_start_frame;
    std::optional<std::uint32_t> loop_length_frames;
    std::optional<std::int8_t> wave_start_velocity_sensitivity;
    std::optional<std::uint8_t> filter_type;
    std::optional<std::uint8_t> filter_cutoff;
    std::optional<std::uint8_t> filter_q_width;
    std::optional<std::uint8_t> filter_scaling_break1;
    std::optional<std::uint8_t> filter_scaling_break2;
    std::optional<std::int8_t> filter_scaling_cutoff1;
    std::optional<std::int8_t> filter_scaling_cutoff2;
    std::optional<std::int8_t> filter_velocity_to_cutoff;
    std::optional<std::int8_t> filter_velocity_to_q_width;
    std::optional<std::int8_t> expand_detune;
    std::optional<std::int8_t> expand_dephase;
    std::optional<std::int8_t> expand_width;
    std::optional<std::uint8_t> random_pitch;
    std::optional<std::uint8_t> level;
    std::optional<std::int8_t> pan;
    std::optional<std::uint8_t> velocity_low_limit;
    std::optional<std::int8_t> velocity_offset;
    std::optional<std::uint8_t> velocity_high;
    std::optional<std::uint8_t> velocity_low;
    std::optional<std::uint8_t> level_scaling_break1;
    std::optional<std::uint8_t> level_scaling_break2;
    std::optional<std::uint8_t> level_scaling_level1;
    std::optional<std::uint8_t> level_scaling_level2;
    std::optional<std::int8_t> velocity_sensitivity;
    std::optional<std::uint8_t> alternate_group;
    std::optional<std::uint8_t> sample_eq_frequency;
    std::optional<std::int8_t> sample_eq_gain_db;
    std::optional<std::uint8_t> sample_eq_width_tenths;
    std::optional<std::int8_t> filter_cutoff_distance;
    SampleFilterEnvelopeParameters feg;
    SamplePitchEnvelopeParameters peg;
    SampleAmplitudeEnvelopeParameters aeg;
    SampleLfoParameters lfo;
    std::optional<std::int8_t> filter_gain;
    std::array<SampleControlParameters, 6> controls;
    // A3000 switch; current layouts store independent low/high crossfade widths.
    std::optional<bool> velocity_crossfade;
    std::optional<std::uint8_t> velocity_xfade_high;
    std::optional<std::uint8_t> velocity_xfade_low;
    std::optional<std::uint8_t> output1_destination;
    std::optional<std::uint8_t> output1_level;
    std::optional<std::uint8_t> output2_destination;
    std::optional<std::uint8_t> output2_level;
    std::optional<std::uint8_t> portamento_type;
    std::optional<std::uint8_t> portamento_rate;
    std::optional<std::uint8_t> portamento_time;
};

enum class SampleParameterGeneration : std::uint8_t { a3000, current };

// Stored lane state, including invalid or inactive values. No names or handles
// are present in a parameter block, so neither lane's activity is inferred.
struct SampleParameterMemberState {
    std::uint8_t root_key{};
    std::uint16_t sample_rate{};
    std::int8_t fine_tune_cents{};
    std::uint16_t pitch_base_word{};
    std::uint32_t wave_start_frame{};
    std::uint32_t wave_length_frames{};
    std::uint32_t loop_start_frame{};
    std::uint32_t loop_length_frames{};

    friend bool operator==(const SampleParameterMemberState &, const SampleParameterMemberState &) = default;
};

struct DecodedSampleParameters {
    SampleParameterGeneration generation{SampleParameterGeneration::current};
    SampleParameters parameters;
    std::array<SampleParameterMemberState, 2> members{};
    std::array<std::uint32_t, 4> linked_program_bitmap_words{};
    std::uint8_t sample_flags{};
    std::uint8_t mapout_flags{};
    std::array<std::int16_t, 5> eq_coefficients{};
    std::uint32_t cached_wave_end{};
    std::uint32_t cached_loop_end{};
    std::optional<bool> controller_copies_match;
    std::vector<std::byte> raw_bytes;
};

// Requires exactly 0xbc native or 0xbc/0xe0 current bytes. Short current blocks
// use prefix controllers and omit extension-only parameters. Scalar leaves outside
// their known domains remain absent; raw bytes and both member lanes are kept.
// The root/fine-tune/loop-window convenience leaves refer to the first lane;
// they do not assert equality with the second lane. This is not a write plan.
// Native EQ type has no promoted typed interpretation; its raw bits are kept.
AXK_API Result<DecodedSampleParameters> decode_sample_parameter_block(std::span<const std::byte> bytes,
                                                                      SampleParameterGeneration generation);

} // namespace axk
