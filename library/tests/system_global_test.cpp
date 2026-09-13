#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>

#include <gtest/gtest.h>

#include "axklib/system_file.hpp"

namespace {

axk::DecodedSystemFile global_file(bool native) {
    axk::DecodedSystemFile file;
    file.kind = native ? axk::SystemFileKind::a3000_system : axk::SystemFileKind::a4000_a5000_system2;
    file.system_bulk_bytes.resize(native ? 0x348U : 0xfe0U, std::byte{0xa5});
    auto global = std::span{file.system_bulk_bytes}.subspan(0x10U, native ? 0x2eU : 0x1c0U);
    std::ranges::fill(global, std::byte{});
    constexpr std::array<std::uint8_t, 46> defaults{
        0,  0,  0,  0,  0,   0,   0x42, 0,   0,   0,   0, 74, 71, 73, 72, 0,  0,  0,  0,  0,  0,  60, 62,
        64, 65, 67, 69, 127, 127, 127,  127, 127, 127, 2, 14, 64, 0,  26, 64, 10, 45, 64, 10, 52, 64, 10};
    std::ranges::transform(defaults, global.begin(), [](auto value) { return static_cast<std::byte>(value); });
    if (!native) {
        global[0x2f] = std::byte{1};
        for (std::size_t i = 0; i < 32; ++i)
            global[0x30U + i] = static_cast<std::byte>(i + 1U);
        global[0x1b9] = std::byte{8};
        std::ranges::fill(global.subspan(0x1ba, 5), std::byte{2});
    }
    return file;
}

std::span<std::byte> global_bytes(axk::DecodedSystemFile &file) {
    return std::span{file.system_bulk_bytes}.subspan(0x10U,
                                                     file.kind == axk::SystemFileKind::a3000_system ? 0x2eU : 0x1c0U);
}

} // namespace

TEST(SystemGlobal, ReadsCommonDefaultsAndKeepsCurrentOnlyStateAbsentOnNative) {
    for (const auto native : {true, false}) {
        const auto file = global_file(native);
        const auto before = file;
        const auto decoded = axk::decode_system_global(file);
        ASSERT_TRUE(decoded);
        const auto &p = decoded->parameters;
        EXPECT_EQ(p.master_fine_tune, 0);
        EXPECT_EQ(p.master_coarse_tune, 0);
        EXPECT_EQ(p.master_transpose, 0);
        EXPECT_EQ(p.velocity_curve_selection, 0);
        EXPECT_EQ(p.basic_receive_channel_selection, 0);
        EXPECT_EQ(p.omni, false);
        EXPECT_EQ(p.program_change_enabled, true);
        EXPECT_EQ(p.knob_control_devices[0], 74);
        EXPECT_EQ(p.function_key_notes[5], 69);
        EXPECT_EQ(p.function_key_velocities[5], 127);
        EXPECT_EQ(p.stereo_output_level_offset, 2);
        EXPECT_EQ(p.audition_with_effects, true);
        EXPECT_EQ(p.total_eq[0].frequency_selection, 14);
        EXPECT_EQ(p.total_eq[3].frequency_selection, 52);
        for (std::size_t i = 0; i < p.total_eq.size(); ++i) {
            EXPECT_EQ(p.total_eq[i].gain_selection, 64);
            EXPECT_EQ(p.total_eq[i].width_selection.has_value(), i != 0U);
        }
        EXPECT_EQ(p.program_mode.has_value(), !native);
        EXPECT_EQ(p.remix_type_selection, 0);
        EXPECT_EQ(p.remix_variation_selection, 0);
        EXPECT_EQ(p.remix_auto_audition.has_value(), !native);
        EXPECT_EQ(p.knob_midi_out.has_value(), !native);
        EXPECT_EQ(p.function_key_midi_out.has_value(), !native);
        EXPECT_EQ(p.remix_zone_start.has_value(), !native);
        EXPECT_EQ(p.remix_zone_end.has_value(), !native);
        for (std::size_t i = 0; i < 32U; ++i) {
            EXPECT_EQ(p.part_program_numbers[i].has_value(), !native);
            if (!native)
                EXPECT_EQ(*p.part_program_numbers[i], i + 1U);
        }
        EXPECT_EQ(decoded->registered_remix.has_value(), !native);
        EXPECT_EQ(decoded->working_program_marker.has_value(), !native);
        EXPECT_EQ(decoded->raw_bytes.size(), native ? 46U : 448U);
        EXPECT_EQ(file, before);
    }
}

