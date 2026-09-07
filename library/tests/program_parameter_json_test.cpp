#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <variant>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/program_parameter_json.hpp"
#include "axklib/program_spec_json.hpp"
#include "axklib/writer_internal.hpp"

namespace {
using Json = nlohmann::json;
}

TEST(ProgramParameterJson, RoundTripsSparseGlobalGroupsWithoutInventingDefaults) {
    const Json input = {
        {"level", 0},
        {"transpose", -127},
        {"controller_reset", {{"a", {{"1", false}, {"16", true}}}}},
        {"note_toggle", {{"b", {{"1", true}}}}},
        {"portamento", {{"type", 3}, {"rate", 1}, {"time", 127}}},
        {"lfo", {{"wave", 5}, {"reset_channel", -2}, {"tempo", 250}}},
        {"step_wave", {{"step_count", 12}, {"slope", "both"}, {"values", {{"1", 0}, {"16", 127}}}}},
        {"ad",
         {{"enabled", false},
          {"source", 2},
          {"left", {{"pan", -63}, {"output1", {{"destination", 1}, {"level", 64}}}}}}},
        {"controllers", {{"4", {{"device", 126}, {"function", 128}, {"type", 3}, {"range", -63}}}}},
        {"effect_connections", {{"1", 2}, {"2", 4}}},
        {"effects", {{"1", {{"type", 1}, {"enabled", false}, {"parameters", {{"2", 2345}}}}}}},
    };
    const auto parsed = axk::detail::parse_program_parameters_json(input);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(axk::detail::program_parameters_json(*parsed), input);
    EXPECT_EQ(parsed->effects[0].parameters[1], 2345);
    EXPECT_FALSE(parsed->controllers[0].device);
}

TEST(ProgramParameterJson, RoundTripsAssignmentOffsetsAndTypedReceiveSettings) {
    const Json input = {{"level_offset", 0},    {"pan_offset", 100},
                        {"key_low", 12},        {"receive", {{"port", "b"}, {"channel", 16}}},
                        {"output1", -1},        {"mono", "inherit"},
                        {"portamento", "off"},  {"key_crossfade", "on"},
                        {"midi_control", false}};
    const auto parsed = axk::detail::parse_program_assignment_parameters_json(input);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(axk::detail::program_assignment_parameters_json(*parsed), input);
    ASSERT_TRUE(parsed->receive);
    EXPECT_EQ(std::get<axk::ProgramReceiveChannel>(*parsed->receive).channel, 16);
    for (const auto *receive : {"inherit", "basic"}) {
        const Json value = {{"receive", receive}};
        const auto setting = axk::detail::parse_program_assignment_parameters_json(value);
        ASSERT_TRUE(setting);
        EXPECT_EQ(axk::detail::program_assignment_parameters_json(*setting), value);
    }
}

TEST(ProgramParameterJson, RejectsNullOverflowWrongTypesUnknownAndNoncanonicalIndices) {
    for (const auto &input :
         std::array<Json, 10>{Json{{"level", nullptr}}, Json{{"level", true}}, Json{{"level", 1.5}},
                              Json{{"level", std::numeric_limits<std::uint64_t>::max()}}, Json{{"transpose", 128}},
                              Json{{"lfo", {{"rate", 1}}}}, Json{{"controllers", {{"0", {{"device", 1}}}}}},
                              Json{{"effects", {{"01", {{"type", 1}}}}}}, Json{{"step_wave", {{"slope", 1}}}},
                              Json{{"effects", {{"1", {{"parameters", {{"17", 1}}}}}}}}}) {
        SCOPED_TRACE(input.dump());
        EXPECT_FALSE(axk::detail::parse_program_parameters_json(input));
    }
    for (const auto &input : std::array<Json, 6>{Json{{"receive_mode", "SAMPLE"}}, Json{{"receive", nullptr}},
                                                 Json{{"receive", {{"port", "b"}, {"channel", 0}}}},
                                                 Json{{"receive", {{"port", "x"}, {"channel", 1}}}}, Json{{"mono", -1}},
                                                 Json{{"runtime_handle", 0}}}) {
        SCOPED_TRACE(input.dump());
        EXPECT_FALSE(axk::detail::parse_program_assignment_parameters_json(input));
    }
}

TEST(ProgramParameterJson, NeverSerializesInvalidCppEnumsAsDifferentValidValues) {
    axk::ProgramAssignmentParameters value;
    value.mono = static_cast<axk::ProgramInheritableSwitch>(2);
    EXPECT_THROW(axk::detail::program_assignment_parameters_json(value), std::invalid_argument);
    value = {};
    value.receive = axk::ProgramReceiveChannel{static_cast<axk::MidiPort>(9), 1};
    EXPECT_THROW(axk::detail::program_assignment_parameters_json(value), std::invalid_argument);
    value.receive = axk::ProgramReceiveChannel{axk::MidiPort::a, 0};
    EXPECT_THROW(axk::detail::program_assignment_parameters_json(value), std::invalid_argument);
}

