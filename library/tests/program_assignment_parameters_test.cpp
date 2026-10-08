#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/object.hpp"
#include "axklib/program_parameter_codec.hpp"

namespace {

std::vector<std::byte> assignments(std::uint16_t count = 3, std::size_t capacity = 8, std::uint32_t version = 4U) {
    const auto size = 0x120U + 0x38U * capacity + (version == 4U ? 0xb0U : 0U);
    std::vector<std::byte> result(size + 64U, std::byte{0xa5});
    axk::ByteWriter writer{result};
    EXPECT_TRUE(writer.write_ascii_field(0, 16, "FSFSDEV3SPLXPROG"));
    EXPECT_TRUE(writer.write_be32(0x14, version));
    EXPECT_TRUE(writer.write_be32(0x18, static_cast<std::uint32_t>(size - (version == 4U ? 0xe0U : 0x30U))));
    EXPECT_TRUE(writer.write_be32(0x1c, version == 4U ? static_cast<std::uint32_t>(size - 0x30U) : 0U));
    EXPECT_TRUE(writer.write_u8(0x30, 0x14));
    EXPECT_TRUE(writer.write_be16(0x96, count));
    for (std::size_t index = 0; index < count; ++index) {
        const auto row = 0x120U + index * 0x38U;
        EXPECT_TRUE(writer.write_ascii_field(row, 16, "TARGET"));
        result[row + 0x14U] = std::byte{0x10};
        result[row + 0x1eU] = result[row + 0x21U] = std::byte{127};
        result[row + 0x1fU] = result[row + 0x22U] = std::byte{};
    }
    return result;
}

axk::ProgramAssignmentParameterPatch patch_for(std::size_t ordinal) {
    axk::ProgramAssignmentParameterPatch patch;
    patch.ordinal = ordinal;
    patch.expected_target_kind = "SBNK";
    patch.expected_target_name = "TARGET";
    return patch;
}

} // namespace

