#include "axklib/application/program_editor.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

#include "axklib/effects.hpp"

namespace axk::app {
namespace {
using Json = nlohmann::json;

Json effect_catalog(EffectProfile profile) {
    auto result = Json::array();
    const auto count = profile == EffectProfile::a3000 ? 55U : 97U;
    for (unsigned type = 0; type < count; ++type) {
        const auto id = static_cast<std::uint16_t>(type);
        const auto info = *effect_type_info(id, profile);
        const auto write = *effect_write_info(id, profile);
        auto parameters = Json::array();
        for (std::size_t word = 0; word < 16U; ++word) {
            const auto parameter = effect_parameter_info(id, static_cast<std::uint8_t>(word + 1U), profile);
            const auto &domain = write.parameters[word];
            parameters.push_back({{"index", word},
                                  {"label", parameter ? std::string(parameter->parameter_label) : ""},
                                  {"min", domain.minimum},
                                  {"max", domain.maximum},
                                  {"editable", domain.kind == EffectParameterKind::stored_value}});
        }
        result.push_back({{"id", id},
                          {"label", info.ui_label},
                          {"printedNumber", info.printed_number},
                          {"resetWords", write.reset_words},
                          {"parameters", std::move(parameters)}});
    }
    return result;
}
} // namespace

Json program_editor_catalog() {
    auto formats = Json::array();
    for (const auto profile : {EffectProfile::a3000, EffectProfile::a5000}) {
        const bool native = profile == EffectProfile::a3000;
        auto fields = Json::array();
        const auto field = [&](const std::string &key, int minimum, int maximum) {
            fields.push_back({{"key", key}, {"min", minimum}, {"max", maximum}});
        };
        field("level", 0, 127);
        field("transpose", -127, 127);
        field("portamento.type", 0, 3);
        field("portamento.rate", 1, 127);
        field("portamento.time", 1, 127);
        field("lfo.cycle", 0, 6);
        field("lfo.sync", 0, native ? 1 : 2);
        field("lfo.wave", 0, native ? 5 : 6);
        field("lfo.initial_phase", 0, 3);
        field("lfo.tempo", 25, 250);
        field("lfo.reset_channel", -2, native ? 16 : 32);
        field("lfo.reset_note", -1, 127);
        field("lfo.sample_hold_speed", 0, 127);
        if (!native) {
            field("step_wave.step_count", 2, 16);
            fields.back()["allowedValues"] = {2, 3, 4, 6, 8, 12, 16};
            field("step_wave.slope", 0, 3);
            for (unsigned index = 1; index <= 16U; ++index)
                field("step_wave.values." + std::to_string(index), 0, 127);
        }
        for (const auto *port : {"a", "b"}) {
            if (native && std::string(port) == "b")
                continue;
            for (unsigned channel = 1; channel <= 16U; ++channel) {
                for (const auto *group : {"controller_reset", "note_toggle"}) {
                    field(std::string(group) + "." + port + "." + std::to_string(channel), 0, 1);
                    fields.back()["boolean"] = true;
                }
            }
        }
        field("ad.enabled", 0, 1);
        fields.back()["boolean"] = true;
        field("ad.source", 0, 2);
        for (const auto *channel : {"left", "right"}) {
            if (native && std::string(channel) == "right")
                continue;
            const auto prefix = std::string("ad.") + channel;
            field(prefix + ".pan", -63, 63);
            for (unsigned output = 1; output <= 2U; ++output) {
                const auto path = prefix + ".output" + std::to_string(output);
                field(path + ".destination", 0, native ? (output == 1U ? 4 : 5) : 12);
                field(path + ".level", 0, 127);
            }
        }
        for (unsigned index = 1; index <= 4U; ++index) {
            const auto prefix = "controllers." + std::to_string(index);
            field(prefix + ".device", 0, native ? 125 : 126);
            field(prefix + ".function", 0, native ? 63 : 128);
            field(prefix + ".type", 0, 3);
            field(prefix + ".range", -63, 63);
        }
        for (unsigned group = 1; group <= (native ? 1U : 2U); ++group)
            field("effect_connections." + std::to_string(group), 0, 4);
        for (unsigned index = 1; index <= (native ? 3U : 6U); ++index) {
            const auto prefix = "effects." + std::to_string(index);
            field(prefix + ".enabled", 0, 1);
            fields.back()["boolean"] = true;
            field(prefix + ".input_level", 0, 127);
            field(prefix + ".output_level", 0, 127);
            field(prefix + ".pan", -63, 63);
            field(prefix + ".width", -126, 0);
            field(prefix + ".destination", 0, !native && index <= 3U ? 8 : 5);
            field(prefix + ".type", 0, native ? 54 : 96);
        }
        for (const auto *name : {"level_offset", "pan_offset", "fine_tune_offset", "coarse_tune_offset", "key_shift",
                                 "amp_attack_offset", "amp_decay_offset", "amp_release_offset", "filter_cutoff_offset",
                                 "filter_cutoff_distance_offset", "output1_level_offset", "output2_level_offset"})
            field(std::string("assignments.*.") + name, -127, 127);
        if (!native)
            for (const auto *name :
                 {"velocity_sensitivity_offset", "high_velocity_crossfade_offset", "low_velocity_crossfade_offset"})
                field(std::string("assignments.*.") + name, -127, 127);
        field("assignments.*.filter_gain_offset", -63, 63);
        field("assignments.*.filter_q_offset", -31, 31);
        field("assignments.*.alternate_group", -1, 16);
        field("assignments.*.output1", -1, native ? 4 : 12);
        field("assignments.*.output2", -1, native ? 5 : 12);
        field("assignments.*.receive", -1, native ? 16 : 32);
        for (const auto *name : {"key_low", "key_high", "velocity_low", "velocity_high"})
            field(std::string("assignments.*.") + name, 0, 127);
        for (const auto *name : {"portamento", "mono", "key_crossfade", "velocity_crossfade"}) {
            if (!native && std::string(name) == "velocity_crossfade")
                continue;
            field(std::string("assignments.*.") + name, -1, 1);
        }
        field("assignments.*.midi_control", 0, 1);
        fields.back()["boolean"] = true;
        for (auto &entry : fields) {
            const auto key = entry.at("key").get<std::string>();
            if (!key.starts_with("assignments.*."))
                continue;
            entry["defaultValue"] = key.ends_with("midi_control")                                 ? Json(true)
                                    : key.ends_with("key_high") || key.ends_with("velocity_high") ? Json(127)
                                    : entry.at("min") == -1                                       ? Json(-1)
                                                                                                  : Json(0);
        }
        formats.push_back({{"model", native ? "A3000" : "A5000"},
                           {"fields", std::move(fields)},
                           {"effects", effect_catalog(profile)}});
    }
    return {{"schemaVersion", 1}, {"formats", std::move(formats)}};
}
} // namespace axk::app
