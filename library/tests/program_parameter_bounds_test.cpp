#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/bytes.hpp"
#include "axklib/program_parameter_codec.hpp"
#include "axklib/program_parameter_json.hpp"

namespace {

using Json = nlohmann::json;
constexpr std::size_t tail = 0x2e0;
constexpr std::size_t row_start = 0x120;

std::vector<std::byte> payload() {
    std::vector<std::byte> bytes(0x390 + 64, std::byte{0xa5});
    axk::ByteWriter writer{bytes};
    EXPECT_TRUE(writer.write_ascii_field(0, 16, "FSFSDEV3SPLXPROG"));
    EXPECT_TRUE(writer.write_be32(0x14, 4));
    EXPECT_TRUE(writer.write_be32(0x18, 0x2b0));
    EXPECT_TRUE(writer.write_be32(0x1c, 0x360));
    EXPECT_TRUE(writer.write_be16(0x96, 1));
    EXPECT_TRUE(writer.write_ascii_field(row_start, 16, "Target"));
    bytes[row_start + 0x14] = std::byte{0x10};
    for (const auto offset : {0x1eU, 0x21U})
        bytes[row_start + offset] = std::byte{127};
    for (const auto offset : {0x1fU, 0x22U})
        bytes[row_start + offset] = std::byte{};
    for (const auto offset : {0x1dU, 0x28U, 0x2dU, 0x30U})
        bytes[row_start + offset] = std::byte{0xff};
    return bytes;
}

struct Field {
    std::string path;
    std::size_t offset;
    int minimum;
    int maximum;
    unsigned mask{0xff};
    unsigned shift{};
};

Json patch(const std::string &path, Json value) {
    auto result = Json::object();
    const Json::json_pointer leaf{path};
    std::vector<Json::json_pointer> parents;
    for (auto parent = leaf.parent_pointer(); !parent.empty(); parent = parent.parent_pointer())
        parents.push_back(parent);
    for (auto parent = parents.rbegin(); parent != parents.rend(); ++parent)
        result[*parent] = Json::object();
    result[leaf] = std::move(value);
    return result;
}

void expected_byte(std::vector<std::byte> &bytes, const Field &field, int value) {
    const auto original = std::to_integer<unsigned>(bytes[field.offset]);
    const auto raw = static_cast<unsigned>(static_cast<std::uint8_t>(value));
    bytes[field.offset] = static_cast<std::byte>((original & ~field.mask) | ((raw << field.shift) & field.mask));
}

std::vector<Field> global_fields(axk::ASeriesModel model) {
    const auto extended = model == axk::ASeriesModel::a5000;
    std::vector<Field> fields{
        {"/level", 0x8b, 0, 127},
        {"/transpose", 0x8e, -127, 127},
        {"/portamento/type", 0x90, 0, 3},
        {"/portamento/rate", 0x91, 1, 127},
        {"/portamento/time", 0x92, 1, 127},
        {"/lfo/cycle", 0x81, 0, 6, 7, 0},
        {"/lfo/wave", 0x81, 0, 6, 0x38, 3},
        {"/lfo/initial_phase", 0x81, 0, 3, 0xc0, 6},
        {"/lfo/sync", 0x80, 0, extended ? 2 : 1, 0xc0, 6},
        {"/lfo/reset_channel", 0x8f, -2, extended ? 32 : 16},
        {"/lfo/reset_note", 0x95, -1, 127},
        {"/lfo/tempo", 0x94, 25, 250},
        {"/lfo/sample_hold_speed", 0x93, 0, 127},
        {"/ad/source", 0x80, 0, 2, 6, 1},
        {"/ad/left/pan", 0x86, -63, 63},
        {"/ad/right/pan", tail + 0x91, -63, 63},
        {"/effect_connections/1", 0x80, 0, 4, 0x38, 3},
    };
    if (extended)
        fields.push_back({"/effect_connections/2", tail + 0x8c, 0, 4, 7, 0});
    for (std::size_t index = 0; index < 16; ++index)
        fields.push_back({"/step_wave/values/" + std::to_string(index + 1), tail + 0x96 + index, 0, 127});
    for (std::size_t index = 0; index < 4; ++index) {
        const auto path = "/controllers/" + std::to_string(index + 1) + "/";
        const auto offset = tail + 0x78 + index * 4;
        fields.push_back({path + "device", offset, 0, 126});
        fields.push_back({path + "function", offset + 1, 0, extended ? 128 : 71});
        fields.push_back({path + "type", offset + 2, 0, 3});
        fields.push_back({path + "range", offset + 3, -63, 63});
    }
    for (std::size_t index = 0; index < (extended ? 6U : 3U); ++index) {
        const auto path = "/effects/" + std::to_string(index + 1) + "/";
        const auto offset = index < 3U ? 0x98 + index * 0x28 : tail + (index - 3) * 0x28;
        fields.push_back({path + "input_level", offset + 1, 0, 127});
        fields.push_back({path + "output_level", offset + 2, 0, 127});
        fields.push_back({path + "pan", offset + 3, -63, 63});
        fields.push_back({path + "destination", offset + 4, 0, extended && index < 3U ? 8 : 5});
        fields.push_back({path + "width", offset + 5, -126, 0});
    }
    return fields;
}

void expect_global_bytes(const Field &field, int value, axk::ASeriesModel model) {
    auto bytes = payload();
    auto expected = bytes;
    expected_byte(expected, field, value);
    if (field.path.starts_with("/controllers/")) {
        const auto index = (field.offset - tail - 0x78) / 4;
        std::copy_n(expected.begin() + static_cast<std::ptrdiff_t>(tail + 0x78 + index * 4), 4,
                    expected.begin() + static_cast<std::ptrdiff_t>(0x110 + index * 4));
        if (std::to_integer<unsigned>(expected[0x111 + index * 4]) > 63U)
            expected[0x111 + index * 4] = std::byte{};
    }
    const auto parsed = axk::detail::parse_program_parameters_json(patch(field.path, value));
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_TRUE(axk::detail::apply_program_parameters(bytes, *parsed, model));
    EXPECT_EQ(bytes, expected);
    ASSERT_TRUE(axk::detail::apply_program_parameters(bytes, *parsed, model));
    EXPECT_EQ(bytes, expected);
}

} // namespace

