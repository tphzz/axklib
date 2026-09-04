#include "axklib/sample_parameter_json.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <type_traits>

#include <nlohmann/json.hpp>

#include "axklib/writer_internal.hpp"

namespace axk::detail {
namespace {

using Json = nlohmann::json;

Error invalid(ErrorCode code, ErrorCategory category, std::string message) {
    return make_error(code, category, std::move(message));
}

Result<void> fields(const Json &value, std::string_view context, std::initializer_list<std::string_view> allowed,
                    ErrorCode code, ErrorCategory category) {
    if (!value.is_object())
        return std::unexpected{invalid(code, category, std::string{context} + " must be a JSON object")};
    for (const auto &[name, ignored] : value.items()) {
        static_cast<void>(ignored);
        if (!std::ranges::contains(allowed, name))
            return std::unexpected{invalid(code, category, std::string{context} + " has unknown field: " + name)};
    }
    return {};
}

template <typename T>
Result<void> integer_field(const Json &value, std::string_view name, std::optional<T> &target, std::string_view context,
                           ErrorCode code, ErrorCategory category) {
    if (!value.contains(name))
        return {};
    const auto &source = value[name];
    if (!source.is_number_integer()) {
        return std::unexpected{
            invalid(code, category, std::string{context} + "." + std::string{name} + " must be an integer")};
    }
    if constexpr (std::is_signed_v<T>) {
        const auto parsed = source.get<std::int64_t>();
        if (parsed < std::numeric_limits<T>::min() || parsed > std::numeric_limits<T>::max()) {
            return std::unexpected{invalid(
                code, category, std::string{context} + "." + std::string{name} + " is outside its supported range")};
        }
        target = static_cast<T>(parsed);
    } else {
        if (source.is_number_integer() && source.get<std::int64_t>() < 0) {
            return std::unexpected{invalid(
                code, category, std::string{context} + "." + std::string{name} + " is outside its supported range")};
        }
        const auto parsed = source.get<std::uint64_t>();
        if (parsed > std::numeric_limits<T>::max()) {
            return std::unexpected{invalid(
                code, category, std::string{context} + "." + std::string{name} + " is outside its supported range")};
        }
        target = static_cast<T>(parsed);
    }
    return {};
}

Result<void> boolean_field(const Json &value, std::string_view name, std::optional<bool> &target,
                           std::string_view context, ErrorCode code, ErrorCategory category) {
    if (!value.contains(name))
        return {};
    if (!value[name].is_boolean()) {
        return std::unexpected{
            invalid(code, category, std::string{context} + "." + std::string{name} + " must be a boolean")};
    }
    target = value[name].get<bool>();
    return {};
}

template <typename Envelope>
Result<void> parse_envelope(const Json &value, Envelope &result, std::string_view context, ErrorCode code,
                            ErrorCategory category) {
    if constexpr (std::is_same_v<Envelope, SampleFilterEnvelopeParameters>) {
        if (auto valid = fields(value, context,
                                {"attack_rate", "decay_rate", "release_rate", "init_level", "attack_level",
                                 "sustain_level", "release_level", "rate_key_scaling", "rate_velocity_sensitivity",
                                 "attack_level_velocity_sensitivity", "level_velocity_sensitivity"},
                                code, category);
            !valid) {
            return valid;
        }
    } else if constexpr (std::is_same_v<Envelope, SamplePitchEnvelopeParameters>) {
        if (auto valid = fields(value, context,
                                {"attack_rate", "decay_rate", "release_rate", "init_level", "attack_level",
                                 "sustain_level", "release_level", "rate_key_scaling", "rate_velocity_sensitivity",
                                 "level_velocity_sensitivity", "range"},
                                code, category);
            !valid) {
            return valid;
        }
    } else {
        if (auto valid = fields(value, context,
                                {"attack_rate", "decay_rate", "release_rate", "sustain_level", "attack_mode",
                                 "rate_key_scaling", "rate_velocity_sensitivity"},
                                code, category);
            !valid) {
            return valid;
        }
    }
    if (value.empty())
        return std::unexpected{invalid(code, category, std::string{context} + " must not be empty")};
#define AXK_INTEGER(member)                                                                                            \
    if constexpr (requires { result.member; })                                                                         \
        if (auto parsed = integer_field(value, #member, result.member, context, code, category); !parsed)              \
    return parsed
    AXK_INTEGER(attack_rate);
    AXK_INTEGER(decay_rate);
    AXK_INTEGER(release_rate);
    AXK_INTEGER(init_level);
    AXK_INTEGER(attack_level);
    AXK_INTEGER(sustain_level);
    AXK_INTEGER(release_level);
    AXK_INTEGER(rate_key_scaling);
    AXK_INTEGER(rate_velocity_sensitivity);
    AXK_INTEGER(attack_level_velocity_sensitivity);
    AXK_INTEGER(level_velocity_sensitivity);
    AXK_INTEGER(range);
    AXK_INTEGER(attack_mode);
#undef AXK_INTEGER
    return {};
}

Result<void> parse_lfo(const Json &value, SampleLfoParameters &result, std::string_view context, ErrorCode code,
                       ErrorCategory category) {
    if (auto valid = fields(value, context,
                            {"wave", "speed", "delay_time", "key_on_sync", "cutoff_mod_phase_invert",
                             "pitch_mod_phase_invert", "cutoff_mod_depth", "pitch_mod_depth", "amp_mod_depth"},
                            code, category);
        !valid) {
        return valid;
    }
    if (value.empty())
        return std::unexpected{invalid(code, category, std::string{context} + " must not be empty")};
#define AXK_INTEGER(member)                                                                                            \
    if (auto parsed = integer_field(value, #member, result.member, context, code, category); !parsed)                  \
    return parsed
    AXK_INTEGER(wave);
    AXK_INTEGER(speed);
    AXK_INTEGER(delay_time);
    AXK_INTEGER(cutoff_mod_depth);
    AXK_INTEGER(pitch_mod_depth);
    AXK_INTEGER(amp_mod_depth);
#undef AXK_INTEGER
#define AXK_BOOLEAN(member)                                                                                            \
    if (auto parsed = boolean_field(value, #member, result.member, context, code, category); !parsed)                  \
    return parsed
    AXK_BOOLEAN(key_on_sync);
    AXK_BOOLEAN(cutoff_mod_phase_invert);
    AXK_BOOLEAN(pitch_mod_phase_invert);
#undef AXK_BOOLEAN
    return {};
}

Result<void> parse_controls(const Json &value, std::array<SampleControlParameters, 6> &result, std::string_view context,
                            ErrorCode code, ErrorCategory category) {
    if (!value.is_object())
        return std::unexpected{invalid(code, category, std::string{context} + " must be a JSON object")};
    if (value.empty())
        return std::unexpected{invalid(code, category, std::string{context} + " must not be empty")};
    for (const auto &[name, control] : value.items()) {
        if (name.size() != 1U || name[0] < '1' || name[0] > '6')
            return std::unexpected{invalid(code, category, std::string{context} + " has unknown field: " + name)};
        const auto item_context = std::string{context} + "." + name;
        if (auto valid = fields(control, item_context, {"device", "function", "type", "range"}, code, category);
            !valid) {
            return valid;
        }
        if (control.empty())
            return std::unexpected{invalid(code, category, item_context + " must not be empty")};
        auto &target = result[static_cast<std::size_t>(name[0] - '1')];
        if (auto parsed = integer_field(control, "device", target.device, item_context, code, category); !parsed)
            return parsed;
        if (auto parsed = integer_field(control, "function", target.function, item_context, code, category); !parsed)
            return parsed;
        if (auto parsed = integer_field(control, "type", target.type, item_context, code, category); !parsed)
            return parsed;
        if (auto parsed = integer_field(control, "range", target.range, item_context, code, category); !parsed)
            return parsed;
    }
    return {};
}

} // namespace

Result<SampleParameters> parse_sample_parameters_json(const Json &value, std::string_view context,
                                                      bool require_nonempty, ErrorCode error_code,
                                                      ErrorCategory error_category) {
    if (auto valid = fields(value, context,
                            {"fixed_pitch",
                             "key_crossfade",
                             "mono_mode",
                             "sample_eq_type",
                             "midi_receive_channel",
                             "pitch_bend_type",
                             "pitch_bend_range",
                             "coarse_tune",
                             "root_key",
                             "fine_tune_cents",
                             "key_low",
                             "key_high",
                             "loop_mode",
                             "loop_tempo_hundredths",
                             "loop_start_frame",
                             "loop_length_frames",
                             "wave_start_velocity_sensitivity",
                             "filter_type",
                             "filter_cutoff",
                             "filter_q_width",
                             "filter_scaling_break1",
                             "filter_scaling_break2",
                             "filter_scaling_cutoff1",
                             "filter_scaling_cutoff2",
                             "filter_velocity_to_cutoff",
                             "filter_velocity_to_q_width",
                             "expand_detune",
                             "expand_dephase",
                             "expand_width",
                             "random_pitch",
                             "level",
                             "pan",
                             "velocity_low_limit",
                             "velocity_offset",
                             "velocity_high",
                             "velocity_low",
                             "level_scaling_break1",
                             "level_scaling_break2",
                             "level_scaling_level1",
                             "level_scaling_level2",
                             "velocity_sensitivity",
                             "alternate_group",
                             "sample_eq_frequency",
                             "sample_eq_gain_db",
                             "sample_eq_width_tenths",
                             "filter_cutoff_distance",
                             "feg",
                             "peg",
                             "aeg",
                             "lfo",
                             "filter_gain",
                             "controls",
                             "velocity_xfade_high",
                             "velocity_xfade_low",
                             "output1_destination",
                             "output1_level",
                             "output2_destination",
                             "output2_level",
                             "portamento_type",
                             "portamento_rate",
                             "portamento_time"},
                            error_code, error_category);
        !valid) {
        return std::unexpected{valid.error()};
    }

    SampleParameters result;
#define AXK_INTEGER(member)                                                                                            \
    if (auto parsed = integer_field(value, #member, result.member, context, error_code, error_category); !parsed)      \
        return std::unexpected { parsed.error() }
    AXK_INTEGER(sample_eq_type);
    AXK_INTEGER(midi_receive_channel);
    AXK_INTEGER(pitch_bend_type);
    AXK_INTEGER(pitch_bend_range);
    AXK_INTEGER(coarse_tune);
    AXK_INTEGER(root_key);
    AXK_INTEGER(fine_tune_cents);
    AXK_INTEGER(key_low);
    AXK_INTEGER(key_high);
    AXK_INTEGER(loop_tempo_hundredths);
    AXK_INTEGER(loop_start_frame);
    AXK_INTEGER(loop_length_frames);
    AXK_INTEGER(wave_start_velocity_sensitivity);
    AXK_INTEGER(filter_type);
    AXK_INTEGER(filter_cutoff);
    AXK_INTEGER(filter_q_width);
    AXK_INTEGER(filter_scaling_break1);
    AXK_INTEGER(filter_scaling_break2);
    AXK_INTEGER(filter_scaling_cutoff1);
    AXK_INTEGER(filter_scaling_cutoff2);
    AXK_INTEGER(filter_velocity_to_cutoff);
    AXK_INTEGER(filter_velocity_to_q_width);
    AXK_INTEGER(expand_detune);
    AXK_INTEGER(expand_dephase);
    AXK_INTEGER(expand_width);
    AXK_INTEGER(random_pitch);
    AXK_INTEGER(level);
    AXK_INTEGER(pan);
    AXK_INTEGER(velocity_low_limit);
    AXK_INTEGER(velocity_offset);
    AXK_INTEGER(velocity_high);
    AXK_INTEGER(velocity_low);
    AXK_INTEGER(level_scaling_break1);
    AXK_INTEGER(level_scaling_break2);
    AXK_INTEGER(level_scaling_level1);
    AXK_INTEGER(level_scaling_level2);
    AXK_INTEGER(velocity_sensitivity);
    AXK_INTEGER(alternate_group);
    AXK_INTEGER(sample_eq_frequency);
    AXK_INTEGER(sample_eq_gain_db);
    AXK_INTEGER(sample_eq_width_tenths);
    AXK_INTEGER(filter_cutoff_distance);
    AXK_INTEGER(filter_gain);
    AXK_INTEGER(velocity_xfade_high);
    AXK_INTEGER(velocity_xfade_low);
    AXK_INTEGER(output1_destination);
    AXK_INTEGER(output1_level);
    AXK_INTEGER(output2_destination);
    AXK_INTEGER(output2_level);
    AXK_INTEGER(portamento_type);
    AXK_INTEGER(portamento_rate);
    AXK_INTEGER(portamento_time);
#undef AXK_INTEGER
#define AXK_BOOLEAN(member)                                                                                            \
    if (auto parsed = boolean_field(value, #member, result.member, context, error_code, error_category); !parsed)      \
        return std::unexpected { parsed.error() }
    AXK_BOOLEAN(fixed_pitch);
    AXK_BOOLEAN(key_crossfade);
    AXK_BOOLEAN(mono_mode);
#undef AXK_BOOLEAN
    if (value.contains("loop_mode")) {
        std::optional<std::uint8_t> mode;
        if (auto parsed = integer_field(value, "loop_mode", mode, context, error_code, error_category); !parsed)
            return std::unexpected{parsed.error()};
        result.loop_mode = static_cast<AudioSamplerLoopMode>(*mode);
    }
    if (value.contains("feg")) {
        if (auto parsed =
                parse_envelope(value["feg"], result.feg, std::string{context} + ".feg", error_code, error_category);
            !parsed)
            return std::unexpected{parsed.error()};
    }
    if (value.contains("peg")) {
        if (auto parsed =
                parse_envelope(value["peg"], result.peg, std::string{context} + ".peg", error_code, error_category);
            !parsed)
            return std::unexpected{parsed.error()};
    }
    if (value.contains("aeg")) {
        if (auto parsed =
                parse_envelope(value["aeg"], result.aeg, std::string{context} + ".aeg", error_code, error_category);
            !parsed)
            return std::unexpected{parsed.error()};
    }
    if (value.contains("lfo")) {
        if (auto parsed =
                parse_lfo(value["lfo"], result.lfo, std::string{context} + ".lfo", error_code, error_category);
            !parsed)
            return std::unexpected{parsed.error()};
    }
    if (value.contains("controls")) {
        if (auto parsed = parse_controls(value["controls"], result.controls, std::string{context} + ".controls",
                                         error_code, error_category);
            !parsed)
            return std::unexpected{parsed.error()};
    }
    if (auto valid = validate_sample_parameters(result); !valid) {
        return std::unexpected{
            invalid(error_code, error_category, std::string{context} + " contains an unsupported parameter value")};
    }
    if (require_nonempty && !has_sample_parameter_values(result)) {
        return std::unexpected{
            invalid(error_code, error_category, std::string{context} + " must contain at least one parameter")};
    }
    return result;
}

} // namespace axk::detail