TEST(ProgramSpecJson, SharesFreshGlobalAndAssignmentParameters) {
    const Json input = {
        {"number", 33},
        {"name", "Params"},
        {"model", "A5000"},
        {"parameters", {{"level", 87}, {"effects", {{"1", {{"type", 1}, {"parameters", {{"2", 2345}}}}}}}}},
        {"assignments",
         Json::array(
             {{{"sample_bank", "Bank"}, {"parameters", {{"receive", "basic"}, {"pan_offset", 100}}}},
              {{"sample", "Direct"},
               {"parameters",
                {{"receive", {{"port", "b"}, {"channel", 16}}}, {"mono", "inherit"}, {"midi_control", false}}}}})}};
    const auto parsed = axk::detail::parse_program_spec_json(input);
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(axk::detail::program_spec_json(*parsed), input);
    const auto payload = axk::detail::prepare_prog_payload(*parsed);
    ASSERT_TRUE(payload) << payload.error().message;
    EXPECT_EQ((*payload)[0x8b], std::byte{87});
    EXPECT_EQ((*payload)[0x135], std::byte{0x10});
    EXPECT_EQ((*payload)[0x138], std::byte{100});
    EXPECT_EQ((*payload)[0x16d], std::byte{0x20});
    EXPECT_EQ((*payload)[0x18b], std::byte{});
}

TEST(ProgramSpecJson, DefaultsToA4000AndInheritedAssignmentReceive) {
    const Json input = {{"number", 1}, {"name", "Neutral"}, {"assignments", Json::array({{{"sample", "Direct"}}})}};
    const auto parsed = axk::detail::parse_program_spec_json(input);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->model, axk::ASeriesModel::a4000);
    EXPECT_FALSE(parsed->assignments[0].parameters.receive);
    const auto payload = axk::detail::prepare_prog_payload(*parsed);
    ASSERT_TRUE(payload);
    EXPECT_EQ((*payload)[0x135], std::byte{0xff});
    auto expected = input;
    expected["model"] = "A4000";
    EXPECT_EQ(axk::detail::program_spec_json(*parsed), expected);
}

TEST(ProgramSpecJson, RejectsObsoleteAndMalformedContractsWithoutNarrowing) {
    const Json base = {{"number", 1}, {"name", "Neutral"}, {"assignments", Json::array({{{"sample", "Direct"}}})}};
    for (const auto &change :
         std::array<Json, 8>{Json{{"number", std::numeric_limits<std::uint64_t>::max()}}, Json{{"number", true}},
                             Json{{"number", 1.5}}, Json{{"model", "A3000"}}, Json{{"model", nullptr}},
                             Json{{"parameters", nullptr}}, Json{{"assignments", nullptr}}, Json{{"unknown", 1}}}) {
        auto input = base;
        input.update(change);
        EXPECT_FALSE(axk::detail::parse_program_spec_json(input)) << input.dump();
    }
    for (const auto &assignment : std::array<Json, 6>{
             Json{{"sample", "Direct"}, {"receive_mode", "SAMPLE"}}, Json{{"sample", "Direct"}, {"receive_channel", 1}},
             Json{{"sample", "Direct"}, {"sample_bank", "Bank"}}, Json{{"parameters", Json::object()}},
             Json{{"sample", "Direct"}, {"parameters", nullptr}}, Json{{"sample", " Direct"}}}) {
        auto input = base;
        input["assignments"][0] = assignment;
        EXPECT_FALSE(axk::detail::parse_program_spec_json(input)) << input.dump();
    }
}

TEST(ProgramSpecJson, BuildManifestUsesTheSharedParserWithoutLosingParameters) {
    const auto program = Json::parse(R"({
        "number":33,"name":"Params","parameters":{"level":87},
        "assignments":[
            {"sample_bank":"Bank","parameters":{"receive":{"port":"a","channel":1}}},
            {"sample":"Direct","parameters":{"receive":{"port":"a","channel":2},"pan_offset":-127}}
        ]
    })");
    Json volume = {
        {"name", "Programs"},
        {"waveforms", Json::array({{{"id", "wave"}, {"name", "Wave"}, {"path", "wave.wav"}, {"root_key", 60}}})},
        {"samples",
         Json::array({{{"name", "Member"}, {"waveform_id", "wave"}}, {{"name", "Direct"}, {"waveform_id", "wave"}}})},
        {"sample_banks", Json::array({{{"name", "Bank"}, {"member_samples", {"Member"}}}})},
        {"programs", Json::array({program})}};
    const Json manifest = {{"schema_version", "1.0"},
                           {"size_bytes", 4 * 1024 * 1024},
                           {"partitions", Json::array({{{"name", "Partition"}, {"volumes", Json::array({volume})}}})}};
    const auto parsed = axk::parse_hds_build_manifest(manifest.dump());
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto &authored = parsed->partitions[0].volumes[0].programs[0];
    auto expected = program;
    expected["model"] = "A4000";
    EXPECT_EQ(axk::detail::program_spec_json(authored), expected);
}