TEST(SystemGlobal, RetainsEveryRecipeByteWithoutWalkingUnterminatedPatterns) {
    auto file = global_file(false);
    auto bytes = global_bytes(file);
    for (std::size_t i = 0; i < 360U; ++i)
        bytes[0x50U + i] = static_cast<std::byte>((i * 17U + 1U) % 256U);
    bytes[0x1bf] = std::byte{0xfe};
    const auto decoded = axk::decode_system_global(file);
    ASSERT_TRUE(decoded);
    ASSERT_TRUE(decoded->registered_remix);
    for (std::size_t slot = 0; slot < 5U; ++slot) {
        const auto &recipe = decoded->registered_remix->at(slot);
        for (std::size_t i = 0; i < 24U; ++i) {
            EXPECT_EQ(recipe.duration_codes[i], std::to_integer<std::uint8_t>(bytes[0x50U + slot * 24U + i]));
            EXPECT_EQ(recipe.random_choices[i], std::to_integer<std::uint8_t>(bytes[0xc8U + slot * 24U + i]));
            EXPECT_EQ(recipe.processing_flags[i], std::to_integer<std::uint8_t>(bytes[0x140U + slot * 24U + i]));
        }
    }
    EXPECT_EQ(decoded->working_program_marker, 254);
    EXPECT_TRUE(std::ranges::equal(decoded->raw_bytes, bytes));
}

TEST(SystemGlobal, EveryScalarProjectionMatchesItsStoredCoordinateAndCompleteByteDomain) {
    for (const auto native : {true, false}) {
        auto file = global_file(native);
        auto bytes = global_bytes(file);
        for (unsigned seed = 0; seed < 256U; ++seed) {
            SCOPED_TRACE(native);
            SCOPED_TRACE(seed);
            for (std::size_t offset = 0; offset < bytes.size(); ++offset)
                bytes[offset] = static_cast<std::byte>((offset + seed) % 256U);
            const auto decoded = axk::decode_system_global(file);
            ASSERT_TRUE(decoded);
            const auto &p = decoded->parameters;
            auto raw = [&](std::size_t offset) { return std::to_integer<int>(bytes[offset]); };
            auto check = []<typename T>(const std::optional<T> &value, int stored, int minimum, int maximum) {
                const auto valid = stored >= minimum && stored <= maximum;
                EXPECT_EQ(value.has_value(), valid);
                if (valid && value)
                    EXPECT_EQ(static_cast<int>(*value), stored);
            };
            auto check_signed = [&](const auto &value, std::size_t offset, int minimum, int maximum) {
                const auto byte = raw(offset);
                check(value, byte >= 128 ? byte - 256 : byte, minimum, maximum);
            };
            check_signed(p.master_fine_tune, 0, -63, 63);
            check_signed(p.master_coarse_tune, 1, -127, 127);
            check_signed(p.master_transpose, 2, -127, 127);
            check(p.velocity_curve_selection, raw(3), 0, 17);
            check(p.basic_receive_channel_selection, raw(4), 0, native ? 15 : 31);
            check(p.stereo_to_assignable_selection, raw(5), 0, 5);
            constexpr std::array flag_members{&axk::SystemGlobalParameters::omni,
                                              &axk::SystemGlobalParameters::program_change_enabled,
                                              &axk::SystemGlobalParameters::wave_length_lock,
                                              &axk::SystemGlobalParameters::wave_auto_zero,
                                              &axk::SystemGlobalParameters::wave_auto_snap,
                                              &axk::SystemGlobalParameters::audition_with_easy_edit,
                                              &axk::SystemGlobalParameters::audition_with_effects,
                                              &axk::SystemGlobalParameters::play_and_load};
            for (std::size_t bit = 0; bit < flag_members.size(); ++bit)
                EXPECT_EQ(p.*flag_members[bit], (raw(6) & (1 << bit)) != 0);
            for (std::size_t i = 0; i < 4U; ++i) {
                check_signed(p.knob_transmit_channels[i], 7U + i, -1, native ? 16 : 32);
                check(p.knob_control_devices[i], raw(0xbU + i), 0, 120);
            }
            for (std::size_t i = 0; i < 6U; ++i) {
                check(p.function_key_transmit_channels[i], raw(0xfU + i), 0, native ? 16 : 32);
                check(p.function_key_notes[i], raw(0x15U + i), 0, 127);
                check(p.function_key_velocities[i], raw(0x1bU + i), 1, 127);
            }
            check(p.stereo_output_level_offset, raw(0x21), 0, 4);
            constexpr std::array<std::size_t, 4> frequency_offsets{0x22, 0x25, 0x28, 0x2b};
            constexpr std::array<int, 4> minimum_frequency{4, 4, 4, 28};
            constexpr std::array<int, 4> maximum_frequency{40, 40, 58, 58};
            for (std::size_t band = 0; band < 4U; ++band) {
                const auto offset = frequency_offsets[band];
                check(p.total_eq[band].frequency_selection, raw(offset), minimum_frequency[band],
                      maximum_frequency[band]);
                check(p.total_eq[band].gain_selection, raw(offset + 1U), 52, 76);
                if (band != 0U)
                    check(p.total_eq[band].width_selection, raw(offset + 2U), 10, 120);
                else
                    EXPECT_FALSE(p.total_eq[band].width_selection);
            }
            if (!native) {
                check(p.program_mode, raw(0x2e), 0, 1);
                check(p.remix_type_selection, raw(0x24) / 16, 0, 9);
                check(p.remix_variation_selection, raw(0x24) % 16, 0, 7);
                EXPECT_EQ(p.remix_auto_audition, (raw(0x2f) & 1) != 0);
                EXPECT_EQ(p.knob_midi_out, (raw(0x2f) & 2) != 0);
                EXPECT_EQ(p.function_key_midi_out, (raw(0x2f) & 4) != 0);
                for (std::size_t i = 0; i < 32U; ++i)
                    check(p.part_program_numbers[i], raw(0x30U + i), 1, 128);
                check(p.remix_zone_start, raw(0x1b8), 0, 7);
                check(p.remix_zone_end, raw(0x1b9), 1, 8);
                for (std::size_t i = 0; i < 5U; ++i)
                    check(p.assignable_output_level_offsets[i], raw(0x1baU + i), 0, 4);
                EXPECT_EQ(decoded->working_program_marker, raw(0x1bf));
            }
            EXPECT_TRUE(std::ranges::equal(decoded->raw_bytes, bytes));
        }
    }
}

