#include "axklib/sample_parameter_json.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace axk::detail {

nlohmann::json sample_parameters_json(const SampleParameters &value, std::vector<std::string> *unavailable) {
    auto result = nlohmann::json::object();
#define AXK_FIELD(member)                                                                                              \
    if (value.member)                                                                                                  \
        result[#member] = *value.member;                                                                               \
    else if (unavailable)                                                                                              \
    unavailable->emplace_back(#member)
    AXK_FIELD(fixed_pitch);
    AXK_FIELD(key_crossfade);
    AXK_FIELD(mono_mode);
    AXK_FIELD(sample_eq_type);
    AXK_FIELD(midi_receive_channel);
    AXK_FIELD(pitch_bend_type);
    AXK_FIELD(pitch_bend_range);
    AXK_FIELD(coarse_tune);
    AXK_FIELD(root_key);
    AXK_FIELD(fine_tune_cents);
    AXK_FIELD(key_low);
    AXK_FIELD(key_high);
    AXK_FIELD(loop_mode);
    AXK_FIELD(loop_tempo_hundredths);
    AXK_FIELD(loop_start_frame);
    AXK_FIELD(loop_length_frames);
    AXK_FIELD(wave_start_velocity_sensitivity);
    AXK_FIELD(filter_type);
    AXK_FIELD(filter_cutoff);
    AXK_FIELD(filter_q_width);
    AXK_FIELD(filter_scaling_break1);
    AXK_FIELD(filter_scaling_break2);
    AXK_FIELD(filter_scaling_cutoff1);
    AXK_FIELD(filter_scaling_cutoff2);
    AXK_FIELD(filter_velocity_to_cutoff);
    AXK_FIELD(filter_velocity_to_q_width);
    AXK_FIELD(expand_detune);
    AXK_FIELD(expand_dephase);
    AXK_FIELD(expand_width);
    AXK_FIELD(random_pitch);
    AXK_FIELD(level);
    AXK_FIELD(pan);
    AXK_FIELD(velocity_low_limit);
    AXK_FIELD(velocity_offset);
    AXK_FIELD(velocity_high);
    AXK_FIELD(velocity_low);
    AXK_FIELD(level_scaling_break1);
    AXK_FIELD(level_scaling_break2);
    AXK_FIELD(level_scaling_level1);
    AXK_FIELD(level_scaling_level2);
    AXK_FIELD(velocity_sensitivity);
    AXK_FIELD(alternate_group);
    AXK_FIELD(sample_eq_frequency);
    AXK_FIELD(sample_eq_gain_db);
    AXK_FIELD(sample_eq_width_tenths);
    AXK_FIELD(filter_cutoff_distance);
    AXK_FIELD(filter_gain);
    AXK_FIELD(velocity_xfade_high);
    AXK_FIELD(velocity_xfade_low);
    AXK_FIELD(output1_destination);
    AXK_FIELD(output1_level);
    AXK_FIELD(output2_destination);
    AXK_FIELD(output2_level);
    AXK_FIELD(portamento_type);
    AXK_FIELD(portamento_rate);
    AXK_FIELD(portamento_time);
#undef AXK_FIELD
#define AXK_GROUP_FIELD(group, member)                                                                                 \
    if (value.group.member)                                                                                            \
        result[#group][#member] = *value.group.member;                                                                 \
    else if (unavailable)                                                                                              \
    unavailable->emplace_back(#group "." #member)
    AXK_GROUP_FIELD(aeg, attack_rate);
    AXK_GROUP_FIELD(aeg, decay_rate);
    AXK_GROUP_FIELD(aeg, release_rate);
    AXK_GROUP_FIELD(aeg, sustain_level);
    AXK_GROUP_FIELD(aeg, attack_mode);
    AXK_GROUP_FIELD(aeg, rate_key_scaling);
    AXK_GROUP_FIELD(aeg, rate_velocity_sensitivity);
    AXK_GROUP_FIELD(feg, attack_rate);
    AXK_GROUP_FIELD(feg, decay_rate);
    AXK_GROUP_FIELD(feg, release_rate);
    AXK_GROUP_FIELD(feg, init_level);
    AXK_GROUP_FIELD(feg, attack_level);
    AXK_GROUP_FIELD(feg, sustain_level);
    AXK_GROUP_FIELD(feg, release_level);
    AXK_GROUP_FIELD(feg, rate_key_scaling);
    AXK_GROUP_FIELD(feg, rate_velocity_sensitivity);
    AXK_GROUP_FIELD(feg, attack_level_velocity_sensitivity);
    AXK_GROUP_FIELD(feg, level_velocity_sensitivity);
    AXK_GROUP_FIELD(peg, attack_rate);
    AXK_GROUP_FIELD(peg, decay_rate);
    AXK_GROUP_FIELD(peg, release_rate);
    AXK_GROUP_FIELD(peg, init_level);
    AXK_GROUP_FIELD(peg, attack_level);
    AXK_GROUP_FIELD(peg, sustain_level);
    AXK_GROUP_FIELD(peg, release_level);
    AXK_GROUP_FIELD(peg, rate_key_scaling);
    AXK_GROUP_FIELD(peg, rate_velocity_sensitivity);
    AXK_GROUP_FIELD(peg, level_velocity_sensitivity);
    AXK_GROUP_FIELD(peg, range);
    AXK_GROUP_FIELD(lfo, wave);
    AXK_GROUP_FIELD(lfo, speed);
    AXK_GROUP_FIELD(lfo, delay_time);
    AXK_GROUP_FIELD(lfo, key_on_sync);
    AXK_GROUP_FIELD(lfo, cutoff_mod_phase_invert);
    AXK_GROUP_FIELD(lfo, pitch_mod_phase_invert);
    AXK_GROUP_FIELD(lfo, cutoff_mod_depth);
    AXK_GROUP_FIELD(lfo, pitch_mod_depth);
    AXK_GROUP_FIELD(lfo, amp_mod_depth);
#undef AXK_GROUP_FIELD
    auto controls = nlohmann::json::object();
    bool has_controls = false;
    std::size_t index{};
    for (const auto &control : value.controls) {
        auto row = nlohmann::json::object();
#define AXK_CONTROL(member)                                                                                            \
    if (control.member)                                                                                                \
        row[#member] = *control.member;                                                                                \
    else if (unavailable)                                                                                              \
    unavailable->push_back("controls." + std::to_string(index + 1U) + "." #member)
        AXK_CONTROL(device);
        AXK_CONTROL(function);
        AXK_CONTROL(type);
        AXK_CONTROL(range);
#undef AXK_CONTROL
        has_controls = has_controls || !row.empty();
        ++index;
        if (!row.empty())
            controls[std::to_string(index)] = std::move(row);
    }
    if (has_controls)
        result["controls"] = std::move(controls);
    return result;
}

} // namespace axk::detail