TEST(ProgramParameterBounds, EveryGlobalNumericLeafAndIndexHasIndependentByteAndDomainChecks) {
    for (const auto model : {axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        for (const auto &field : global_fields(model)) {
            SCOPED_TRACE(field.path);
            for (const auto value : {field.minimum, field.maximum, (field.minimum + field.maximum) / 2}) {
                SCOPED_TRACE(value);
                expect_global_bytes(field, value, model);
            }
            for (const auto value : {field.minimum - 1, field.maximum + 1}) {
                auto bytes = payload();
                const auto original = bytes;
                const auto parsed = axk::detail::parse_program_parameters_json(patch(field.path, value));
                if (parsed) {
                    EXPECT_FALSE(axk::detail::apply_program_parameters(bytes, *parsed, model));
                }
                EXPECT_EQ(bytes, original);
            }
        }
    }
}

TEST(ProgramParameterBounds, EveryGlobalBooleanAndChannelMaskIsIndependent) {
    std::vector<Field> fields{{"/ad/enabled", 0x80, 0, 1, 1, 0}};
    for (std::size_t slot = 0; slot < 6; ++slot)
        fields.push_back({"/effects/" + std::to_string(slot + 1) + "/enabled",
                          slot < 3 ? 0x98 + slot * 0x28 : tail + (slot - 3) * 0x28, 0, 1});
    for (const auto &[path, offset] :
         std::array<std::pair<const char *, std::size_t>, 4>{{{"/controller_reset/a/", 0x82},
                                                              {"/note_toggle/a/", 0x84},
                                                              {"/controller_reset/b/", tail + 0x88},
                                                              {"/note_toggle/b/", tail + 0x8a}}}) {
        for (unsigned bit = 0; bit < 16; ++bit)
            fields.push_back({std::string{path} + std::to_string(bit + 1), offset + (bit < 8 ? 1U : 0U), 0, 1,
                              1U << (bit % 8), bit % 8});
    }
    for (const auto &field : fields) {
        SCOPED_TRACE(field.path);
        for (const auto value : {false, true}) {
            auto bytes = payload();
            auto expected = bytes;
            expected_byte(expected, field, value ? 1 : 0);
            const auto parsed = axk::detail::parse_program_parameters_json(patch(field.path, value));
            ASSERT_TRUE(parsed);
            ASSERT_TRUE(axk::detail::apply_program_parameters(bytes, *parsed, axk::ASeriesModel::a5000));
            EXPECT_EQ(bytes, expected);
        }
        EXPECT_FALSE(axk::detail::parse_program_parameters_json(patch(field.path, 1)));
        EXPECT_FALSE(axk::detail::parse_program_parameters_json(patch(field.path, nullptr)));
    }
}

TEST(ProgramParameterBounds, EveryStepCountAndSlopePreservesUnrelatedPackedLanes) {
    constexpr std::array counts{2, 3, 4, 6, 8, 12, 16};
    constexpr std::array slopes{"none", "rising", "falling", "both"};
    for (std::size_t count = 0; count < counts.size(); ++count) {
        for (std::size_t slope = 0; slope < slopes.size(); ++slope) {
            auto bytes = payload();
            auto expected = bytes;
            expected[tail + 0xa6] = static_cast<std::byte>(0xa0U | count | (slope << 3U));
            const auto parsed = axk::detail::parse_program_parameters_json(
                Json{{"step_wave", {{"step_count", counts[count]}, {"slope", slopes[slope]}}}});
            ASSERT_TRUE(parsed);
            ASSERT_TRUE(axk::detail::apply_program_parameters(bytes, *parsed, axk::ASeriesModel::a4000));
            EXPECT_EQ(bytes, expected);
        }
    }
    for (const auto slope : {-1, 4, 255}) {
        auto bytes = payload();
        const auto original = bytes;
        axk::ProgramParameters parameters;
        parameters.step_wave.slope = static_cast<axk::ProgramStepWaveSlope>(slope);
        EXPECT_FALSE(axk::detail::apply_program_parameters(bytes, parameters, axk::ASeriesModel::a4000));
        EXPECT_EQ(bytes, original);
    }
}

TEST(ProgramParameterBounds, EveryEasyEditScalarHasIndependentByteAndDomainChecks) {
    const auto fields = std::to_array<Field>({
        {"/level_offset", 0x16, -127, 127},
        {"/velocity_sensitivity_offset", 0x17, -127, 127},
        {"/pan_offset", 0x18, -127, 127},
        {"/high_velocity_crossfade_offset", 0x19, -127, 127},
        {"/fine_tune_offset", 0x1a, -127, 127},
        {"/low_velocity_crossfade_offset", 0x1b, -127, 127},
        {"/coarse_tune_offset", 0x1c, -127, 127},
        {"/key_high", 0x1e, 0, 127},
        {"/key_low", 0x1f, 0, 127},
        {"/key_shift", 0x20, -127, 127},
        {"/velocity_high", 0x21, 0, 127},
        {"/velocity_low", 0x22, 0, 127},
        {"/alternate_group", 0x24, -1, 16},
        {"/amp_attack_offset", 0x25, -127, 127},
        {"/amp_decay_offset", 0x26, -127, 127},
        {"/amp_release_offset", 0x27, -127, 127},
        {"/filter_cutoff_offset", 0x29, -127, 127},
        {"/filter_gain_offset", 0x2a, -63, 63},
        {"/filter_q_offset", 0x2b, -31, 31},
        {"/filter_cutoff_distance_offset", 0x2c, -127, 127},
        {"/output1_level_offset", 0x2f, -127, 127},
        {"/output2_level_offset", 0x32, -127, 127},
    });
    for (auto field : fields) {
        SCOPED_TRACE(field.path);
        field.offset += row_start;
        for (const auto value : {field.minimum - 1, field.minimum, (field.minimum + field.maximum) / 2, field.maximum,
                                 field.maximum + 1}) {
            SCOPED_TRACE(value);
            auto bytes = payload();
            auto expected = bytes;
            const auto parsed = axk::detail::parse_program_assignment_parameters_json(patch(field.path, value));
            const auto valid = value >= field.minimum && value <= field.maximum;
            if (valid)
                expected_byte(expected, field, value);
            if (parsed) {
                const std::array patches{axk::ProgramAssignmentParameterPatch{0, "SBNK", "Target", *parsed}};
                EXPECT_EQ(
                    axk::detail::apply_program_assignment_patches(bytes, patches, axk::ASeriesModel::a4000).has_value(),
                    valid);
            } else {
                EXPECT_FALSE(valid);
            }
            EXPECT_EQ(bytes, expected);
        }
    }
}

TEST(ProgramParameterBounds, EveryAdOutputAndLevelHonorsItsModelAndLegacyBucket) {
    constexpr std::array<std::pair<std::size_t, unsigned>, 13> output1{{{0, 0},
                                                                        {0x87, 1},
                                                                        {0x87, 2},
                                                                        {0x87, 3},
                                                                        {0x87, 4},
                                                                        {0x89, 1},
                                                                        {0x89, 2},
                                                                        {0x89, 3},
                                                                        {0x89, 4},
                                                                        {0x89, 5},
                                                                        {0, 0},
                                                                        {0, 0},
                                                                        {0, 0}}};
    constexpr std::array<std::pair<std::size_t, unsigned>, 13> output2{{{0, 0},
                                                                        {0x89, 1},
                                                                        {0x89, 2},
                                                                        {0x89, 3},
                                                                        {0x89, 4},
                                                                        {0x89, 5},
                                                                        {0x87, 1},
                                                                        {0x87, 2},
                                                                        {0x87, 3},
                                                                        {0x87, 4},
                                                                        {0, 0},
                                                                        {0, 0},
                                                                        {0, 0}}};
    for (const auto model : {axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        for (const auto &side : {std::string{"left"}, std::string{"right"}}) {
            for (std::size_t member = 0; member < 4; ++member) {
                const auto destination = member % 2 == 0;
                const auto maximum = destination ? (model == axk::ASeriesModel::a5000 ? 12 : 9) : 127;
                const Field field{"/ad/" + side + "/output" + std::to_string(1 + member / 2) +
                                      (destination ? "/destination" : "/level"),
                                  tail + (side == "left" ? 0x8d : 0x92) + member, 0, maximum};
                SCOPED_TRACE(field.path);
                for (const auto value : {-1, 0, maximum / 2, maximum, maximum + 1}) {
                    auto bytes = payload();
                    auto expected = bytes;
                    const auto valid = value >= 0 && value <= maximum;
                    if (valid) {
                        expected_byte(expected, field, value);
                        if (side == "left") {
                            expected[0x87] = expected[0x89] = std::byte{};
                            for (const auto &[source, buckets] :
                                 std::array{std::pair{tail + 0x8f, std::span{output2}},
                                            std::pair{tail + 0x8d, std::span{output1}}}) {
                                const auto target = std::to_integer<unsigned>(expected[source]);
                                if (target < buckets.size() && buckets[target].first != 0U) {
                                    const auto [offset, raw] = buckets[target];
                                    expected[offset] = static_cast<std::byte>(raw);
                                    expected[offset + 1] = expected[source + 1];
                                }
                            }
                        }
                    }
                    const auto parsed = axk::detail::parse_program_parameters_json(patch(field.path, value));
                    if (parsed)
                        EXPECT_EQ(axk::detail::apply_program_parameters(bytes, *parsed, model).has_value(), valid);
                    else
                        EXPECT_FALSE(valid);
                    EXPECT_EQ(bytes, expected);
                }
            }
        }
    }
}

TEST(ProgramParameterBounds, AllAssignmentReplacementLanesAndMidiControlPreserveOtherBits) {
    for (const auto &[name, shift] :
         std::array<std::pair<const char *, unsigned>, 3>{{{"portamento", 0}, {"mono", 2}, {"key_crossfade", 4}}}) {
        for (const auto &[setting, raw] :
             std::array<std::pair<const char *, unsigned>, 3>{{{"inherit", 3}, {"off", 0}, {"on", 1}}}) {
            auto bytes = payload();
            auto expected = bytes;
            expected[row_start + 0x23] = static_cast<std::byte>((0xa5U & ~(3U << shift)) | (raw << shift));
            const auto parsed = axk::detail::parse_program_assignment_parameters_json(Json{{name, setting}});
            ASSERT_TRUE(parsed);
            const std::array patches{axk::ProgramAssignmentParameterPatch{0, "SBNK", "Target", *parsed}};
            ASSERT_TRUE(axk::detail::apply_program_assignment_patches(bytes, patches, axk::ASeriesModel::a4000));
            EXPECT_EQ(bytes, expected);
        }
        for (const auto &value : std::array<Json, 4>{Json(0), Json(false), Json(nullptr), Json("unknown")})
            EXPECT_FALSE(axk::detail::parse_program_assignment_parameters_json(Json{{name, value}}));
    }
    for (const auto value : {false, true}) {
        auto bytes = payload();
        auto expected = bytes;
        expected[row_start + 0x33] = static_cast<std::byte>(value);
        axk::ProgramAssignmentParameters parameters;
        parameters.midi_control = value;
        const std::array patches{axk::ProgramAssignmentParameterPatch{0, "SBNK", "Target", parameters}};
        ASSERT_TRUE(axk::detail::apply_program_assignment_patches(bytes, patches, axk::ASeriesModel::a4000));
        EXPECT_EQ(bytes, expected);
    }
}

TEST(ProgramParameterBounds, AssignmentOutputsCoverEveryDestinationAndRejectModelOverflow) {
    constexpr std::array<std::pair<std::size_t, unsigned>, 13> output1{{{0, 0},
                                                                        {0x2d, 1},
                                                                        {0x2d, 2},
                                                                        {0x2d, 3},
                                                                        {0x2d, 4},
                                                                        {0x30, 1},
                                                                        {0x30, 2},
                                                                        {0x30, 3},
                                                                        {0x30, 4},
                                                                        {0x30, 5},
                                                                        {0, 0},
                                                                        {0, 0},
                                                                        {0, 0}}};
    constexpr std::array<std::pair<std::size_t, unsigned>, 13> output2{{{0, 0},
                                                                        {0x30, 1},
                                                                        {0x30, 2},
                                                                        {0x30, 3},
                                                                        {0x30, 4},
                                                                        {0x30, 5},
                                                                        {0x2d, 1},
                                                                        {0x2d, 2},
                                                                        {0x2d, 3},
                                                                        {0x2d, 4},
                                                                        {0, 0},
                                                                        {0, 0},
                                                                        {0, 0}}};
    for (const auto model : {axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        const auto maximum = model == axk::ASeriesModel::a5000 ? 12 : 9;
        for (const auto output : {1U, 2U}) {
            const auto &buckets = output == 1U ? output1 : output2;
            for (int value = -2; value <= maximum + 1; ++value) {
                auto bytes = payload();
                auto expected = bytes;
                const auto valid = value >= -1 && value <= maximum;
                if (valid) {
                    expected[row_start + (output == 1U ? 0x1d : 0x28)] =
                        static_cast<std::byte>(static_cast<std::uint8_t>(value));
                    if (value >= 0) {
                        const auto [offset, raw] = buckets[static_cast<std::size_t>(value)];
                        if (offset != 0U)
                            expected[row_start + offset] = static_cast<std::byte>(raw);
                    }
                }
                const auto parsed = axk::detail::parse_program_assignment_parameters_json(
                    Json{{"output" + std::to_string(output), value}});
                ASSERT_TRUE(parsed);
                const std::array patches{axk::ProgramAssignmentParameterPatch{0, "SBNK", "Target", *parsed}};
                EXPECT_EQ(axk::detail::apply_program_assignment_patches(bytes, patches, model).has_value(), valid);
                EXPECT_EQ(bytes, expected);
            }
        }
    }
}

TEST(ProgramParameterBounds, EveryReceiveSettingHasOneEncodingAndInvalidCppChoicesReject) {
    std::vector<std::pair<axk::ProgramReceiveSetting, unsigned>> settings{{axk::ProgramReceiveInherit{}, 0xff},
                                                                          {axk::ProgramReceiveBasic{}, 0x10}};
    for (std::uint8_t channel = 1; channel <= 16; ++channel) {
        settings.emplace_back(axk::ProgramReceiveChannel{axk::MidiPort::a, channel}, channel - 1U);
        settings.emplace_back(axk::ProgramReceiveChannel{axk::MidiPort::b, channel}, channel + 16U);
    }
    for (const auto &[setting, raw] : settings) {
        auto bytes = payload();
        auto expected = bytes;
        expected[row_start + 0x15] = static_cast<std::byte>(raw);
        axk::ProgramAssignmentParameters parameters;
        parameters.receive = setting;
        const std::array patches{axk::ProgramAssignmentParameterPatch{0, "SBNK", "Target", parameters}};
        ASSERT_TRUE(axk::detail::apply_program_assignment_patches(bytes, patches, axk::ASeriesModel::a5000));
        EXPECT_EQ(bytes, expected);
    }
    for (const auto port : {2, 255}) {
        auto bytes = payload();
        const auto original = bytes;
        axk::ProgramAssignmentParameters parameters;
        parameters.receive = axk::ProgramReceiveChannel{static_cast<axk::MidiPort>(port), 1};
        const std::array patches{axk::ProgramAssignmentParameterPatch{0, "SBNK", "Target", parameters}};
        EXPECT_FALSE(axk::detail::apply_program_assignment_patches(bytes, patches, axk::ASeriesModel::a5000));
        EXPECT_EQ(bytes, original);
    }
    for (const auto value : {-2, 2}) {
        for (const auto member :
             {&axk::ProgramAssignmentParameters::portamento, &axk::ProgramAssignmentParameters::mono,
              &axk::ProgramAssignmentParameters::key_crossfade}) {
            auto bytes = payload();
            const auto original = bytes;
            axk::ProgramAssignmentParameters parameters;
            parameters.*member = static_cast<axk::ProgramInheritableSwitch>(value);
            const std::array patches{axk::ProgramAssignmentParameterPatch{0, "SBNK", "Target", parameters}};
            EXPECT_FALSE(axk::detail::apply_program_assignment_patches(bytes, patches, axk::ASeriesModel::a5000));
            EXPECT_EQ(bytes, original);
        }
    }
}
