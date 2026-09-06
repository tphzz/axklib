#include "axklib/program_parameter_json.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "program_parameter_json_internal.hpp"

namespace axk::detail {
namespace {
using namespace program_json_internal;

Result<ProgramReceiveSetting> receive(const Json &value) {
    if (value == "inherit")
        return ProgramReceiveInherit{};
    if (value == "basic")
        return ProgramReceiveBasic{};
    if (auto valid = fields(value, {"port", "channel"}); !valid)
        return std::unexpected{valid.error()};
    if (!value.contains("port") || (value["port"] != "a" && value["port"] != "b") || !value.contains("channel"))
        return std::unexpected{invalid("receive requires a port and a channel")};
    std::optional<std::uint8_t> channel;
    if (auto parsed = read(value, "channel", channel); !parsed)
        return std::unexpected{parsed.error()};
    if (*channel < 1U || *channel > 16U)
        return std::unexpected{invalid("receive channel must be 1..16")};
    return ProgramReceiveChannel{value["port"] == "a" ? MidiPort::a : MidiPort::b, *channel};
}

Result<void> read_switch(const Json &value, std::string_view name, std::optional<ProgramInheritableSwitch> &target) {
    if (!value.contains(name))
        return {};
    if (value[name] == "inherit")
        target = ProgramInheritableSwitch::inherit;
    else if (value[name] == "off")
        target = ProgramInheritableSwitch::off;
    else if (value[name] == "on")
        target = ProgramInheritableSwitch::on;
    else
        return std::unexpected{invalid("switch must be inherit, off, or on")};
    return {};
}

Json receive_json(const ProgramReceiveSetting &value) {
    if (std::holds_alternative<ProgramReceiveInherit>(value))
        return "inherit";
    if (std::holds_alternative<ProgramReceiveBasic>(value))
        return "basic";
    const auto &channel = std::get<ProgramReceiveChannel>(value);
    if ((channel.port != MidiPort::a && channel.port != MidiPort::b) || channel.channel < 1U || channel.channel > 16U)
        throw std::invalid_argument{"Invalid Program receive channel"};
    return {{"port", channel.port == MidiPort::a ? "a" : "b"}, {"channel", channel.channel}};
}

void write_switch(Json &result, std::string_view name, const std::optional<ProgramInheritableSwitch> &value) {
    if (!value)
        return;
    switch (*value) {
    case ProgramInheritableSwitch::inherit:
        result[name] = "inherit";
        break;
    case ProgramInheritableSwitch::off:
        result[name] = "off";
        break;
    case ProgramInheritableSwitch::on:
        result[name] = "on";
        break;
    default:
        throw std::invalid_argument{"Invalid Program inheritance switch"};
    }
}

} // namespace

Result<ProgramAssignmentParameters> parse_program_assignment_parameters_json(const nlohmann::json &value) {
    if (auto valid = fields(value, {"receive",
                                    "level_offset",
                                    "velocity_sensitivity_offset",
                                    "pan_offset",
                                    "high_velocity_crossfade_offset",
                                    "fine_tune_offset",
                                    "low_velocity_crossfade_offset",
                                    "coarse_tune_offset",
                                    "output1",
                                    "output2",
                                    "key_high",
                                    "key_low",
                                    "key_shift",
                                    "velocity_high",
                                    "velocity_low",
                                    "portamento",
                                    "mono",
                                    "key_crossfade",
                                    "alternate_group",
                                    "amp_attack_offset",
                                    "amp_decay_offset",
                                    "amp_release_offset",
                                    "filter_cutoff_offset",
                                    "filter_gain_offset",
                                    "filter_q_offset",
                                    "filter_cutoff_distance_offset",
                                    "output1_level_offset",
                                    "output2_level_offset",
                                    "midi_control"});
        !valid)
        return std::unexpected{valid.error()};
    ProgramAssignmentParameters result;
#define AXK_READ(member)                                                                                               \
    if (auto parsed = read(value, #member, result.member); !parsed)                                                    \
        return std::unexpected { parsed.error() }
    AXK_READ(level_offset);
    AXK_READ(velocity_sensitivity_offset);
    AXK_READ(pan_offset);
    AXK_READ(high_velocity_crossfade_offset);
    AXK_READ(fine_tune_offset);
    AXK_READ(low_velocity_crossfade_offset);
    AXK_READ(coarse_tune_offset);
    AXK_READ(output1);
    AXK_READ(output2);
    AXK_READ(key_high);
    AXK_READ(key_low);
    AXK_READ(key_shift);
    AXK_READ(velocity_high);
    AXK_READ(velocity_low);
    AXK_READ(alternate_group);
    AXK_READ(amp_attack_offset);
    AXK_READ(amp_decay_offset);
    AXK_READ(amp_release_offset);
    AXK_READ(filter_cutoff_offset);
    AXK_READ(filter_gain_offset);
    AXK_READ(filter_q_offset);
    AXK_READ(filter_cutoff_distance_offset);
    AXK_READ(output1_level_offset);
    AXK_READ(output2_level_offset);
    AXK_READ(midi_control);
#undef AXK_READ
    if (value.contains("receive")) {
        auto parsed = receive(value["receive"]);
        if (!parsed)
            return std::unexpected{parsed.error()};
        result.receive = std::move(*parsed);
    }
#define AXK_SWITCH(member)                                                                                             \
    if (auto parsed = read_switch(value, #member, result.member); !parsed)                                             \
        return std::unexpected { parsed.error() }
    AXK_SWITCH(portamento);
    AXK_SWITCH(mono);
    AXK_SWITCH(key_crossfade);
#undef AXK_SWITCH
    return result;
}

nlohmann::json program_assignment_parameters_json(const ProgramAssignmentParameters &value) {
    auto result = Json::object();
#define AXK_WRITE(member) write(result, #member, value.member)
    AXK_WRITE(level_offset);
    AXK_WRITE(velocity_sensitivity_offset);
    AXK_WRITE(pan_offset);
    AXK_WRITE(high_velocity_crossfade_offset);
    AXK_WRITE(fine_tune_offset);
    AXK_WRITE(low_velocity_crossfade_offset);
    AXK_WRITE(coarse_tune_offset);
    AXK_WRITE(output1);
    AXK_WRITE(output2);
    AXK_WRITE(key_high);
    AXK_WRITE(key_low);
    AXK_WRITE(key_shift);
    AXK_WRITE(velocity_high);
    AXK_WRITE(velocity_low);
    AXK_WRITE(alternate_group);
    AXK_WRITE(amp_attack_offset);
    AXK_WRITE(amp_decay_offset);
    AXK_WRITE(amp_release_offset);
    AXK_WRITE(filter_cutoff_offset);
    AXK_WRITE(filter_gain_offset);
    AXK_WRITE(filter_q_offset);
    AXK_WRITE(filter_cutoff_distance_offset);
    AXK_WRITE(output1_level_offset);
    AXK_WRITE(output2_level_offset);
    AXK_WRITE(midi_control);
#undef AXK_WRITE
    if (value.receive)
        result["receive"] = receive_json(*value.receive);
    write_switch(result, "portamento", value.portamento);
    write_switch(result, "mono", value.mono);
    write_switch(result, "key_crossfade", value.key_crossfade);
    return result;
}

} // namespace axk::detail
