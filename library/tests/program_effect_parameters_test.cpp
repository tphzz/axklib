#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/effects.hpp"
#include "axklib/object.hpp"
#include "axklib/program_parameter_codec.hpp"
#include "axklib/writer_internal.hpp"

namespace {

std::vector<std::byte> effect_payload() {
    std::vector<std::byte> result(0x390U, std::byte{0xa5});
    axk::ByteWriter writer{result};
    EXPECT_TRUE(writer.write_ascii_field(0, 16, "FSFSDEV3SPLXPROG"));
    EXPECT_TRUE(writer.write_be32(0x14, 4));
    EXPECT_TRUE(writer.write_be32(0x18, 0x2b0));
    EXPECT_TRUE(writer.write_be32(0x1c, 0x360));
    EXPECT_TRUE(writer.write_be16(0x96, 0));
    return result;
}

} // namespace

TEST(ProgramEffects, TypeChangeResetsAllWordsBeforeOverrides) {
    auto payload = effect_payload();
    auto expected = payload;
    axk::ProgramParameters patch;
    patch.effects[0].type = 47;
    patch.effects[0].parameters[0] = 49;
    constexpr std::array<std::uint16_t, 16> hall{49, 18, 10, 8, 13, 49, 0, 4, 50, 8, 64, 5, 5, 5, 5, 5};
    expected[0x9e] = expected[0x9f] = std::byte{47};
    axk::ByteWriter writer{expected};
    for (std::size_t index = 0; index < hall.size(); ++index)
        ASSERT_TRUE(writer.write_be16(0xa0U + index * 2U, hall[index]));
    ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, expected);
    ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, expected);
}

TEST(ProgramEffects, BypassAndSameTypePreserveHiddenWideAndLegacyValues) {
    for (const auto type : {1U, 97U}) {
        auto payload = effect_payload();
        payload[0x9e] = static_cast<std::byte>(type);
        auto expected = payload;
        axk::ProgramParameters patch;
        patch.effects[0].enabled = false;
        expected[0x98] = std::byte{};
        ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
        EXPECT_EQ(payload, expected);
        patch = {};
        patch.effects[0].type = static_cast<std::uint8_t>(type);
        const auto applied = axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000);
        EXPECT_EQ(applied.has_value(), type == 1U);
        EXPECT_EQ(payload, expected);
    }
}

TEST(ProgramEffects, ParameterOnlyWriteIsByteLocalAndUsesU16) {
    auto payload = effect_payload();
    payload[0x9e] = std::byte{1};
    auto expected = payload;
    axk::ProgramParameters patch;
    patch.effects[0].parameters[1] = 2345;
    expected[0xa2] = std::byte{9};
    expected[0xa3] = std::byte{0x29};
    ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, expected);
    const auto decoded = axk::decode_object(payload);
    ASSERT_TRUE(decoded);
    const auto &effect = std::get<axk::CurrentProg>(decoded->payload).parameters.effects[0];
    EXPECT_EQ(effect.parameters[1], 2345);
    EXPECT_FALSE(effect.parameters[15]);
}

TEST(ProgramEffects, InvalidWordsAndActionsRejectTheWholePatch) {
    for (const auto &[type, index, word] : std::array<std::array<unsigned, 3>, 6>{
             {{47, 11, 0}, {91, 14, 0}, {67, 3, 21}, {1, 1, 65535}, {12, 11, 0}, {14, 9, 0}}}) {
        auto payload = effect_payload();
        const auto original = payload;
        axk::ProgramParameters patch;
        patch.level = 3;
        patch.effects[0].type = static_cast<std::uint8_t>(type);
        patch.effects[0].parameters[index] = static_cast<std::uint16_t>(word);
        EXPECT_FALSE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
        EXPECT_EQ(payload, original);
    }
}

TEST(ProgramEffects, ModelsBoundSlotsAndDestinationsWithoutClearingInactiveRoutes) {
    auto payload = effect_payload();
    axk::ProgramParameters patch;
    patch.effects[0].destination = 8;
    const auto original = payload;
    EXPECT_FALSE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, original);
    ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a5000));
    patch = {};
    patch.effects[5].enabled = false;
    const auto before = payload;
    EXPECT_FALSE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload, before);
    ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a5000));
    patch.effects[5].destination = 6;
    const auto unchanged = payload;
    EXPECT_FALSE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a5000));
    EXPECT_EQ(payload, unchanged);
    patch = {};
    patch.effect_connections[0] = 2;
    ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
    EXPECT_EQ(payload[0x9c], std::byte{8});
}

TEST(ProgramEffects, FreshExplicitThroughDiffersFromOmittedResetTemplate) {
    axk::ProgramSpec program;
    program.number = 1;
    program.name = "FX";
    program.assignments.push_back({"SBNK", "SAMPLE", {.receive = axk::ProgramReceiveChannel{axk::MidiPort::a, 1U}}});
    const auto neutral = axk::detail::prepare_prog_payload(program);
    ASSERT_TRUE(neutral);
    for (std::size_t index = 0; index < 16U; ++index)
        EXPECT_EQ(*axk::ByteReader{*neutral}.be16(0xa0U + index * 2U), 0);
    program.parameters.effects[0].type = 0;
    const auto explicit_type = axk::detail::prepare_prog_payload(program);
    ASSERT_TRUE(explicit_type);
    for (std::size_t index = 0; index < 16U; ++index)
        EXPECT_EQ(*axk::ByteReader{*explicit_type}.be16(0xa0U + index * 2U), 6);
}

