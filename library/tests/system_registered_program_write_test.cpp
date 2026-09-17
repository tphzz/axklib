#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/effects.hpp"
#include "axklib/system_file.hpp"

namespace {

axk::DecodedSystemFile retained_program(axk::ASeriesModel model, std::uint8_t revision = 0) {
    const bool native = model == axk::ASeriesModel::a3000;
    const auto size = native ? 0x400U : 0x1000U;
    std::vector<std::byte> bytes(axk::current_record_envelope_size + size, std::byte{0xa5});
    axk::ByteWriter writer{bytes};
    EXPECT_TRUE(writer.write_ascii_field(0, 12, "FSFSDEV3SPLX", std::byte{}));
    EXPECT_TRUE(writer.write_ascii_field(12, 4, "PRF3", std::byte{}));
    EXPECT_TRUE(writer.write_be32(0x14, 4));
    EXPECT_TRUE(writer.write_be32(0x18, 0x36));
    EXPECT_TRUE(writer.write_be32(0x1c, size + 8U));
    EXPECT_TRUE(writer.write_be32(0x30, native ? 0x21520531U : 0xdeadfaceU));
    bytes[0x3e] = static_cast<std::byte>(revision);
    return axk::decode_system_file(
               native ? axk::SystemFileKind::a3000_system : axk::SystemFileKind::a4000_a5000_system2, bytes)
        .value();
}

constexpr std::array models{axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000};

} // namespace

TEST(SystemRegisteredProgramWrite, EmptyPatchesPreserveEachModelAndStorageRevision) {
    for (auto model : models) {
        for (std::uint8_t revision = 0; revision <= (model == axk::ASeriesModel::a3000 ? 0U : 1U); ++revision) {
            const auto file = retained_program(model, revision);
            EXPECT_EQ(axk::patch_system_registered_program(file, {}, model).value(), file);
            EXPECT_EQ(axk::patch_system_file(file, {}, model).value(), file);
        }
    }
}

TEST(SystemRegisteredProgramWrite, CommonEditsAreExactAndPreserveNativeHighBitAndEveryGap) {
    for (auto model : models) {
        const auto file = retained_program(model);
        auto expected = file;
        const auto common = model == axk::ASeriesModel::a3000 ? 0x314U : 0x60cU;
        axk::ProgramParameters patch;
        patch.level = 0;
        patch.transpose = -12;
        patch.lfo.sync = 1;
        patch.controller_reset.a[0] = false;
        patch.note_toggle.a[15] = true;
        expected.system_bulk_bytes[common + 0x0bU] = std::byte{};
        expected.system_bulk_bytes[common + 0x0eU] = std::byte{0xf4};
        expected.system_bulk_bytes[common] = model == axk::ASeriesModel::a3000 ? std::byte{0xe5} : std::byte{0x65};
        expected.system_bulk_bytes[common + 3U] = std::byte{0xa4};
        const auto result = axk::patch_system_registered_program(file, patch, model);
        ASSERT_TRUE(result);
        EXPECT_EQ(*result, expected);
        EXPECT_EQ(axk::patch_system_registered_program(*result, patch, model).value(), *result);
        axk::SystemFilePatch combined;
        combined.registered_program = patch;
        EXPECT_EQ(axk::patch_system_file(file, combined, model).value(), expected);
    }
}

