#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/object.hpp"
#include "axklib/program_parameter_codec.hpp"
#include "axklib/program_parameters.hpp"
#include "axklib/writer_internal.hpp"

namespace {

std::vector<std::byte> program_payload(std::uint32_t version = 4U) {
    const auto size = version == 4U ? 0x390U : 0x2e0U;
    std::vector<std::byte> payload(size, std::byte{0xa5});
    axk::ByteWriter writer{payload};
    EXPECT_TRUE(writer.write_ascii_field(0, 16, "FSFSDEV3SPLXPROG", std::byte{}));
    EXPECT_TRUE(writer.write_be32(0x14, version));
    EXPECT_TRUE(writer.write_be32(0x18, size - (version == 4U ? 0xe0U : 0x30U)));
    EXPECT_TRUE(writer.write_be32(0x1c, version == 4U ? size - 0x30U : 0U));
    EXPECT_TRUE(writer.write_ascii_field(0x32, 16, "001", std::byte{}));
    EXPECT_TRUE(writer.write_ascii_field(0x78, 8, "TEST", std::byte{}));
    EXPECT_TRUE(writer.write_be16(0x96, 0));
    return payload;
}

} // namespace

TEST(ProgramParameters, ChangesOnlyRequestedCommonBytes) {
    auto payload = program_payload();
    auto expected = payload;
    axk::ProgramParameters patch;
    patch.level = 0;
    patch.transpose = -127;
    patch.portamento.type = 3;
    patch.portamento.rate = 1;
    patch.portamento.time = 127;
    expected[0x8b] = std::byte{0};
    expected[0x8e] = std::byte{0x81};
    expected[0x90] = std::byte{3};
    expected[0x91] = std::byte{1};
    expected[0x92] = std::byte{127};
    ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, expected);
    ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, expected);
}

TEST(ProgramParameters, EmptyPatchPreservesEveryByteIncludingInvalidImportedValues) {
    auto payload = program_payload();
    const auto original = payload;
    ASSERT_TRUE(axk::detail::apply_program_parameters(payload, {}, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, original);
}

TEST(ProgramParameters, PacksLfoAndStepWaveWithoutOverwritingOtherLanes) {
    auto payload = program_payload();
    auto expected = payload;
    axk::ProgramParameters patch;
    patch.lfo.cycle = 6;
    patch.lfo.wave = 3;
    patch.lfo.initial_phase = 2;
    patch.lfo.sync = 1;
    patch.lfo.tempo = 250;
    patch.lfo.reset_channel = -2;
    patch.lfo.reset_note = -1;
    patch.lfo.sample_hold_speed = 127;
    patch.step_wave.step_count = 12;
    patch.step_wave.slope = axk::ProgramStepWaveSlope::both;
    patch.step_wave.values[0] = 0;
    patch.step_wave.values[15] = 127;
    expected[0x80] = std::byte{0x65};
    expected[0x81] = std::byte{0x9e};
    expected[0x8f] = std::byte{0xfe};
    expected[0x93] = std::byte{127};
    expected[0x94] = std::byte{250};
    expected[0x95] = std::byte{0xff};
    expected[0x376] = std::byte{0};
    expected[0x385] = std::byte{127};
    expected[0x386] = std::byte{0xbd};
    ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, expected);
}

TEST(ProgramParameters, ChannelMapPatchesPreserveOtherChannelsAndSeparatePorts) {
    auto payload = program_payload();
    auto expected = payload;
    axk::ProgramParameters patch;
    patch.controller_reset.a[0] = false;
    patch.controller_reset.a[15] = true;
    patch.note_toggle.b[0] = false;
    patch.note_toggle.b[15] = false;
    expected[0x83] = std::byte{0xa4};
    expected[0x84] = payload[0x84];
    expected[0x36a] = std::byte{0x25};
    expected[0x36b] = std::byte{0xa4};
    ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a5000));
    EXPECT_EQ(payload, expected);
}