TEST(ProgramAssignments, OffsetsAndInheritancePreserveOtherBytesAndRawPan) {
    auto payload = assignments();
    auto patch = patch_for(1);
    patch.parameters.pan_offset = 100;
    patch.parameters.portamento = axk::ProgramInheritableSwitch::inherit;
    patch.parameters.mono = axk::ProgramInheritableSwitch::off;
    patch.parameters.key_crossfade = axk::ProgramInheritableSwitch::on;
    patch.parameters.receive = axk::ProgramReceiveBasic{};
    patch.parameters.alternate_group = -1;
    patch.parameters.midi_control = false;
    auto expected = payload;
    expected[0x170] = std::byte{100};
    expected[0x17b] = std::byte{0x93};
    expected[0x16d] = std::byte{16};
    expected[0x17c] = std::byte{0xff};
    expected[0x18b] = std::byte{};
    ASSERT_TRUE(axk::detail::apply_program_assignment_patches(payload, std::array{patch}, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, expected);
    patch.parameters.pan_offset = -127;
    ASSERT_TRUE(axk::detail::apply_program_assignment_patches(payload, std::array{patch}, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload[0x170], std::byte{0x81});
}

TEST(ProgramAssignments, OutputProjectionUsesOutputOneLastAndKeepsUnmatchedLevel) {
    auto payload = assignments();
    auto patch = patch_for(0);
    patch.parameters.output1 = 5;
    patch.parameters.output2 = 1;
    patch.parameters.output1_level_offset = -12;
    patch.parameters.output2_level_offset = 34;
    auto expected = payload;
    expected[0x13d] = std::byte{5};
    expected[0x148] = std::byte{1};
    expected[0x14f] = std::byte{0xf4};
    expected[0x152] = std::byte{34};
    expected[0x14d] = std::byte{0xff};
    expected[0x150] = std::byte{1};
    expected[0x151] = std::byte{0xf4};
    ASSERT_TRUE(axk::detail::apply_program_assignment_patches(payload, std::array{patch}, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, expected);
    payload[0x150] = std::byte{88};
    const auto stale = payload;
    ASSERT_TRUE(axk::detail::apply_program_assignment_patches(payload, std::array{patch}, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, stale);
}

TEST(ProgramAssignments, MergedKeyAndVelocityLimitsValidateAtomically) {
    auto payload = assignments();
    auto first = patch_for(0);
    first.parameters.level_offset = 5;
    auto second = patch_for(1);
    second.parameters.key_low = 110;
    second.parameters.key_high = 100;
    const auto original = payload;
    EXPECT_FALSE(
        axk::detail::apply_program_assignment_patches(payload, std::array{first, second}, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, original);
    second.parameters.key_high = 120;
    second.parameters.velocity_low = 127;
    second.parameters.velocity_high = 127;
    ASSERT_TRUE(
        axk::detail::apply_program_assignment_patches(payload, std::array{first, second}, axk::ASeriesModel::a4000));
    const auto before = payload;
    second.parameters.key_high = 109;
    second.parameters.key_low.reset();
    EXPECT_FALSE(axk::detail::apply_program_assignment_patches(payload, std::array{second}, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, before);
}

TEST(ProgramAssignments, OrdinalIdentityAndCountGuardsRejectStaleOrDuplicateRows) {
    auto payload = assignments();
    auto patch = patch_for(0);
    patch.parameters.level_offset = 1;
    const auto original = payload;
    EXPECT_FALSE(
        axk::detail::apply_program_assignment_patches(payload, std::array{patch, patch}, axk::ASeriesModel::a4000));
    for (const auto ordinal : {3U, 8U, 999U}) {
        patch.ordinal = ordinal;
        EXPECT_FALSE(
            axk::detail::apply_program_assignment_patches(payload, std::array{patch}, axk::ASeriesModel::a4000));
    }
    patch.ordinal = 0;
    patch.expected_target_name = "STALE";
    EXPECT_FALSE(axk::detail::apply_program_assignment_patches(payload, std::array{patch}, axk::ASeriesModel::a4000));
    patch.expected_target_name = "TARGET";
    patch.expected_target_kind = "SBAC";
    EXPECT_FALSE(axk::detail::apply_program_assignment_patches(payload, std::array{patch}, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, original);
}

TEST(ProgramAssignments, LastCountedRowAt999IsIndependentOfFreshCapacity) {
    auto payload = assignments(999, 1001);
    auto patch = patch_for(998);
    patch.parameters.level_offset = 127;
    auto expected = payload;
    expected[0x120U + 998U * 0x38U + 0x16U] = std::byte{127};
    ASSERT_TRUE(axk::detail::apply_program_assignment_patches(payload, std::array{patch}, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, expected);
}

TEST(ProgramAssignments, ReceiveChannelsAreTypedAndModelBounded) {
    auto payload = assignments();
    auto patch = patch_for(0);
    patch.parameters.receive = axk::ProgramReceiveChannel{axk::MidiPort::b, 16};
    const auto original = payload;
    EXPECT_FALSE(axk::detail::apply_program_assignment_patches(payload, std::array{patch}, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, original);
    ASSERT_TRUE(axk::detail::apply_program_assignment_patches(payload, std::array{patch}, axk::ASeriesModel::a5000));
    EXPECT_EQ(payload[0x135], std::byte{32});
    for (const auto channel : {0U, 17U, 255U}) {
        patch.parameters.receive = axk::ProgramReceiveChannel{axk::MidiPort::a, static_cast<std::uint8_t>(channel)};
        const auto before = payload;
        EXPECT_FALSE(
            axk::detail::apply_program_assignment_patches(payload, std::array{patch}, axk::ASeriesModel::a5000));
        EXPECT_EQ(payload, before);
    }
}

TEST(ProgramAssignments, NativeOutputsUseTheirOwnLanesAndPreserveDormantRows) {
    auto payload = assignments(3, 8, 2U);
    auto patch = patch_for(1);
    patch.parameters.output1 = 4;
    patch.parameters.output2 = 5;
    patch.parameters.output1_level_offset = -12;
    patch.parameters.output2_level_offset = 34;
    auto expected = payload;
    constexpr auto row = 0x120U + 0x38U;
    expected[row + 0x2dU] = std::byte{4};
    expected[row + 0x2eU] = std::byte{0xf4};
    expected[row + 0x30U] = std::byte{5};
    expected[row + 0x31U] = std::byte{34};
    ASSERT_TRUE(axk::detail::apply_program_assignment_patches(payload, std::array{patch}, axk::ASeriesModel::a3000));
    EXPECT_EQ(payload, expected);
    ASSERT_TRUE(axk::detail::apply_program_assignment_patches(payload, std::array{patch}, axk::ASeriesModel::a3000));
    EXPECT_EQ(payload, expected);
    const auto decoded = axk::decode_object(payload);
    ASSERT_TRUE(decoded);
    const auto &parameters = std::get<axk::CurrentProg>(decoded->payload).assignments[1].parameters;
    EXPECT_EQ(parameters.output1, 4);
    EXPECT_EQ(parameters.output2, 5);
    EXPECT_EQ(parameters.output1_level_offset, -12);
    EXPECT_EQ(parameters.output2_level_offset, 34);
}

TEST(ProgramAssignments, NativeInvalidLaterRowRejectsTheEntirePatchSet) {
    const auto original = assignments(3, 8, 2U);
    auto first = patch_for(0);
    first.parameters.level_offset = 12;
    std::array<axk::ProgramAssignmentParameters, 5> invalid{};
    invalid[0].output1 = 5;
    invalid[1].output2 = 6;
    invalid[2].receive = axk::ProgramReceiveChannel{axk::MidiPort::b, 1};
    invalid[3].high_velocity_crossfade_offset = 5;
    invalid[4].velocity_sensitivity_offset = 0;
    for (const auto &parameters : invalid) {
        auto payload = original;
        auto second = patch_for(1);
        second.parameters = parameters;
        EXPECT_FALSE(axk::detail::apply_program_assignment_patches(payload, std::array{first, second},
                                                                   axk::ASeriesModel::a3000));
        EXPECT_EQ(payload, original);
    }
}

TEST(ProgramAssignments, NativeVelocityCrossfadePacksOnlyTheUpperTwoBits) {
    for (const auto setting : {axk::ProgramInheritableSwitch::inherit, axk::ProgramInheritableSwitch::off,
                               axk::ProgramInheritableSwitch::on}) {
        SCOPED_TRACE(static_cast<int>(setting));
        auto payload = assignments(3, 8, 2U);
        auto patch = patch_for(1);
        patch.parameters.velocity_crossfade = setting;
        auto expected = payload;
        constexpr auto row = 0x120U + 0x38U;
        const auto packed = setting == axk::ProgramInheritableSwitch::inherit ? 3U : static_cast<unsigned>(setting);
        expected[row + 0x23U] = static_cast<std::byte>(0x25U | (packed << 6U));
        ASSERT_TRUE(
            axk::detail::apply_program_assignment_patches(payload, std::array{patch}, axk::ASeriesModel::a3000));
        EXPECT_EQ(payload, expected);
        const auto decoded = axk::decode_object(payload);
        ASSERT_TRUE(decoded);
        const auto &parameters = std::get<axk::CurrentProg>(decoded->payload).assignments[1].parameters;
        EXPECT_EQ(parameters.velocity_crossfade, setting);
        EXPECT_FALSE(parameters.high_velocity_crossfade_offset);
        EXPECT_FALSE(parameters.low_velocity_crossfade_offset);
    }
}

TEST(ProgramAssignments, NativeVelocityCrossfadeRejectsReservedValueAtomically) {
    auto payload = assignments(3, 8, 2U);
    const auto original = payload;
    auto first = patch_for(0);
    first.parameters.level_offset = 12;
    auto second = patch_for(1);
    second.parameters.velocity_crossfade = static_cast<axk::ProgramInheritableSwitch>(2);
    EXPECT_FALSE(
        axk::detail::apply_program_assignment_patches(payload, std::array{first, second}, axk::ASeriesModel::a3000));
    EXPECT_EQ(payload, original);
    const auto decoded = axk::decode_object(payload);
    ASSERT_TRUE(decoded);
    EXPECT_FALSE(std::get<axk::CurrentProg>(decoded->payload).assignments[1].parameters.velocity_crossfade);
}

TEST(ProgramAssignments, CurrentVelocityCrossfadeUsesIndependentOffsetsNotTheNativeSwitch) {
    auto payload = assignments();
    const auto original = payload;
    auto patch = patch_for(0);
    patch.parameters.velocity_crossfade = axk::ProgramInheritableSwitch::on;
    patch.parameters.level_offset = 12;
    EXPECT_FALSE(axk::detail::apply_program_assignment_patches(payload, std::array{patch}, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, original);
    const auto decoded = axk::decode_object(payload);
    ASSERT_TRUE(decoded);
    EXPECT_FALSE(std::get<axk::CurrentProg>(decoded->payload).assignments[0].parameters.velocity_crossfade);
}

TEST(ProgramAssignments, NativeOutputBoundariesPreserveCurrentOnlyLanes) {
    for (const auto output1 : std::array<std::int8_t, 3>{-1, 0, 4}) {
        for (const auto output2 : std::array<std::int8_t, 3>{-1, 0, 5}) {
            auto payload = assignments(3, 8, 2U);
            auto patch = patch_for(0);
            patch.parameters.output1 = output1;
            patch.parameters.output2 = output2;
            patch.parameters.output1_level_offset = -127;
            patch.parameters.output2_level_offset = 127;
            auto expected = payload;
            expected[0x14d] = static_cast<std::byte>(static_cast<std::uint8_t>(output1));
            expected[0x14e] = std::byte{0x81};
            expected[0x150] = static_cast<std::byte>(static_cast<std::uint8_t>(output2));
            expected[0x151] = std::byte{127};
            ASSERT_TRUE(
                axk::detail::apply_program_assignment_patches(payload, std::array{patch}, axk::ASeriesModel::a3000));
            EXPECT_EQ(payload, expected);
        }
    }
}