TEST(SystemRegisteredProgramWrite, NativeControllersHaveNoShadowButCurrentChangesProjectOnlyChangedRecords) {
    for (auto model : models) {
        for (std::size_t index = 0; index < 4U; ++index) {
            const bool native = model == axk::ASeriesModel::a3000;
            const auto file = retained_program(model);
            auto expected = file;
            const auto canonical = (native ? 0x2fcU : 0x5ccU) + index * 4U;
            axk::ProgramParameters patch;
            patch.controllers[index].function = native ? 63 : 71;
            expected.system_bulk_bytes[canonical + 1U] = native ? std::byte{63} : std::byte{71};
            if (!native) {
                const auto legacy = 0x544U + index * 4U;
                for (std::size_t byte = 0; byte < 4U; ++byte)
                    expected.system_bulk_bytes[legacy + byte] = expected.system_bulk_bytes[canonical + byte];
                expected.system_bulk_bytes[legacy + 1U] = std::byte{};
            }
            EXPECT_EQ(axk::patch_system_registered_program(file, patch, model).value(), expected);
            expected.system_bulk_bytes[native ? 0U : 0x544U + index * 4U] = std::byte{0x55};
            EXPECT_EQ(axk::patch_system_registered_program(expected, patch, model).value(), expected);
        }
    }
}

TEST(SystemRegisteredProgramWrite, ModelAndMalformedRequestsFailWithoutChangingInput) {
    for (auto model : models) {
        const auto file = retained_program(model);
        const auto before = axk::encode_system_file(file).value();
        const auto exercise = [&](auto select, unsigned maximum) {
            for (unsigned value = 0; value < 256U; ++value) {
                axk::ProgramParameters patch;
                patch.level = 42;
                select(patch) = static_cast<std::uint8_t>(value);
                const auto result = axk::patch_system_registered_program(file, patch, model);
                EXPECT_EQ(result.has_value(), value <= maximum) << value;
            }
        };
        exercise([](auto &p) -> auto & { return p.controllers[3].device; },
                 model == axk::ASeriesModel::a3000 ? 125 : 126);
        exercise([](auto &p) -> auto & { return p.controllers[3].function; },
                 model == axk::ASeriesModel::a3000   ? 63
                 : model == axk::ASeriesModel::a4000 ? 71
                                                     : 128);
        exercise([](auto &p) -> auto & { return p.lfo.wave; }, model == axk::ASeriesModel::a3000 ? 5 : 6);
        exercise([](auto &p) -> auto & { return p.lfo.sync; }, model == axk::ASeriesModel::a5000 ? 2 : 1);
        axk::ProgramParameters unsupported;
        unsupported.level = 42;
        unsupported.effects[5].enabled = false;
        EXPECT_EQ(axk::patch_system_registered_program(file, unsupported, model).has_value(),
                  model == axk::ASeriesModel::a5000);
        unsupported = {};
        unsupported.step_wave.step_count = 2;
        EXPECT_EQ(axk::patch_system_registered_program(file, unsupported, model).has_value(),
                  model != axk::ASeriesModel::a3000);
        unsupported = {};
        unsupported.ad.right.pan = 0;
        EXPECT_EQ(axk::patch_system_registered_program(file, unsupported, model).has_value(),
                  model != axk::ASeriesModel::a3000);
        EXPECT_EQ(axk::encode_system_file(file).value(), before);
        auto malformed = file;
        malformed.system_bulk_bytes.pop_back();
        EXPECT_FALSE(axk::patch_system_registered_program(malformed, {}, model));
        EXPECT_FALSE(axk::patch_system_registered_program(file, {}, static_cast<axk::ASeriesModel>(255)));
    }
}