TEST(ProgramParameters, RejectsInvalidMergedRequestBeforeChangingAnyBytes) {
    for (const auto invalid : std::array<std::uint8_t, 3>{0, 128, 255}) {
        auto payload = program_payload();
        const auto original = payload;
        axk::ProgramParameters patch;
        patch.level = 0;
        patch.portamento.rate = invalid;
        EXPECT_FALSE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
        EXPECT_EQ(payload, original);
    }
    for (const auto count : std::array<std::uint8_t, 4>{0, 1, 5, 17}) {
        auto payload = program_payload();
        const auto original = payload;
        axk::ProgramParameters patch;
        patch.step_wave.step_count = count;
        EXPECT_FALSE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
        EXPECT_EQ(payload, original);
    }
}

TEST(ProgramParameters, RequiresCurrentLayoutAndExplicitSupportedModel) {
    axk::ProgramParameters patch;
    patch.level = 64;
    for (const auto version : {1U, 2U}) {
        auto payload = program_payload(version);
        const auto original = payload;
        EXPECT_FALSE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
        EXPECT_EQ(payload, original);
    }
    for (const auto model : {axk::ASeriesModel::a3000, static_cast<axk::ASeriesModel>(255)}) {
        auto payload = program_payload();
        const auto original = payload;
        EXPECT_FALSE(axk::detail::apply_program_parameters(payload, patch, model));
        EXPECT_EQ(payload, original);
    }
}

TEST(ProgramParameters, RejectsRequestedModelOnlyValuesWithoutRejectingUntouchedOnes) {
    auto payload = program_payload();
    payload[0x80] = std::byte{0x80};
    payload[0x8f] = std::byte{32};
    axk::ProgramParameters unrelated;
    unrelated.level = 12;
    ASSERT_TRUE(axk::detail::apply_program_parameters(payload, unrelated, axk::ASeriesModel::a4000));
    const auto original = payload;
    axk::ProgramParameters patch;
    patch.lfo.sync = 2;
    patch.lfo.reset_channel = 32;
    patch.controller_reset.b[2] = false;
    EXPECT_FALSE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, original);
    ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a5000));
}

TEST(ProgramParameters, FreshCreationUsesSharedParametersAndKeepsNeutralDefaults) {
    axk::ProgramSpec program;
    program.number = 1;
    program.name = "TEST";
    program.assignments.push_back({"SBNK", "SAMPLE", {.receive = axk::ProgramReceiveChannel{axk::MidiPort::a, 1U}}});
    const auto neutral = axk::detail::prepare_prog_payload(program);
    ASSERT_TRUE(neutral);
    EXPECT_EQ((*neutral)[0x8b], std::byte{127});
    EXPECT_EQ((*neutral)[0x91], std::byte{90});
    EXPECT_EQ((*neutral)[0x94], std::byte{120});
    EXPECT_EQ((*neutral)[0x386], std::byte{4});
    auto expected = *neutral;
    program.parameters.level = 23;
    program.parameters.lfo.tempo = 222;
    program.parameters.step_wave.values[7] = 17;
    expected[0x8b] = std::byte{23};
    expected[0x94] = std::byte{222};
    expected[0x37d] = std::byte{17};
    const auto authored = axk::detail::prepare_prog_payload(program);
    ASSERT_TRUE(authored);
    EXPECT_EQ(*authored, expected);
    program.parameters.portamento.time = 0;
    EXPECT_FALSE(axk::detail::prepare_prog_payload(program));
}

TEST(ProgramParameters, DecodeRetainsInvalidRawFieldsWithoutInventingLegacyExtensions) {
    auto payload = program_payload();
    payload[0x8b] = std::byte{127};
    payload[0x8e] = std::byte{0x81};
    payload[0x81] = std::byte{0xff};
    payload[0x386] = std::byte{0xff};
    const auto decoded = axk::decode_object(payload);
    ASSERT_TRUE(decoded);
    const auto &program = std::get<axk::CurrentProg>(decoded->payload);
    EXPECT_EQ(program.parameters.level, 127);
    EXPECT_EQ(program.parameters.transpose, -127);
    EXPECT_FALSE(program.parameters.lfo.cycle);
    EXPECT_FALSE(program.parameters.lfo.wave);
    EXPECT_EQ(program.parameters.lfo.initial_phase, 3);
    EXPECT_FALSE(program.parameters.step_wave.step_count);
    EXPECT_EQ(program.raw_common_parameter_block[1], std::byte{0xff});
    EXPECT_EQ(program.raw_extended_parameter_block[0x1e], std::byte{0xff});
    for (const auto version : {1U, 2U}) {
        const auto legacy = axk::decode_object(program_payload(version));
        ASSERT_TRUE(legacy);
        const auto &value = std::get<axk::CurrentProg>(legacy->payload);
        EXPECT_FALSE(value.parameters.step_wave.step_count);
        EXPECT_FALSE(value.parameters.controller_reset.b[0]);
        EXPECT_TRUE(value.raw_extended_parameter_block.empty());
    }
}