TEST(SystemGlobal, ReadsStoredValuesWithoutApplyingCoupledEditsOrLoadNormalization) {
    auto file = global_file(false);
    auto bytes = global_bytes(file);
    bytes[6] = std::byte{0xff};
    bytes[0x24] = std::byte{0x97};
    bytes[0x2e] = std::byte{1};
    bytes[0x2f] = std::byte{0xff};
    bytes[0x1b8] = std::byte{7};
    bytes[0x1b9] = std::byte{1};
    const auto before = file;
    for (const auto revision : {0U, 1U}) {
        file.storage_revision = static_cast<std::uint8_t>(revision);
        const auto decoded = axk::decode_system_global(file);
        ASSERT_TRUE(decoded);
        const auto &p = decoded->parameters;
        EXPECT_EQ(p.program_mode, axk::ProgramMode::multi);
        EXPECT_EQ(p.omni, true);
        EXPECT_EQ(p.wave_length_lock, true);
        EXPECT_EQ(p.wave_auto_zero, true);
        EXPECT_EQ(p.wave_auto_snap, true);
        EXPECT_EQ(p.remix_type_selection, 9);
        EXPECT_EQ(p.remix_variation_selection, 7);
        EXPECT_EQ(p.knob_midi_out, true);
        EXPECT_EQ(p.function_key_midi_out, true);
        EXPECT_EQ(p.remix_zone_start, 7);
        EXPECT_EQ(p.remix_zone_end, 1);
        EXPECT_EQ(file.system_bulk_bytes, before.system_bulk_bytes);
    }
}

TEST(SystemGlobal, NativeRemixSelectorsUseNativeDomainsWithoutCurrentRecipeState) {
    auto file = global_file(true);
    for (unsigned raw = 0; raw < 256U; ++raw) {
        global_bytes(file)[0x24] = static_cast<std::byte>(raw);
        const auto before = file.system_bulk_bytes;
        const auto value = axk::decode_system_global(file);
        ASSERT_TRUE(value);
        const auto &p = value->parameters;
        EXPECT_EQ(p.remix_type_selection.has_value(), (raw >> 4U) <= 4U);
        EXPECT_EQ(p.remix_variation_selection.has_value(), (raw & 15U) <= 3U);
        if (p.remix_type_selection)
            EXPECT_EQ(*p.remix_type_selection, raw >> 4U);
        if (p.remix_variation_selection)
            EXPECT_EQ(*p.remix_variation_selection, raw & 15U);
        EXPECT_EQ(value->raw_bytes[0x24], static_cast<std::byte>(raw));
        EXPECT_EQ(file.system_bulk_bytes, before);
    }
    const auto decoded = axk::decode_system_global(file);
    ASSERT_TRUE(decoded);
    EXPECT_FALSE(decoded->registered_remix);
    EXPECT_FALSE(decoded->working_program_marker);
    for (const auto offset : decoded->parameters.assignable_output_level_offsets)
        EXPECT_FALSE(offset);
}

TEST(SystemGlobal, RejectsUnsupportedKindsRevisionsAndIncompleteOrOversizedBulk) {
    for (const auto native : {true, false}) {
        auto file = global_file(native);
        file.storage_revision = native ? 1U : 2U;
        EXPECT_FALSE(axk::decode_system_global(file));
        file.storage_revision = 0;
        file.system_bulk_bytes.push_back(std::byte{});
        EXPECT_FALSE(axk::decode_system_global(file));
        file.system_bulk_bytes.resize(0x10U);
        EXPECT_FALSE(axk::decode_system_global(file));
        file.system_bulk_bytes.clear();
        EXPECT_FALSE(axk::decode_system_global(file));
    }
    auto file = global_file(false);
    file.kind = static_cast<axk::SystemFileKind>(255);
    EXPECT_FALSE(axk::decode_system_global(file));
}
