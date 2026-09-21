#include "axklib/sample_bank_overrides.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "axklib/bytes.hpp"
#include "axklib/sample_parameter_codec.hpp"
#include "axklib/sample_parameter_rules.hpp"

namespace axk {
namespace {
Error invalid(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
}
bool enabled(const CurrentSbac &bank, std::uint8_t selector) {
    return (bank.override_enable_words[selector / 32U] & (1U << (selector % 32U))) != 0U;
}
} // namespace

std::vector<SampleBankOverrideUnit> sample_bank_override_units(SampleParameterGeneration generation) {
    std::vector<SampleBankOverrideUnit> result;
    const auto add = [&](std::uint8_t id, std::initializer_list<std::string> keys) {
        result.push_back({id, {id}, keys});
    };
    add(3, {"midi_receive_channel"});
    add(4, {"pitch_bend_type"});
    add(5, {"pitch_bend_range"});
    add(9, {"coarse_tune"});
    add(19, {"wave_start_velocity_sensitivity"});
    add(21, {"filter_type"});
    add(22, {"filter_cutoff"});
    add(23, {"filter_q_width"});
    add(24, {"filter_scaling_break1", "filter_scaling_break2"});
    add(25, {"filter_scaling_cutoff1", "filter_scaling_cutoff2"});
    add(26, {"filter_velocity_to_cutoff"});
    add(27, {"filter_velocity_to_q_width"});
    add(28, {"fixed_pitch"});
    add(29, {"expand_detune"});
    add(30, {"expand_dephase"});
    add(31, {"expand_width"});
    add(32, {"random_pitch"});
    add(33, {"level"});
    add(34, {"pan"});
    add(35, {"velocity_low_limit"});
    add(36, {"velocity_offset"});
    add(37, {"velocity_high"});
    add(38, {"velocity_low"});
    add(39, {"level_scaling_break1", "level_scaling_break2"});
    add(40, {"level_scaling_level1", "level_scaling_level2"});
    add(41, {"velocity_sensitivity"});
    add(42, {"portamento_type"});
    add(43, {"mono_mode"});
    add(44, {"key_crossfade"});
    if (generation == SampleParameterGeneration::a3000)
        add(45, {"velocity_crossfade"});
    else {
        add(46, {"velocity_xfade_low"});
        add(47, {"velocity_xfade_high"});
    }
    add(48, {"alternate_group"});
    if (generation == SampleParameterGeneration::a3000) {
        result.push_back({49, {49, 50, 51}, {"sample_eq_frequency", "sample_eq_gain_db", "sample_eq_width_tenths"}});
    } else
        result.push_back({49,
                          {49, 50, 51, 85},
                          {"sample_eq_frequency", "sample_eq_gain_db", "sample_eq_width_tenths", "sample_eq_type"}});
    add(52, {"filter_cutoff_distance"});
    add(53, {"feg.attack_rate", "feg.decay_rate", "feg.release_rate"});
    add(54, {"feg.init_level", "feg.attack_level", "feg.sustain_level", "feg.release_level"});
    add(55, {"feg.rate_key_scaling"});
    add(56, {"feg.rate_velocity_sensitivity"});
    add(57, {"feg.attack_level_velocity_sensitivity"});
    add(58, {"feg.level_velocity_sensitivity"});
    add(59, {"peg.attack_rate", "peg.decay_rate", "peg.release_rate"});
    add(60, {"peg.init_level", "peg.attack_level", "peg.sustain_level", "peg.release_level"});
    add(61, {"peg.rate_key_scaling"});
    add(62, {"peg.rate_velocity_sensitivity"});
    add(63, {"peg.level_velocity_sensitivity"});
    add(64, {"peg.range"});
    add(65, {"aeg.attack_rate", "aeg.decay_rate", "aeg.release_rate"});
    add(66, {"aeg.sustain_level"});
    add(67, {"aeg.rate_key_scaling"});
    add(68, {"aeg.rate_velocity_sensitivity"});
    add(69, {"aeg.attack_mode"});
    add(70, {"lfo.wave"});
    add(71, {"lfo.speed"});
    add(72, {"lfo.delay_time"});
    add(73, {"lfo.key_on_sync"});
    add(74, {"lfo.pitch_mod_phase_invert"});
    add(75, {"lfo.cutoff_mod_phase_invert"});
    add(76, {"lfo.cutoff_mod_depth"});
    add(77, {"lfo.pitch_mod_depth"});
    add(78, {"lfo.amp_mod_depth"});
    add(79, {"output1_destination"});
    add(80, {"output1_level"});
    add(81, {"output2_destination"});
    add(82, {"output2_level"});
    add(83, {});
    for (std::size_t i = 0; i < 6U; ++i)
        for (const auto field : {"device", "function", "type", "range"})
            result.back().keys.push_back("controls." + std::to_string(i + 1U) + "." + field);
    add(84, {"filter_gain"});
    if (generation != SampleParameterGeneration::a3000) {
        add(87, {"portamento_rate"});
        add(88, {"portamento_time"});
    }
    return result;
}

bool sample_bank_override_state_supported(const CurrentSbac &bank) {
    const auto generation = sample_parameter_generation(bank.storage.format);
    if (!generation || !bank.storage.structurally_valid)
        return false;
    std::array<std::uint32_t, 3> allowed{};
    for (const auto &unit : sample_bank_override_units(*generation))
        for (const auto selector : unit.selectors)
            allowed[selector / 32U] |= 1U << (selector % 32U);
    for (std::size_t i = 0; i < allowed.size(); ++i)
        if ((bank.override_enable_words[i] & ~allowed[i]) != 0U)
            return false;
    return true;
}

Result<void> apply_sample_bank_overrides(std::vector<std::byte> &payload, const SampleBankOverrideEdit &edit) {
    const auto decoded = decode_object(payload);
    if (!decoded)
        return std::unexpected{decoded.error()};
    const auto *bank = std::get_if<CurrentSbac>(&decoded->payload);
    if (!bank || !sample_bank_override_state_supported(*bank))
        return std::unexpected{invalid("Sample Bank has an unsupported storage or override state; it is preserved")};
    const auto generation = *sample_parameter_generation(bank->storage.format);
    const auto units = sample_bank_override_units(generation);
    auto words = bank->override_enable_words;
    std::set<std::uint8_t> touched;
    for (const bool enable : {true, false}) {
        for (const auto id : enable ? edit.enable : edit.disable) {
            const auto unit = std::ranges::find(units, id, &SampleBankOverrideUnit::id);
            if (unit == units.end() || !touched.insert(id).second)
                return std::unexpected{invalid("Override units must be supported, unique and disjoint")};
            for (const auto selector : unit->selectors) {
                const auto mask = 1U << (selector % 32U);
                auto &word = words[selector / 32U];
                word = enable ? word | mask : word & ~mask;
            }
        }
    }
    bool changed_values = false;
    std::set<std::uint8_t> validate_units(edit.enable.begin(), edit.enable.end());
    for (const auto &rule : sample_parameter_rules()) {
        if (!rule.get(edit.parameters))
            continue;
        changed_values = true;
        const auto unit =
            std::ranges::find_if(units, [&](const auto &item) { return std::ranges::contains(item.keys, rule.key); });
        if (unit == units.end() || std::ranges::contains(edit.disable, unit->id) ||
            (!std::ranges::contains(edit.enable, unit->id) &&
             !std::ranges::any_of(unit->selectors, [&](auto selector) { return enabled(*bank, selector); })))
            return std::unexpected{invalid("Only enabled bank override values may be changed")};
        for (const auto selector : unit->selectors)
            words[selector / 32U] |= 1U << (selector % 32U);
        validate_units.insert(unit->id);
    }
    if (!changed_values && touched.empty())
        return std::unexpected{invalid("No bank override edit was supplied")};
    auto parameters = bank->raw_sample_parameter_block;
    const bool native = generation == SampleParameterGeneration::a3000;
    if (auto applied = detail::apply_sample_parameters_to_block(
            std::span{parameters}.first(native ? 188U : 224U), edit.parameters,
            native ? detail::SampleParameterLayout::a3000 : detail::SampleParameterLayout::a4000_a5000);
        !applied)
        return applied;
    // Activation must not make invalid dormant grouped values effective.
    for (const auto id : validate_units) {
        const auto &unit = *std::ranges::find(units, id, &SampleBankOverrideUnit::id);
        SampleParameters activated;
        for (const auto &key : unit.keys) {
            const auto rule = std::ranges::find(sample_parameter_rules(), key, &SampleParameterRule::key);
            const auto location = sample_parameter_location(*rule, generation);
            const auto value = location ? read_sample_parameter_value(parameters, *location) : std::nullopt;
            if (!value || !sample_parameter_value_allowed(*location, *value))
                return std::unexpected{invalid("The enabled override group contains an unsupported value: " + key)};
            rule->set(activated, *value);
        }
        if (auto valid = detail::validate_sample_parameter_fields(activated, generation); !valid)
            return valid;
    }
    if (validate_units.contains(52) && (words[0] & (1U << 21U)) != 0U) {
        const auto rule = std::ranges::find(sample_parameter_rules(), "filter_type", &SampleParameterRule::key);
        const auto type = read_sample_parameter_value(parameters, *sample_parameter_location(*rule, generation));
        if (!type || *type < 10 || *type > 17)
            return std::unexpected{
                invalid("Cutoff Distance requires a dual-filter bank override or inherited Filter Type")};
    }
    auto candidate = payload;
    std::copy_n(parameters.begin(), 188U, candidate.begin() + 0x78U);
    if (native) {
        if (edit.parameters.controls[0].device)
            candidate[0x6cU] = parameters[0];
        if (edit.parameters.controls[0].function)
            candidate[0x6dU] = parameters[1];
        if (edit.parameters.controls[0].type)
            candidate[0x6eU] = parameters[2];
    } else
        std::copy_n(parameters.begin() + 188U, 36U,
                    candidate.begin() + static_cast<std::ptrdiff_t>(*bank->parameter_tail_offset));
    ByteWriter writer{candidate};
    for (std::size_t i = 0; i < words.size(); ++i)
        if (auto written = writer.write_be32(0x134U + i * 4U, words[i]); !written)
            return written;
    payload = std::move(candidate);
    return {};
}
} // namespace axk
