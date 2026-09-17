#include "axklib/program_parameter_json.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "program_parameter_json_internal.hpp"

namespace axk::detail {
namespace {
using namespace program_json_internal;

#define AXK_READ(member)                                                                                               \
    if (auto read_result = read(value, #member, result.member); !read_result)                                          \
        return std::unexpected { read_result.error() }
#define AXK_FIELDS(...)                                                                                                \
    if (auto valid = fields(value, {__VA_ARGS__}); !valid)                                                             \
        return std::unexpected { valid.error() }
#define AXK_CHILD(member, parser)                                                                                      \
    if (auto parsed = child(value, #member, result.member, parser); !parsed)                                           \
        return std::unexpected { parsed.error() }

Result<ProgramChannelMap> channel_map(const Json &value) {
    AXK_FIELDS("a", "b");
    ProgramChannelMap result;
    if (auto parsed = read_numbered(value, "a", result.a, scalar<bool>); !parsed)
        return std::unexpected{parsed.error()};
    if (auto parsed = read_numbered(value, "b", result.b, scalar<bool>); !parsed)
        return std::unexpected{parsed.error()};
    return result;
}

Result<ProgramPortamentoParameters> portamento(const Json &value) {
    AXK_FIELDS("type", "rate", "time");
    ProgramPortamentoParameters result;
    AXK_READ(type);
    AXK_READ(rate);
    AXK_READ(time);
    return result;
}

Result<ProgramLfoParameters> lfo(const Json &value) {
    AXK_FIELDS("cycle", "sync", "wave", "initial_phase", "tempo", "reset_channel", "reset_note", "sample_hold_speed");
    ProgramLfoParameters result;
    AXK_READ(cycle);
    AXK_READ(sync);
    AXK_READ(wave);
    AXK_READ(initial_phase);
    AXK_READ(tempo);
    AXK_READ(reset_channel);
    AXK_READ(reset_note);
    AXK_READ(sample_hold_speed);
    return result;
}

constexpr std::array<std::string_view, 4> slopes{"none", "rising", "falling", "both"};

Result<ProgramStepWaveParameters> step_wave(const Json &value) {
    AXK_FIELDS("step_count", "slope", "values");
    ProgramStepWaveParameters result;
    AXK_READ(step_count);
    if (value.contains("slope")) {
        if (!value["slope"].is_string())
            return std::unexpected{invalid("StepWave slope must be a name")};
        const auto name = value["slope"].get<std::string>();
        const auto found = std::ranges::find(slopes, name);
        if (found == slopes.end())
            return std::unexpected{invalid("unknown StepWave slope")};
        result.slope = static_cast<ProgramStepWaveSlope>(found - slopes.begin());
    }
    if (auto parsed = read_numbered(value, "values", result.values, scalar<std::uint8_t>); !parsed)
        return std::unexpected{parsed.error()};
    return result;
}

Result<ProgramAdOutputParameters> ad_output(const Json &value) {
    AXK_FIELDS("destination", "level");
    ProgramAdOutputParameters result;
    AXK_READ(destination);
    AXK_READ(level);
    return result;
}

Result<ProgramAdChannelParameters> ad_channel(const Json &value) {
    AXK_FIELDS("pan", "output1", "output2");
    ProgramAdChannelParameters result;
    AXK_READ(pan);
    AXK_CHILD(output1, ad_output);
    AXK_CHILD(output2, ad_output);
    return result;
}

Result<ProgramAdParameters> ad(const Json &value) {
    AXK_FIELDS("enabled", "source", "left", "right");
    ProgramAdParameters result;
    AXK_READ(enabled);
    AXK_READ(source);
    AXK_CHILD(left, ad_channel);
    AXK_CHILD(right, ad_channel);
    return result;
}

Result<void> controller(const Json &value, ProgramControllerParameters &result) {
    AXK_FIELDS("device", "function", "type", "range");
    AXK_READ(device);
    AXK_READ(function);
    AXK_READ(type);
    AXK_READ(range);
    return {};
}

Result<void> effect(const Json &value, ProgramEffectParameters &result) {
    AXK_FIELDS("enabled", "input_level", "output_level", "pan", "width", "destination", "type", "parameters");
    AXK_READ(enabled);
    AXK_READ(input_level);
    AXK_READ(output_level);
    AXK_READ(pan);
    AXK_READ(width);
    AXK_READ(destination);
    AXK_READ(type);
    return read_numbered(value, "parameters", result.parameters, scalar<std::uint16_t>);
}

template <typename T> Json channel_map_json(const T &value) {
    auto result = Json::object();
    write_group(result, "a", write_numbered(value.a, scalar_json<bool>));
    write_group(result, "b", write_numbered(value.b, scalar_json<bool>));
    return result;
}

#define AXK_WRITE(member) write(result, #member, value.member)

Json ad_channel_json(const ProgramAdChannelParameters &value) {
    auto result = Json::object();
    AXK_WRITE(pan);
    auto output1 = Json::object();
    write(output1, "destination", value.output1.destination);
    write(output1, "level", value.output1.level);
    auto output2 = Json::object();
    write(output2, "destination", value.output2.destination);
    write(output2, "level", value.output2.level);
    write_group(result, "output1", std::move(output1));
    write_group(result, "output2", std::move(output2));
    return result;
}

Json effect_json(const ProgramEffectParameters &value) {
    auto result = Json::object();
    AXK_WRITE(enabled);
    AXK_WRITE(input_level);
    AXK_WRITE(output_level);
    AXK_WRITE(pan);
    AXK_WRITE(width);
    AXK_WRITE(destination);
    AXK_WRITE(type);
    write_group(result, "parameters", write_numbered(value.parameters, scalar_json<std::uint16_t>));
    return result;
}

Json controller_json(const ProgramControllerParameters &value) {
    auto result = Json::object();
    AXK_WRITE(device);
    AXK_WRITE(function);
    AXK_WRITE(type);
    AXK_WRITE(range);
    return result;
}

} // namespace

Result<ProgramParameters> parse_program_parameters_json(const nlohmann::json &value) {
    AXK_FIELDS("level", "transpose", "controller_reset", "note_toggle", "portamento", "lfo", "step_wave", "ad",
               "controllers", "effect_connections", "effects");
    ProgramParameters result;
    AXK_READ(level);
    AXK_READ(transpose);
    AXK_CHILD(controller_reset, channel_map);
    AXK_CHILD(note_toggle, channel_map);
    AXK_CHILD(portamento, portamento);
    AXK_CHILD(lfo, lfo);
    AXK_CHILD(step_wave, step_wave);
    AXK_CHILD(ad, ad);
    if (auto parsed = read_numbered(value, "controllers", result.controllers, controller); !parsed)
        return std::unexpected{parsed.error()};
    if (auto parsed = read_numbered(value, "effect_connections", result.effect_connections, scalar<std::uint8_t>);
        !parsed)
        return std::unexpected{parsed.error()};
    if (auto parsed = read_numbered(value, "effects", result.effects, effect); !parsed)
        return std::unexpected{parsed.error()};
    return result;
}

nlohmann::json program_parameters_json(const ProgramParameters &value) {
    auto result = Json::object();
    AXK_WRITE(level);
    AXK_WRITE(transpose);
    write_group(result, "controller_reset", channel_map_json(value.controller_reset));
    write_group(result, "note_toggle", channel_map_json(value.note_toggle));
    auto port = Json::object();
    write(port, "type", value.portamento.type);
    write(port, "rate", value.portamento.rate);
    write(port, "time", value.portamento.time);
    write_group(result, "portamento", std::move(port));
    auto lfo_value = Json::object();
#define AXK_LFO(member) write(lfo_value, #member, value.lfo.member)
    AXK_LFO(cycle);
    AXK_LFO(sync);
    AXK_LFO(wave);
    AXK_LFO(initial_phase);
    AXK_LFO(tempo);
    AXK_LFO(reset_channel);
    AXK_LFO(reset_note);
    AXK_LFO(sample_hold_speed);
#undef AXK_LFO
    write_group(result, "lfo", std::move(lfo_value));
    auto steps = Json::object();
    write(steps, "step_count", value.step_wave.step_count);
    if (value.step_wave.slope)
        steps["slope"] = slopes.at(static_cast<std::size_t>(*value.step_wave.slope));
    write_group(steps, "values", write_numbered(value.step_wave.values, scalar_json<std::uint8_t>));
    write_group(result, "step_wave", std::move(steps));
    auto ad_value = Json::object();
    write(ad_value, "enabled", value.ad.enabled);
    write(ad_value, "source", value.ad.source);
    write_group(ad_value, "left", ad_channel_json(value.ad.left));
    write_group(ad_value, "right", ad_channel_json(value.ad.right));
    write_group(result, "ad", std::move(ad_value));
    write_group(result, "controllers", write_numbered(value.controllers, controller_json));
    write_group(result, "effect_connections", write_numbered(value.effect_connections, scalar_json<std::uint8_t>));
    write_group(result, "effects", write_numbered(value.effects, effect_json));
    return result;
}

#undef AXK_READ
#undef AXK_FIELDS
#undef AXK_CHILD
#undef AXK_WRITE

} // namespace axk::detail