TEST(SystemRegisteredProgramWrite, EveryCommonScalarUsesItsOwnByteAndDomain) {
    for (auto model : models) {
        const auto file = retained_program(model);
        const bool native = model == axk::ASeriesModel::a3000;
        const auto common = native ? 0x314U : 0x60cU;
        const auto exercise = [&](auto select, int minimum, int maximum, std::size_t offset) {
            for (unsigned raw = 0; raw < 256U; ++raw) {
                axk::ProgramParameters patch;
                using Value = typename std::remove_cvref_t<decltype(select(patch))>::value_type;
                const auto value = static_cast<Value>(raw);
                select(patch) = value;
                const auto result = axk::patch_system_registered_program(file, patch, model);
                const bool valid = static_cast<int>(value) >= minimum && static_cast<int>(value) <= maximum;
                ASSERT_EQ(result.has_value(), valid) << offset << ':' << raw;
                if (valid) {
                    auto expected = file;
                    expected.system_bulk_bytes[common + offset] = static_cast<std::byte>(raw);
                    EXPECT_EQ(*result, expected) << offset << ':' << raw;
                }
            }
        };
        exercise([](auto &p) -> auto & { return p.level; }, 0, 127, 0x0b);
        exercise([](auto &p) -> auto & { return p.transpose; }, -127, 127, 0x0e);
        exercise([](auto &p) -> auto & { return p.portamento.type; }, 0, 3, 0x10);
        exercise([](auto &p) -> auto & { return p.portamento.rate; }, 1, 127, 0x11);
        exercise([](auto &p) -> auto & { return p.portamento.time; }, 1, 127, 0x12);
        exercise([](auto &p) -> auto & { return p.lfo.sample_hold_speed; }, 0, 127, 0x13);
        exercise([](auto &p) -> auto & { return p.lfo.tempo; }, 25, 250, 0x14);
        exercise([](auto &p) -> auto & { return p.lfo.reset_note; }, -1, 127, 0x15);
        exercise([](auto &p) -> auto & { return p.lfo.reset_channel; }, -2, model == axk::ASeriesModel::a5000 ? 32 : 16,
                 0x0f);
        exercise([](auto &p) -> auto & { return p.ad.left.pan; }, -63, 63, 0x06);
        if (native) {
            exercise([](auto &p) -> auto & { return p.ad.left.output1.destination; }, 0, 4, 0x07);
            exercise([](auto &p) -> auto & { return p.ad.left.output1.level; }, 0, 127, 0x08);
            exercise([](auto &p) -> auto & { return p.ad.left.output2.destination; }, 0, 5, 0x09);
            exercise([](auto &p) -> auto & { return p.ad.left.output2.level; }, 0, 127, 0x0a);
        }
    }
}

TEST(SystemRegisteredProgramWrite, AllEffectTypesResetOnlyTheirOwnWordsAndRequiredShadow) {
    for (auto model : models) {
        const bool native = model == axk::ASeriesModel::a3000;
        const auto count = model == axk::ASeriesModel::a5000 ? 6U : 3U;
        for (std::size_t slot = 0; slot < count; ++slot) {
            for (unsigned type = 0; type <= (native ? 54U : 96U); ++type) {
                const auto file = retained_program(model);
                auto expected = file;
                const auto offset = native      ? 0x284U + slot * 40U
                                    : slot < 3U ? 0x4ccU + slot * 40U
                                                : 0x554U + (slot - 3U) * 40U;
                const auto info = axk::effect_write_info(
                    static_cast<std::uint16_t>(type), native ? axk::EffectProfile::a3000 : axk::EffectProfile::a4000);
                ASSERT_TRUE(info);
                expected.system_bulk_bytes[offset + (native ? 7U : 6U)] = static_cast<std::byte>(type);
                if (!native && slot < 3U)
                    expected.system_bulk_bytes[offset + 7U] = static_cast<std::byte>(info->legacy_type);
                axk::ByteWriter writer{expected.system_bulk_bytes};
                for (std::size_t word = 0; word < 16U; ++word)
                    ASSERT_TRUE(writer.write_be16(offset + 8U + word * 2U, info->reset_words[word]));
                axk::ProgramParameters patch;
                patch.effects[slot].type = static_cast<std::uint8_t>(type);
                const auto result = axk::patch_system_registered_program(file, patch, model);
                ASSERT_TRUE(result);
                EXPECT_EQ(*result, expected);
                expected.system_bulk_bytes[offset + 8U] = std::byte{0x55};
                EXPECT_EQ(axk::patch_system_registered_program(expected, patch, model).value(), expected);
            }
        }
    }
}

