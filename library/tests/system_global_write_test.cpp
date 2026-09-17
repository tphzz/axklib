#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/system_file.hpp"

namespace {

constexpr std::array models{axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000};

axk::DecodedSystemFile retained_global_file(axk::ASeriesModel model, std::uint8_t revision = 0) {
    const auto native = model == axk::ASeriesModel::a3000;
    const auto size = native ? 0x400U : 0x1000U;
    std::vector<std::byte> bytes(axk::current_record_envelope_size + size);
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::byte>((i * 73U + 19U) & 255U);
    axk::ByteWriter writer{bytes};
    EXPECT_TRUE(writer.write_ascii_field(0, 12, "FSFSDEV3SPLX", std::byte{}));
    EXPECT_TRUE(writer.write_ascii_field(12, 4, "PRF3", std::byte{}));
    EXPECT_TRUE(writer.write_be32(0x14, 4));
    EXPECT_TRUE(writer.write_be32(0x18, 0x36));
    EXPECT_TRUE(writer.write_be32(0x1c, size + 8U));
    EXPECT_TRUE(writer.write_be32(0x30, native ? 0x21520531U : 0xdeadfaceU));
    bytes[0x3e] = static_cast<std::byte>(revision);
    auto file = axk::decode_system_file(
        native ? axk::SystemFileKind::a3000_system : axk::SystemFileKind::a4000_a5000_system2, bytes);
    EXPECT_TRUE(file);
    return file.value();
}

axk::DecodedSystemFile replace_global_byte(const axk::DecodedSystemFile &file, std::size_t offset, unsigned value) {
    auto bytes = axk::encode_system_file(file).value();
    bytes[0x60U + offset] = static_cast<std::byte>(value);
    return axk::decode_system_file(file.kind, bytes).value();
}

} // namespace

TEST(SystemGlobalWrite, NativeRemixPatchesPreserveOtherBytesAndRequireValidEffectiveNibbles) {
    const auto model = axk::ASeriesModel::a3000;
    const auto original = retained_global_file(model);
    for (unsigned raw = 0; raw < 256U; ++raw) {
        const auto file = replace_global_byte(original, 0x24, raw);
        const auto before = axk::encode_system_file(file).value();
        axk::SystemGlobalParameters patch;
        patch.remix_type_selection = 4;
        const auto type_only = axk::patch_system_global(file, patch, model);
        EXPECT_EQ(type_only.has_value(), (raw & 15U) <= 3U);
        if (type_only) {
            auto expected = before;
            expected[0x84] = static_cast<std::byte>(0x40U | (raw & 15U));
            EXPECT_EQ(axk::encode_system_file(*type_only).value(), expected);
        }
        patch = {};
        patch.remix_variation_selection = 3;
        const auto variation_only = axk::patch_system_global(file, patch, model);
        EXPECT_EQ(variation_only.has_value(), (raw >> 4U) <= 4U);
        if (variation_only) {
            auto expected = before;
            expected[0x84] = static_cast<std::byte>((raw & 0xf0U) | 3U);
            EXPECT_EQ(axk::encode_system_file(*variation_only).value(), expected);
        }
        patch.remix_type_selection = 4;
        const auto both = axk::patch_system_global(file, patch, model);
        ASSERT_TRUE(both) << both.error().message;
        auto expected = before;
        expected[0x84] = std::byte{0x43};
        EXPECT_EQ(axk::encode_system_file(*both).value(), expected);
        EXPECT_EQ(axk::encode_system_file(file).value(), before);
    }
    for (unsigned value = 0; value < 256U; ++value) {
        axk::SystemGlobalParameters patch;
        patch.remix_type_selection = static_cast<std::uint8_t>(value);
        patch.remix_variation_selection = 0;
        EXPECT_EQ(axk::patch_system_global(original, patch, model).has_value(), value <= 4U);
        patch.remix_type_selection = 0;
        patch.remix_variation_selection = static_cast<std::uint8_t>(value);
        EXPECT_EQ(axk::patch_system_global(original, patch, model).has_value(), value <= 3U);
    }
}

TEST(SystemGlobalWrite, EmptyAndScalarPatchesPreserveEveryOtherRecordByte) {
    for (const auto model : models) {
        const auto file = retained_global_file(model);
        const auto empty = axk::patch_system_global(file, {}, model);
        ASSERT_TRUE(empty);
        EXPECT_EQ(*empty, file);
        axk::SystemGlobalParameters patch;
        patch.master_fine_tune = -63;
        patch.master_coarse_tune = 127;
        patch.master_transpose = -127;
        patch.function_key_notes[5] = 12;
        patch.knob_transmit_channels[2] = -1;
        auto expected = axk::encode_system_file(file).value();
        expected[0x60] = std::byte{193};
        expected[0x61] = std::byte{127};
        expected[0x62] = std::byte{129};
        expected[0x7a] = std::byte{12};
        expected[0x69] = std::byte{255};
        const auto before = file;
        const auto result = axk::patch_system_global(file, patch, model);
        ASSERT_TRUE(result);
        EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
        EXPECT_EQ(file, before);
    }
}