TEST(ProgramEffects, EveryOrdinaryTypeHasExactResetAndBoundaryWrites) {
    for (std::uint16_t type = 0; type <= 96U; ++type) {
        SCOPED_TRACE(type);
        auto payload = effect_payload();
        axk::ProgramParameters patch;
        patch.effects[0].type = static_cast<std::uint8_t>(type);
        ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, axk::ASeriesModel::a4000));
        const auto info = axk::effect_write_info(type);
        ASSERT_TRUE(info);
        for (std::size_t index = 0; index < 16U; ++index) {
            EXPECT_EQ(*axk::ByteReader{payload}.be16(0xa0U + index * 2U), info->reset_words[index]);
            const auto &domain = info->parameters[index];
            if (domain.kind != axk::EffectParameterKind::stored_value)
                continue;
            for (const auto value : {domain.minimum, domain.maximum}) {
                auto boundary = payload;
                axk::ProgramParameters update;
                update.effects[0].parameters[index] = value;
                ASSERT_TRUE(axk::detail::apply_program_parameters(boundary, update, axk::ASeriesModel::a4000));
                EXPECT_EQ(*axk::ByteReader{boundary}.be16(0xa0U + index * 2U), value);
            }
        }
    }
}

TEST(ProgramEffects, EveryPhysicalSlotRejectsInvalidWordsAndPreservesUnchangedBytes) {
    for (const auto model : {axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        for (std::size_t slot = 0; slot < (model == axk::ASeriesModel::a4000 ? 3U : 6U); ++slot) {
            const auto offset = slot < 3U ? 0x98U + slot * 0x28U : 0x2e0U + (slot - 3U) * 0x28U;
            for (std::uint16_t type = 0; type <= 96U; ++type) {
                SCOPED_TRACE(static_cast<int>(model));
                SCOPED_TRACE(slot);
                SCOPED_TRACE(type);
                const auto info = axk::effect_write_info(type);
                ASSERT_TRUE(info);
                auto original = effect_payload();
                original[offset + 6U] = static_cast<std::byte>(type);
                for (std::size_t index = 0; index < 16U; ++index) {
                    SCOPED_TRACE(index);
                    const auto &domain = info->parameters[index];
                    axk::ProgramParameters patch;
                    auto &word = patch.effects[slot].parameters[index];
                    if (domain.kind == axk::EffectParameterKind::stored_value) {
                        const auto nominal =
                            static_cast<std::uint16_t>(domain.minimum + (domain.maximum - domain.minimum) / 2U);
                        for (const auto value : {domain.minimum, nominal, domain.maximum}) {
                            auto payload = original;
                            auto expected = original;
                            ASSERT_TRUE(axk::ByteWriter{expected}.write_be16(offset + 8U + index * 2U, value));
                            word = value;
                            ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, model));
                            EXPECT_EQ(payload, expected);
                            ASSERT_TRUE(axk::detail::apply_program_parameters(payload, patch, model));
                            EXPECT_EQ(payload, expected);
                        }
                    }
                    std::vector<std::uint16_t> rejected;
                    if (domain.kind != axk::EffectParameterKind::stored_value) {
                        rejected = {0U, 65535U};
                    } else {
                        if (domain.minimum > 0U)
                            rejected.push_back(static_cast<std::uint16_t>(domain.minimum - 1U));
                        if (domain.maximum < 65535U)
                            rejected.push_back(static_cast<std::uint16_t>(domain.maximum + 1U));
                    }
                    for (const auto value : rejected) {
                        auto payload = original;
                        patch.level = 3U;
                        word = value;
                        EXPECT_FALSE(axk::detail::apply_program_parameters(payload, patch, model));
                        EXPECT_EQ(payload, original);
                    }
                }
            }
        }
    }
}

TEST(ProgramEffects, FreshTypeSelectionInitializesOnlyTheRequestedPhysicalBlock) {
    axk::ProgramSpec program;
    program.number = 1;
    program.name = "FRESH";
    program.model = axk::ASeriesModel::a5000;
    program.assignments.push_back({"SBNK", "SAMPLE", {}});
    const auto neutral = axk::detail::prepare_prog_payload(program);
    ASSERT_TRUE(neutral);
    for (std::size_t slot = 0; slot < 6U; ++slot) {
        const auto offset = slot < 3U ? 0x98U + slot * 0x28U : 0x2e0U + (slot - 3U) * 0x28U;
        for (std::uint16_t type = 0; type <= 96U; ++type) {
            SCOPED_TRACE(slot);
            SCOPED_TRACE(type);
            const auto info = axk::effect_write_info(type);
            ASSERT_TRUE(info);
            auto expected = *neutral;
            expected[offset + 6U] = static_cast<std::byte>(type);
            if (slot < 3U)
                expected[offset + 7U] = static_cast<std::byte>(info->legacy_type);
            for (std::size_t index = 0; index < 16U; ++index)
                ASSERT_TRUE(axk::ByteWriter{expected}.write_be16(offset + 8U + index * 2U, info->reset_words[index]));
            program.parameters = {};
            program.parameters.effects[slot].type = static_cast<std::uint8_t>(type);
            const auto payload = axk::detail::prepare_prog_payload(program);
            ASSERT_TRUE(payload);
            EXPECT_EQ(*payload, expected);
        }
    }
}