TEST(SystemRegisteredProgramWrite, ExtendedRoutesAndStepWavePreserveEveryUnrequestedByte) {
    for (auto model : {axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        for (std::uint8_t revision : {std::uint8_t{0}, std::uint8_t{1}}) {
            const auto file = retained_program(model, revision);
            auto expected = file;
            axk::ProgramParameters patch;
            patch.ad.left.output1.destination = 8;
            patch.ad.left.output1.level = 30;
            patch.ad.left.output2.destination = 3;
            patch.ad.left.output2.level = 40;
            patch.ad.right.pan = -63;
            patch.ad.right.output1.destination = 9;
            patch.ad.right.output1.level = 50;
            patch.ad.right.output2.destination = 0;
            patch.ad.right.output2.level = 60;
            patch.step_wave.step_count = 16;
            patch.step_wave.slope = axk::ProgramStepWaveSlope::both;
            const std::array outputs{8, 30, 3, 40, 193, 9, 50, 0, 60};
            for (std::size_t i = 0; i < outputs.size(); ++i)
                expected.system_bulk_bytes[0x5e1U + i] = static_cast<std::byte>(outputs[i]);
            // Both routes project to group 2. Output 1 wins; group 1's level survives.
            expected.system_bulk_bytes[0x613] = std::byte{};
            expected.system_bulk_bytes[0x615] = std::byte{4};
            expected.system_bulk_bytes[0x616] = std::byte{30};
            expected.system_bulk_bytes[0x5fa] = std::byte{0xbe};
            for (std::size_t i = 0; i < 16U; ++i) {
                patch.step_wave.values[i] = static_cast<std::uint8_t>(i);
                expected.system_bulk_bytes[0x5eaU + i] = static_cast<std::byte>(i);
            }
            const auto result = axk::patch_system_registered_program(file, patch, model);
            ASSERT_TRUE(result);
            EXPECT_EQ(*result, expected);
            expected.system_bulk_bytes[0x615] = std::byte{0x55};
            EXPECT_EQ(axk::patch_system_registered_program(expected, patch, model).value(), expected);
        }
    }
}

TEST(SystemRegisteredProgramWrite, PortBMapsAndSecondConnectionRequireA5000AndPreservePackedNeighbors) {
    for (std::size_t channel = 0; channel < 16U; ++channel) {
        const auto file = retained_program(axk::ASeriesModel::a5000);
        auto expected = file;
        axk::ProgramParameters patch;
        patch.controller_reset.b[channel] = false;
        patch.note_toggle.b[channel] = true;
        patch.effect_connections[1] = 2;
        const auto byte = channel < 8U ? 1U : 0U;
        const auto mask = 1U << (channel % 8U);
        expected.system_bulk_bytes[0x5dcU + byte] = static_cast<std::byte>(0xa5U & ~mask);
        expected.system_bulk_bytes[0x5deU + byte] = static_cast<std::byte>(0xa5U | mask);
        expected.system_bulk_bytes[0x5e0] = std::byte{0xa2};
        EXPECT_EQ(axk::patch_system_registered_program(file, patch, axk::ASeriesModel::a5000).value(), expected);
        for (auto model : {axk::ASeriesModel::a3000, axk::ASeriesModel::a4000})
            EXPECT_FALSE(axk::patch_system_registered_program(retained_program(model), patch, model));
    }
}

TEST(SystemRegisteredProgramWrite, LaterEffectFailureRejectsScalarAndEarlierEffectChanges) {
    for (auto model : models) {
        const auto file = retained_program(model);
        axk::ProgramParameters patch;
        patch.level = 42;
        patch.effects[0].type = 1;
        patch.effects[1].input_level = 128;
        EXPECT_FALSE(axk::patch_system_registered_program(file, patch, model));
        EXPECT_EQ(file, retained_program(model));
        const auto other = model == axk::ASeriesModel::a3000 ? axk::ASeriesModel::a4000 : axk::ASeriesModel::a3000;
        EXPECT_FALSE(axk::patch_system_registered_program(file, {}, other));
    }
}