TEST(ProgramParameters, AdAndConnectionPackedWritesAreIndependent) {
    auto payload = program_payload();
    auto expected = payload;
    axk::ProgramParameters patch;
    patch.ad.enabled = false;
    patch.ad.source = 1;
    patch.ad.left.pan = -63;
    patch.ad.right.pan = 63;
    patch.effect_connections[0] = 3;
    patch.effect_connections[1] = 4;
    expected[0x80] = std::byte{0x9a};
    expected[0x86] = std::byte{0xc1};
    expected[0x371] = std::byte{63};
    expected[0x36c] = std::byte{0xa4};
    ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a5000));
    EXPECT_EQ(payload, expected);
}

TEST(ProgramParameters, AdOutputsProjectInOrderAndPreserveUnmatchedLegacyLevels) {
    auto payload = program_payload();
    axk::ProgramParameters patch;
    patch.ad.left.output1.destination = 1;
    patch.ad.left.output1.level = 31;
    patch.ad.left.output2.destination = 6;
    patch.ad.left.output2.level = 72;
    auto expected = payload;
    expected[0x36d] = std::byte{1};
    expected[0x36e] = std::byte{31};
    expected[0x36f] = std::byte{6};
    expected[0x370] = std::byte{72};
    expected[0x87] = std::byte{1};
    expected[0x88] = std::byte{31};
    expected[0x89] = std::byte{0};
    ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, expected);
    payload[0x87] = std::byte{0x7e};
    const auto stale = payload;
    ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, stale);
    patch = {};
    patch.ad.right.output1.destination = 9;
    patch.ad.right.output1.level = 127;
    expected = payload;
    expected[0x372] = std::byte{9};
    expected[0x373] = std::byte{127};
    ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, expected);
}

TEST(ProgramParameters, ControllerWritesProjectOnlyChangedRecords) {
    auto payload = program_payload();
    axk::ProgramParameters patch;
    patch.controllers[2].device = 126;
    patch.controllers[2].function = 71;
    patch.controllers[2].type = 3;
    patch.controllers[2].range = -63;
    auto expected = payload;
    constexpr std::array<std::byte, 4> canonical{std::byte{126}, std::byte{71}, std::byte{3}, std::byte{0xc1}};
    for (std::size_t index = 0; index < canonical.size(); ++index) {
        expected[0x360 + index] = canonical[index];
        expected[0x118 + index] = index == 1U ? std::byte{} : canonical[index];
    }
    ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, expected);
    payload[0x119] = std::byte{22};
    const auto stale = payload;
    ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, stale);
    const auto decoded = axk::decode_object(payload);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(std::get<axk::CurrentProg>(decoded->payload).parameters.controllers[2].range, -63);
}

TEST(ProgramParameters, AdAndControllerModelLimitsAreValidatedBeforeAnyWrite) {
    auto payload = program_payload();
    axk::ProgramParameters patch;
    patch.level = 12;
    patch.ad.left.output1.destination = 12;
    patch.controllers[0].function = 128;
    const auto original = payload;
    EXPECT_FALSE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, original);
    ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a5000));
    for (auto invalid : {13U, 255U}) {
        patch.ad.right.output2.destination = static_cast<std::uint8_t>(invalid);
        const auto before = payload;
        EXPECT_FALSE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a5000));
        EXPECT_EQ(payload, before);
    }
}