TEST(SystemGlobalWrite, ModeWritesClearOmniEvenWhenTheRequestedModeIsUnchanged) {
    for (const auto model : {axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        for (const auto revision : {0U, 1U}) {
            const auto file = retained_global_file(model, static_cast<std::uint8_t>(revision));
            axk::SystemGlobalParameters patch;
            patch.program_mode = axk::ProgramMode::multi;
            patch.basic_receive_channel_selection = 15;
            patch.part_program_numbers[15] = 128;
            const auto result = axk::patch_system_global(file, patch, model);
            ASSERT_TRUE(result);
            const auto &context = std::get<axk::A4000A5000SystemContext>(result->context);
            EXPECT_EQ(context.saved_program_mode, axk::ProgramMode::multi);
            EXPECT_FALSE(context.omni);
            EXPECT_EQ(context.parts[15].program_number, 128);
            EXPECT_EQ(context.parts[15].master, true);
            auto expected = axk::encode_system_file(file).value();
            expected[0x64] = std::byte{15};
            expected[0x66] &= std::byte{0xfe};
            expected[0x8e] = std::byte{1};
            expected[0x9f] = std::byte{128};
            EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
            axk::SystemGlobalParameters enable;
            enable.omni = true;
            const auto enabled = axk::patch_system_global(*result, enable, model);
            ASSERT_TRUE(enabled);
            const auto repeated = axk::patch_system_global(*enabled, patch, model);
            ASSERT_TRUE(repeated);
            EXPECT_EQ(*repeated, *result);
            patch.omni = true;
            EXPECT_FALSE(axk::patch_system_global(file, patch, model));
        }
    }
}

TEST(SystemGlobalWrite, WaveAddressOptionsAreAnAtomicExclusiveGroupIncludingFalseWrites) {
    constexpr std::array members{&axk::SystemGlobalParameters::wave_length_lock,
                                 &axk::SystemGlobalParameters::wave_auto_zero,
                                 &axk::SystemGlobalParameters::wave_auto_snap};
    for (const auto model : models) {
        const auto file = retained_global_file(model);
        for (std::size_t i = 0; i < members.size(); ++i) {
            for (const auto value : {false, true}) {
                axk::SystemGlobalParameters patch;
                patch.*members[i] = value;
                auto expected = axk::encode_system_file(file).value();
                expected[0x66] &= std::byte{0xe3};
                if (value)
                    expected[0x66] |= static_cast<std::byte>(1U << (i + 2U));
                const auto result = axk::patch_system_global(file, patch, model);
                ASSERT_TRUE(result);
                EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
            }
        }
        axk::SystemGlobalParameters conflict;
        conflict.wave_length_lock = true;
        conflict.wave_auto_snap = true;
        EXPECT_FALSE(axk::patch_system_global(file, conflict, model));
    }
}

TEST(SystemGlobalWrite, ScalarCoordinatesDomainsAndModelAvailabilityAreIndependentOfTheReaderTable) {
    struct Field {
        std::size_t offset;
        int minimum;
        int maximum;
        std::function<void(axk::SystemGlobalParameters &, int)> set;
    };
    for (const auto model : models) {
        std::vector<Field> fields;
        const auto port_b = model == axk::ASeriesModel::a5000;
        auto add = [&](std::size_t offset, int minimum, int maximum, auto leaf) {
            fields.push_back({offset, minimum, maximum, [leaf](auto &p, int v) {
                                  using T = typename std::remove_reference_t<decltype(leaf(p))>::value_type;
                                  leaf(p) = static_cast<T>(v);
                              }});
        };
        add(0, -63, 63, [](auto &p) -> auto & { return p.master_fine_tune; });
        add(1, -127, 127, [](auto &p) -> auto & { return p.master_coarse_tune; });
        add(2, -127, 127, [](auto &p) -> auto & { return p.master_transpose; });
        add(3, 0, 17, [](auto &p) -> auto & { return p.velocity_curve_selection; });
        add(4, 0, port_b ? 31 : 15, [](auto &p) -> auto & { return p.basic_receive_channel_selection; });
        add(5, 0, 5, [](auto &p) -> auto & { return p.stereo_to_assignable_selection; });
        for (std::size_t i = 0; i < 4U; ++i) {
            add(7U + i, -1, port_b ? 32 : 16, [i](auto &p) -> auto & { return p.knob_transmit_channels[i]; });
            add(11U + i, 0, 120, [i](auto &p) -> auto & { return p.knob_control_devices[i]; });
        }
        for (std::size_t i = 0; i < 6U; ++i) {
            add(15U + i, 0, port_b ? 32 : 16, [i](auto &p) -> auto & { return p.function_key_transmit_channels[i]; });
            add(21U + i, 0, 127, [i](auto &p) -> auto & { return p.function_key_notes[i]; });
            add(27U + i, 1, 127, [i](auto &p) -> auto & { return p.function_key_velocities[i]; });
        }
        add(33, 0, 4, [](auto &p) -> auto & { return p.stereo_output_level_offset; });
        constexpr std::array<std::size_t, 4> eq_offsets{34, 37, 40, 43};
        for (std::size_t i = 0; i < 4U; ++i) {
            add(eq_offsets[i], i == 3U ? 28 : 4, i < 2U ? 40 : 58,
                [i](auto &p) -> auto & { return p.total_eq[i].frequency_selection; });
            add(eq_offsets[i] + 1U, 52, 76, [i](auto &p) -> auto & { return p.total_eq[i].gain_selection; });
            if (i != 0U)
                add(eq_offsets[i] + 2U, 10, 120, [i](auto &p) -> auto & { return p.total_eq[i].width_selection; });
        }
        add(46, 0, 1, [](auto &p) -> auto & { return p.program_mode; });
        for (std::size_t i = 0; i < 32U; ++i)
            add(48U + i, 1, 128, [i](auto &p) -> auto & { return p.part_program_numbers[i]; });
        for (std::size_t i = 0; i < 5U; ++i)
            add(442U + i, 0, 4, [i](auto &p) -> auto & { return p.assignable_output_level_offsets[i]; });
        const auto file = replace_global_byte(retained_global_file(model), 5, 0);
        for (const auto &field : fields) {
            for (const auto value : {field.minimum - 1, field.minimum, field.maximum, field.maximum + 1}) {
                SCOPED_TRACE(static_cast<int>(model));
                SCOPED_TRACE(field.offset);
                SCOPED_TRACE(value);
                axk::SystemGlobalParameters patch;
                field.set(patch, value);
                const auto result = axk::patch_system_global(file, patch, model);
                const auto available =
                    !(model == axk::ASeriesModel::a3000 && field.offset >= 46U) &&
                    !(model == axk::ASeriesModel::a4000 && field.offset >= 64U && field.offset < 80U);
                const auto valid = available && value >= field.minimum && value <= field.maximum;
                ASSERT_EQ(result.has_value(), valid);
                if (valid) {
                    auto expected = axk::encode_system_file(file).value();
                    expected[0x60U + field.offset] = static_cast<std::byte>(static_cast<std::uint8_t>(value));
                    if (field.offset == 46U)
                        expected[0x66] &= std::byte{0xfe};
                    EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
                }
            }
        }
    }
}

TEST(SystemGlobalWrite, OrdinaryFlagsPreserveAllUnrelatedBits) {
    constexpr std::array members{
        &axk::SystemGlobalParameters::omni, &axk::SystemGlobalParameters::program_change_enabled,
        &axk::SystemGlobalParameters::audition_with_easy_edit, &axk::SystemGlobalParameters::audition_with_effects,
        &axk::SystemGlobalParameters::play_and_load};
    constexpr std::array masks{1U, 2U, 32U, 64U, 128U};
    for (const auto model : models) {
        const auto original = retained_global_file(model);
        for (unsigned bits = 0; bits < 256U; ++bits) {
            const auto file = replace_global_byte(original, 6, bits);
            for (std::size_t i = 0; i < members.size(); ++i) {
                for (const auto enabled : {false, true}) {
                    axk::SystemGlobalParameters patch;
                    patch.*members[i] = enabled;
                    const auto result = axk::patch_system_global(file, patch, model);
                    ASSERT_TRUE(result);
                    auto expected = axk::encode_system_file(file).value();
                    expected[0x66] = static_cast<std::byte>((bits & ~masks[i]) | (enabled ? masks[i] : 0U));
                    EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
                }
            }
        }
    }
}

TEST(SystemGlobalWrite, RemixPatchesValidateEffectiveNibblesZonesAndLoadSensitiveFlags) {
    const auto model = axk::ASeriesModel::a5000;
    const auto file = retained_global_file(model);
    axk::SystemGlobalParameters patch;
    patch.remix_type_selection = 4;
    patch.remix_variation_selection = 7;
    patch.remix_zone_start = 7;
    patch.remix_zone_end = 8;
    patch.remix_auto_audition = false;
    patch.knob_midi_out = false;
    patch.function_key_midi_out = false;
    auto cleaned = replace_global_byte(file, 0x2f, 7);
    const auto before = cleaned;
    auto result = axk::patch_system_global(cleaned, patch, model);
    ASSERT_TRUE(result);
    auto expected = axk::encode_system_file(cleaned).value();
    expected[0x84] = std::byte{0x47};
    expected[0x8f] = std::byte{};
    expected[0x218] = std::byte{7};
    expected[0x219] = std::byte{8};
    EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
    EXPECT_EQ(cleaned, before);
    for (unsigned type = 5; type < 256U; ++type) {
        patch.remix_type_selection = static_cast<std::uint8_t>(type);
        EXPECT_FALSE(axk::patch_system_global(cleaned, patch, model));
    }
    patch.remix_type_selection = 4;
    patch.remix_variation_selection = 8;
    EXPECT_FALSE(axk::patch_system_global(cleaned, patch, model));
    patch.remix_variation_selection = 7;
    patch.remix_zone_end = 7;
    EXPECT_FALSE(axk::patch_system_global(cleaned, patch, model));
    patch.remix_zone_end = 9;
    EXPECT_FALSE(axk::patch_system_global(cleaned, patch, model));
    patch.remix_zone_end = 8;
    patch.knob_midi_out = true;
    EXPECT_FALSE(axk::patch_system_global(cleaned, patch, model));
    patch.knob_midi_out = false;
    patch.function_key_midi_out = true;
    EXPECT_FALSE(axk::patch_system_global(cleaned, patch, model));
    patch.function_key_midi_out = false;
    cleaned = replace_global_byte(cleaned, 0x2f, 0x87);
    EXPECT_FALSE(axk::patch_system_global(cleaned, patch, model));
    axk::SystemGlobalParameters partial;
    partial.remix_zone_start = 0;
    EXPECT_FALSE(axk::patch_system_global(replace_global_byte(file, 0x1b9, 0), partial, model));
    partial = {};
    partial.remix_variation_selection = 0;
    EXPECT_FALSE(axk::patch_system_global(replace_global_byte(file, 0x24, 0x50), partial, model));
    partial = {};
    partial.remix_type_selection = 0;
    EXPECT_FALSE(axk::patch_system_global(replace_global_byte(file, 0x24, 0x0f), partial, model));
    EXPECT_FALSE(
        axk::patch_system_global(retained_global_file(axk::ASeriesModel::a3000), patch, axk::ASeriesModel::a3000));
}

TEST(SystemGlobalWrite, OutputDuplicationRetainsInactiveOffsetsButRejectsEditingThem) {
    const auto model = axk::ASeriesModel::a5000;
    const auto file = retained_global_file(model);
    for (std::size_t i = 0; i < 5U; ++i) {
        axk::SystemGlobalParameters patch;
        patch.stereo_to_assignable_selection = static_cast<std::uint8_t>(i + 1U);
        const auto routed = axk::patch_system_global(file, patch, model);
        ASSERT_TRUE(routed);
        auto expected = axk::encode_system_file(file).value();
        expected[0x65] = static_cast<std::byte>(i + 1U);
        EXPECT_EQ(axk::encode_system_file(*routed).value(), expected);
        patch.assignable_output_level_offsets[i] = 3;
        EXPECT_FALSE(axk::patch_system_global(file, patch, model));
        patch.stereo_to_assignable_selection = 0;
        EXPECT_TRUE(axk::patch_system_global(file, patch, model));
    }
}

TEST(SystemGlobalWrite, InvalidInputAndMixedPatchesNeverMutateTheSource) {
    for (const auto model : models) {
        const auto file = retained_global_file(model);
        const auto before = file;
        axk::SystemGlobalParameters patch;
        patch.master_fine_tune = 5;
        patch.function_key_velocities[5] = 0;
        EXPECT_FALSE(axk::patch_system_global(file, patch, model));
        EXPECT_EQ(file, before);
        patch.function_key_velocities[5] = 127;
        patch.total_eq[0].width_selection = 10;
        EXPECT_FALSE(axk::patch_system_global(file, patch, model));
        EXPECT_FALSE(axk::patch_system_global(file, {}, static_cast<axk::ASeriesModel>(255)));
        EXPECT_FALSE(axk::patch_system_global(
            file, {}, model == axk::ASeriesModel::a3000 ? axk::ASeriesModel::a4000 : axk::ASeriesModel::a3000));
        auto damaged = file;
        damaged.system_bulk_bytes.pop_back();
        EXPECT_FALSE(axk::patch_system_global(damaged, {}, model));
        damaged = file;
        damaged.system_header_bytes[0] = std::byte{};
        EXPECT_FALSE(axk::patch_system_global(damaged, {}, model));
        damaged = file;
        damaged.storage_revision = 99;
        EXPECT_FALSE(axk::patch_system_global(damaged, {}, model));
    }
}
