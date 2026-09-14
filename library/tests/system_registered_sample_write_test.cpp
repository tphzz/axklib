#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/bytes.hpp"
#include "axklib/sample_parameter_codec.hpp"
#include "axklib/system_file.hpp"

namespace {

constexpr std::array models{axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000};

std::size_t sample_offset(axk::ASeriesModel model) { return model == axk::ASeriesModel::a3000 ? 0x1c8U : 0x3ecU; }

axk::DecodedSystemFile retained_sample(axk::ASeriesModel model, std::uint8_t revision = 0) {
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

template <typename Select>
void check_scalar(axk::ASeriesModel model, Select select, int minimum, int maximum, std::size_t offset,
                  int storage_bias = 0) {
    const auto file = retained_sample(model);
    for (unsigned raw = 0; raw < 256U; ++raw) {
        axk::SampleParameters patch;
        using Value = typename std::remove_cvref_t<decltype(select(patch))>::value_type;
        const auto value = std::bit_cast<Value>(static_cast<std::uint8_t>(raw));
        select(patch) = value;
        const auto result = axk::patch_system_registered_sample(file, patch, model);
        const bool valid = static_cast<int>(value) >= minimum && static_cast<int>(value) <= maximum;
        ASSERT_EQ(result.has_value(), valid) << offset << ':' << raw;
        if (valid) {
            auto expected = file;
            expected.system_bulk_bytes[sample_offset(model) + offset] =
                static_cast<std::byte>(static_cast<unsigned>(static_cast<int>(value) + storage_bias) & 0xffU);
            EXPECT_EQ(*result, expected) << offset << ':' << raw;
        }
    }
}

} // namespace

TEST(SystemRegisteredSampleWrite, EmptyAndCombinedPatchesPreserveAllModelsAndRevisions) {
    for (auto model : models) {
        for (std::uint8_t revision = 0; revision <= (model == axk::ASeriesModel::a3000 ? 0U : 1U); ++revision) {
            const auto file = retained_sample(model, revision);
            ASSERT_TRUE(axk::patch_system_registered_sample(file, {}, model));
            EXPECT_EQ(axk::patch_system_registered_sample(file, {}, model).value(), file);
            axk::SystemFilePatch patch;
            patch.registered_sample.level = 42;
            patch.registered_program.level = 30;
            auto expected = file;
            expected.system_bulk_bytes[sample_offset(model) + 0x6eU] = std::byte{42};
            expected.system_bulk_bytes[model == axk::ASeriesModel::a3000 ? 0x31fU : 0x617U] = std::byte{30};
            const auto combined = axk::patch_system_file(file, patch, model);
            ASSERT_TRUE(combined);
            EXPECT_EQ(*combined, expected);
        }
    }
}

TEST(SystemRegisteredSampleWrite, NativePortamentoAndVelocityCrossfadePreserveEveryOtherBit) {
    constexpr auto model = axk::ASeriesModel::a3000;
    for (unsigned flags = 0; flags < 256U; ++flags) {
        auto file = retained_sample(model);
        const auto offset = sample_offset(model) + 0x29U;
        file.system_bulk_bytes[offset] = static_cast<std::byte>(flags);
        for (unsigned selection = 1; selection <= 3U; ++selection) {
            for (unsigned values = 0; values < 4U; ++values) {
                axk::SampleParameters patch;
                auto expected_flags = flags;
                if ((selection & 1U) != 0U) {
                    patch.portamento_type = static_cast<std::uint8_t>(values & 1U);
                    expected_flags = (expected_flags & 0xfeU) | (values & 1U);
                }
                if ((selection & 2U) != 0U) {
                    patch.velocity_crossfade = (values & 2U) != 0U;
                    expected_flags = (expected_flags & 0xf7U) | ((values & 2U) << 2U);
                }
                ASSERT_TRUE(axk::detail::has_sample_parameter_values(patch));
                auto expected = file;
                expected.system_bulk_bytes[offset] = static_cast<std::byte>(expected_flags);
                const auto result = axk::patch_system_registered_sample(file, patch, model);
                ASSERT_TRUE(result) << result.error().message;
                EXPECT_EQ(*result, expected);
                const auto decoded = axk::decode_system_registered_sample(*result);
                ASSERT_TRUE(decoded);
                EXPECT_EQ(decoded->parameters.portamento_type, expected_flags & 1U);
                EXPECT_EQ(decoded->parameters.velocity_crossfade, (expected_flags & 8U) != 0U);
                EXPECT_FALSE(decoded->parameters.velocity_xfade_high);
                EXPECT_FALSE(decoded->parameters.velocity_xfade_low);
            }
        }
    }
}

TEST(SystemRegisteredSampleWrite, NativePackedFieldsRejectWrongGenerationAndExtendedModes) {
    constexpr auto model = axk::ASeriesModel::a3000;
    const auto file = retained_sample(model);
    for (unsigned type = 2; type < 256U; ++type) {
        axk::SystemFilePatch patch;
        patch.registered_program.level = 43;
        patch.registered_sample.portamento_type = static_cast<std::uint8_t>(type);
        EXPECT_FALSE(axk::patch_system_file(file, patch, model));
    }
    for (const bool enabled : {false, true}) {
        axk::SampleParameters patch;
        patch.velocity_crossfade = enabled;
        EXPECT_TRUE(axk::detail::has_sample_parameter_values(patch));
        EXPECT_FALSE(axk::detail::validate_sample_parameters(patch));
        axk::SampleParameters base;
        base.velocity_crossfade = !enabled;
        axk::detail::merge_sample_parameters(base, patch);
        EXPECT_EQ(base.velocity_crossfade, enabled);
        for (auto current : {axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
            EXPECT_FALSE(axk::patch_system_registered_sample(retained_sample(current), patch, current));
        }
    }
    EXPECT_EQ(file, retained_sample(model));
}

TEST(SystemRegisteredSampleWrite, IndependentScalarDomainsAndOffsetsAreGenerationSpecific) {
    for (auto model : models) {
        const bool native = model == axk::ASeriesModel::a3000;
#define CHECK(member, minimum, maximum, offset)                                                                        \
    check_scalar(model, [](auto &p) -> auto & { return p.member; }, minimum, maximum, offset)
        CHECK(midi_receive_channel, 0, 16, 0x2a);
        CHECK(pitch_bend_type, 0, native ? 13 : 12, 0x2b);
        CHECK(pitch_bend_range, 0, 24, 0x2c);
        CHECK(coarse_tune, native ? -127 : -64, native ? 127 : 63, 0x2d);
        CHECK(wave_start_velocity_sensitivity, -63, 63, 0x60);
        CHECK(filter_type, 0, 16, 0x61);
        CHECK(filter_cutoff, 0, 127, 0x62);
        CHECK(filter_q_width, 0, 31, 0x63);
        CHECK(filter_scaling_cutoff1, -127, 127, 0x66);
        CHECK(filter_scaling_cutoff2, -127, 127, 0x67);
        CHECK(filter_velocity_to_cutoff, -63, 68, 0x68);
        CHECK(filter_velocity_to_q_width, -63, 68, 0x69);
        CHECK(expand_width, -63, 63, 0x6c);
        CHECK(random_pitch, 0, 63, 0x6d);
        CHECK(level, 0, 127, 0x6e);
        CHECK(pan, -64, 63, 0x6f);
        CHECK(velocity_low_limit, 0, 127, 0x70);
        CHECK(velocity_offset, -127, 127, 0x71);
        CHECK(level_scaling_level1, 0, 127, 0x76);
        CHECK(level_scaling_level2, 0, 127, 0x77);
        CHECK(velocity_sensitivity, -127, 127, 0x78);
        CHECK(alternate_group, 0, 16, 0x79);
        CHECK(filter_cutoff_distance, -63, 63, 0x7d);
        CHECK(feg.attack_rate, 0, 127, 0x7e);
        CHECK(feg.decay_rate, 0, 127, 0x7f);
        CHECK(feg.release_rate, 0, 127, 0x80);
        CHECK(feg.init_level, -127, 127, 0x81);
        CHECK(feg.attack_level, -127, 127, 0x82);
        CHECK(feg.sustain_level, -127, 127, 0x83);
        CHECK(feg.release_level, -127, 127, 0x84);
        CHECK(feg.rate_key_scaling, -7, 7, 0x85);
        CHECK(feg.rate_velocity_sensitivity, -63, 63, 0x86);
        CHECK(feg.attack_level_velocity_sensitivity, -63, 63, 0x87);
        CHECK(feg.level_velocity_sensitivity, -63, 63, 0x88);
        CHECK(peg.attack_rate, 0, 127, 0x89);
        CHECK(peg.decay_rate, 0, 127, 0x8a);
        CHECK(peg.release_rate, 0, 127, 0x8b);
        CHECK(peg.init_level, -127, 127, 0x8c);
        CHECK(peg.attack_level, -127, 127, 0x8d);
        CHECK(peg.sustain_level, -127, 127, 0x8e);
        CHECK(peg.release_level, -127, 127, 0x8f);
        CHECK(peg.rate_key_scaling, -7, 7, 0x90);
        CHECK(peg.rate_velocity_sensitivity, -63, 63, 0x91);
        CHECK(peg.level_velocity_sensitivity, -63, 63, 0x92);
        CHECK(peg.range, -63, 63, 0x93);
        CHECK(aeg.attack_rate, 0, 127, 0x94);
        CHECK(aeg.decay_rate, 0, 127, 0x95);
        CHECK(aeg.release_rate, 0, 127, 0x96);
        CHECK(aeg.sustain_level, 0, 127, 0x99);
        CHECK(aeg.attack_mode, 0, native ? 1 : 2, 0x9b);
        CHECK(aeg.rate_key_scaling, -7, 7, 0x9c);
        CHECK(aeg.rate_velocity_sensitivity, -63, 63, 0x9d);
        CHECK(lfo.wave, 0, 3, 0x9e);
        check_scalar(model, [](auto &p) -> auto & { return p.lfo.speed; }, 1, 128, 0x9f, -1);
        CHECK(lfo.delay_time, 0, 127, 0xa0);
        CHECK(lfo.cutoff_mod_depth, 0, 127, 0xa2);
        CHECK(lfo.pitch_mod_depth, 0, 127, 0xa3);
        CHECK(lfo.amp_mod_depth, 0, 127, 0xa4);
        CHECK(filter_gain, -31, 31, 0xa9);
        CHECK(output1_destination, 0, native ? 4 : 12, native ? 0xa5U : 0xd6U);
        CHECK(output1_level, 0, 127, native ? 0xa6U : 0xd7U);
        CHECK(output2_destination, 0, native ? 5 : 12, native ? 0xa7U : 0xd8U);
        CHECK(output2_level, 0, 127, native ? 0xa8U : 0xd9U);
        if (!native) {
            CHECK(velocity_xfade_high, 0, 127, 0xd4);
            CHECK(velocity_xfade_low, 0, 127, 0xd5);
            CHECK(portamento_rate, 1, 127, 0xdb);
            CHECK(portamento_time, 1, 127, 0xdc);
        }
#undef CHECK
    }
}

TEST(SystemRegisteredSampleWrite, PackedEditsPreserveEveryUnownedBit) {
    for (auto model : models) {
        for (unsigned raw = 0; raw < 256U; ++raw) {
            auto file = retained_sample(model);
            const auto base = sample_offset(model);
            file.system_bulk_bytes[base + 0x29U] = static_cast<std::byte>(raw);
            file.system_bulk_bytes[base + 0xa1U] = static_cast<std::byte>(raw);
            axk::SampleParameters patch;
            patch.fixed_pitch = true;
            patch.mono_mode = false;
            patch.key_crossfade = true;
            patch.lfo.key_on_sync = false;
            patch.lfo.cutoff_mod_phase_invert = true;
            patch.lfo.pitch_mod_phase_invert = false;
            auto expected = file;
            expected.system_bulk_bytes[base + 0x29U] = static_cast<std::byte>((raw | 0x14U) & 0xfdU);
            expected.system_bulk_bytes[base + 0xa1U] = static_cast<std::byte>((raw | 2U) & 0xfaU);
            const auto result = axk::patch_system_registered_sample(file, patch, model);
            ASSERT_TRUE(result);
            EXPECT_EQ(*result, expected);
        }
    }
}

TEST(SystemRegisteredSampleWrite, ControllersUseNativePrefixOrChangedCurrentWholeRecordProjection) {
    for (auto model : models) {
        const bool native = model == axk::ASeriesModel::a3000;
        for (std::size_t slot = 0; slot < 6U; ++slot) {
            for (unsigned function = 0; function < 256U; ++function) {
                const auto file = retained_sample(model);
                const auto base = sample_offset(model);
                const auto offset = base + (native ? 0U : 0xbcU) + slot * 4U;
                axk::SampleParameters patch;
                patch.controls[slot].function = static_cast<std::uint8_t>(function);
                const auto result = axk::patch_system_registered_sample(file, patch, model);
                ASSERT_EQ(result.has_value(), function <= (native ? 21U : 36U));
                if (!result)
                    continue;
                auto expected = file;
                expected.system_bulk_bytes[offset + 1U] = static_cast<std::byte>(function);
                if (!native) {
                    for (std::size_t i = 0; i < 4U; ++i)
                        expected.system_bulk_bytes[base + slot * 4U + i] = expected.system_bulk_bytes[offset + i];
                    if (function > 21U)
                        expected.system_bulk_bytes[base + slot * 4U + 1U] = std::byte{};
                }
                EXPECT_EQ(*result, expected);
                expected.system_bulk_bytes[base + (native ? 0x18U : slot * 4U)] = std::byte{0x55};
                EXPECT_EQ(axk::patch_system_registered_sample(expected, patch, model).value(), expected);
            }
        }
    }
}

TEST(SystemRegisteredSampleWrite, NativeEqIgnoresUnownedTypeBitsAndRegeneratesOnlyOnEqEdits) {
    for (auto model : models) {
        auto file = retained_sample(model);
        const auto base = sample_offset(model);
        file.system_bulk_bytes[base + 0x29U] = model == axk::ASeriesModel::a3000 ? std::byte{0xff} : std::byte{0x3f};
        axk::SampleParameters patch;
        patch.sample_eq_frequency = 26;
        patch.sample_eq_gain_db = 0;
        patch.sample_eq_width_tenths = 10;
        const auto result = axk::patch_system_registered_sample(file, patch, model);
        ASSERT_TRUE(result);
        const auto decoded = axk::decode_system_registered_sample(*result).value();
        // Retained flat Peak/Dip reference, Q13 order b1,b2,b0,-a1,-a2.
        EXPECT_EQ(decoded.eq_coefficients, (std::array<std::int16_t, 5>{-15904, 7738, 8192, 15904, -7738}));
        auto expected = file;
        expected.system_bulk_bytes[base + 0x7aU] = std::byte{26};
        expected.system_bulk_bytes[base + 0x7bU] = std::byte{64};
        expected.system_bulk_bytes[base + 0x7cU] = std::byte{10};
        for (std::size_t i = 0; i < 10U; ++i)
            expected.system_bulk_bytes[base + 0xaaU + i] = result->system_bulk_bytes[base + 0xaaU + i];
        EXPECT_EQ(*result, expected);
        expected.system_bulk_bytes[base + 0xaaU] = std::byte{0x55};
        axk::SampleParameters unrelated;
        unrelated.level = 42;
        const auto changed = axk::patch_system_registered_sample(expected, unrelated, model);
        ASSERT_TRUE(changed);
        expected.system_bulk_bytes[base + 0x6eU] = std::byte{42};
        EXPECT_EQ(*changed, expected);
        axk::SampleParameters partial;
        partial.sample_eq_gain_db = -12;
        EXPECT_FALSE(axk::patch_system_registered_sample(file, partial, model));
        EXPECT_TRUE(axk::patch_system_registered_sample(*result, partial, model));
    }
}

TEST(SystemRegisteredSampleWrite, CoupledScalarEditsValidateRetainedPartnersNotUnrelatedFields) {
    for (auto model : models) {
        auto file = retained_sample(model);
        const auto base = sample_offset(model);
        const auto exercise = [&](auto select_low, auto select_high, std::size_t low, std::size_t high) {
            file.system_bulk_bytes[base + low] = std::byte{40};
            file.system_bulk_bytes[base + high] = std::byte{60};
            axk::SampleParameters patch;
            select_low(patch) = 61;
            EXPECT_FALSE(axk::patch_system_registered_sample(file, patch, model));
            select_high(patch) = 80;
            auto expected = file;
            expected.system_bulk_bytes[base + low] = std::byte{61};
            expected.system_bulk_bytes[base + high] = std::byte{80};
            EXPECT_EQ(axk::patch_system_registered_sample(file, patch, model).value(), expected);
            file.system_bulk_bytes[base + high] = std::byte{255};
            select_high(patch).reset();
            EXPECT_FALSE(axk::patch_system_registered_sample(file, patch, model));
        };
        exercise([](auto &p) -> auto & { return p.filter_scaling_break1; },
                 [](auto &p) -> auto & { return p.filter_scaling_break2; }, 0x64, 0x65);
        exercise([](auto &p) -> auto & { return p.level_scaling_break1; },
                 [](auto &p) -> auto & { return p.level_scaling_break2; }, 0x74, 0x75);
        exercise([](auto &p) -> auto & { return p.velocity_low; }, [](auto &p) -> auto & { return p.velocity_high; },
                 0x73, 0x72);
    }
}

TEST(SystemRegisteredSampleWrite, UnsupportedContextAndNativeExtensionsFailAtomically) {
    for (auto model : models) {
        const auto file = retained_sample(model);
        const auto before = axk::encode_system_file(file).value();
        const auto reject = [&](axk::SampleParameters patch, const std::string &reason) {
            patch.level = 42;
            const auto result = axk::patch_system_registered_sample(file, patch, model);
            ASSERT_FALSE(result);
            EXPECT_NE(result.error().message.find(reason), std::string::npos) << result.error().message;
            axk::SystemFilePatch combined;
            combined.registered_program.level = 30;
            combined.registered_sample = patch;
            EXPECT_FALSE(axk::patch_system_file(file, combined, model));
            EXPECT_EQ(axk::encode_system_file(file).value(), before);
        };
        axk::SampleParameters patch;
        patch.root_key = 60;
        reject(patch, "pitch");
        patch = {};
        patch.fine_tune_cents = 1;
        reject(patch, "pitch");
        patch = {};
        patch.loop_start_frame = 1;
        reject(patch, "window");
        patch = {};
        patch.loop_length_frames = 1;
        reject(patch, "window");
        patch = {};
        patch.expand_dephase = 1;
        reject(patch, "topology");
        if (model == axk::ASeriesModel::a3000) {
            patch = {};
            patch.sample_eq_type = 0;
            reject(patch, "A3000");
            patch = {};
            patch.portamento_rate = 1;
            reject(patch, "A3000");
            patch = {};
            patch.velocity_xfade_low = 0;
            reject(patch, "A3000");
        }
        auto malformed = file;
        malformed.system_bulk_bytes.pop_back();
        EXPECT_FALSE(axk::patch_system_registered_sample(malformed, {}, model));
        EXPECT_FALSE(axk::patch_system_registered_sample(file, {}, static_cast<axk::ASeriesModel>(255)));
        const auto other = model == axk::ASeriesModel::a3000 ? axk::ASeriesModel::a4000 : axk::ASeriesModel::a3000;
        EXPECT_FALSE(axk::patch_system_registered_sample(file, {}, other));
    }
}

TEST(SystemRegisteredSampleWrite, LoopModeIsIndependentOfRetainedWindowsAndCaches) {
    for (auto model : models) {
        const auto file = retained_sample(model);
        for (unsigned raw = 0; raw < 256U; ++raw) {
            axk::SystemFilePatch patch;
            patch.registered_sample.loop_mode = static_cast<axk::AudioSamplerLoopMode>(raw);
            const auto result = axk::patch_system_file(file, patch, model);
            ASSERT_EQ(result.has_value(), raw <= 5U) << raw;
            if (result) {
                auto expected = file;
                expected.system_bulk_bytes[sample_offset(model) + 0x3dU] = static_cast<std::byte>(raw);
                EXPECT_EQ(*result, expected);
            }
        }
    }
}

TEST(SystemRegisteredSampleWrite, ExplicitLoopIntervalMirrorsBothLanesAndRepairsOnlyItsOwnCache) {
    for (auto model : models) {
        const auto base = sample_offset(model);
        for (std::uint8_t revision = 0; revision <= (model == axk::ASeriesModel::a3000 ? 0U : 1U); ++revision) {
            for (unsigned flags : {0U, 4U}) {
                auto file = retained_sample(model, revision);
                file.system_bulk_bytes[0x16] =
                    (file.system_bulk_bytes[0x16] & std::byte{0xfb}) | static_cast<std::byte>(flags);
                axk::ByteWriter stored{file.system_bulk_bytes};
                ASSERT_TRUE(stored.write_be32(base + 0x40, 100));
                ASSERT_TRUE(stored.write_be32(base + 0x48, 600));
                ASSERT_TRUE(stored.write_be32(base + 0xb4, 700));
                ASSERT_TRUE(stored.write_be32(base + 0x50, 200));
                ASSERT_TRUE(stored.write_be32(base + 0x58, 300));
                for (unsigned selection = 1; selection <= 3; ++selection) {
                    for (unsigned mode = 0; mode <= 5; ++mode) {
                        axk::SystemFilePatch patch;
                        patch.registered_program.level = 30;
                        patch.registered_sample.loop_mode = static_cast<axk::AudioSamplerLoopMode>(mode);
                        if ((selection & 1U) != 0)
                            patch.registered_sample.loop_start_frame = 250;
                        if ((selection & 2U) != 0)
                            patch.registered_sample.loop_length_frames = 200;
                        const auto start = patch.registered_sample.loop_start_frame.value_or(200);
                        const auto length = patch.registered_sample.loop_length_frames.value_or(300);
                        auto expected = file;
                        axk::ByteWriter writer{expected.system_bulk_bytes};
                        ASSERT_TRUE(writer.write_be32(base + 0x50, start));
                        ASSERT_TRUE(writer.write_be32(base + 0x54, start));
                        ASSERT_TRUE(writer.write_be32(base + 0x58, length));
                        ASSERT_TRUE(writer.write_be32(base + 0x5c, length));
                        ASSERT_TRUE(writer.write_be32(base + 0xb8, start + length));
                        expected.system_bulk_bytes[base + 0x3d] = static_cast<std::byte>(mode);
                        expected.system_bulk_bytes[model == axk::ASeriesModel::a3000 ? 0x31fU : 0x617U] = std::byte{30};
                        const auto result = axk::patch_system_file(file, patch, model);
                        ASSERT_TRUE(result) << result.error().message;
                        EXPECT_EQ(*result, expected);
                        EXPECT_EQ(axk::patch_system_file(*result, patch, model).value(), expected);
                    }
                }
            }
        }
    }
}

TEST(SystemRegisteredSampleWrite, LoopIntervalBoundsIncludeEmptyWindowsAndRejectOverflowAtomically) {
    for (auto model : models) {
        const auto base = sample_offset(model);
        auto file = retained_sample(model);
        axk::ByteWriter writer{file.system_bulk_bytes};
        ASSERT_TRUE(writer.write_be32(base + 0x40, 100));
        ASSERT_TRUE(writer.write_be32(base + 0x48, 5));
        ASSERT_TRUE(writer.write_be32(base + 0xb4, 105));
        const auto before = file;
        for (std::uint32_t start = 99; start <= 106; ++start) {
            for (std::uint32_t length = 0; length <= 7; ++length) {
                axk::SystemFilePatch patch;
                patch.registered_program.level = 30;
                patch.registered_sample.loop_start_frame = start;
                patch.registered_sample.loop_length_frames = length;
                const auto result = axk::patch_system_file(file, patch, model);
                ASSERT_EQ(result.has_value(), start >= 100 && start + length <= 105) << start << ':' << length;
                EXPECT_EQ(file, before);
            }
        }
        axk::SampleParameters patch;
        patch.loop_start_frame = std::numeric_limits<std::uint32_t>::max();
        patch.loop_length_frames = 1;
        EXPECT_FALSE(axk::patch_system_registered_sample(file, patch, model));
        ASSERT_TRUE(writer.write_be32(base + 0x40, std::numeric_limits<std::uint32_t>::max()));
        ASSERT_TRUE(writer.write_be32(base + 0x48, 0));
        ASSERT_TRUE(writer.write_be32(base + 0xb4, std::numeric_limits<std::uint32_t>::max()));
        patch.loop_length_frames = 0;
        EXPECT_TRUE(axk::patch_system_registered_sample(file, patch, model));
        ASSERT_TRUE(writer.write_be32(base + 0x48, 1));
        ASSERT_TRUE(writer.write_be32(base + 0xb4, 0));
        EXPECT_FALSE(axk::patch_system_registered_sample(file, patch, model));
        ASSERT_TRUE(writer.write_be32(base + 0x40, 100));
        ASSERT_TRUE(writer.write_be32(base + 0x48, 5));
        patch.loop_start_frame = 100;
        EXPECT_FALSE(axk::patch_system_registered_sample(file, patch, model));
        // A malformed wave cache is unrelated to a level-only edit.
        patch = {};
        patch.level = 42;
        EXPECT_TRUE(axk::patch_system_registered_sample(file, patch, model));
    }
}

TEST(SystemRegisteredSampleWrite, ScalingBreakpointsMustBeDistinctButVelocityEndpointsMayCoincide) {
    for (auto model : models) {
        const auto base = sample_offset(model);
        const auto check_pair = [&](auto select_low, auto select_high, std::size_t low_offset, std::size_t high_offset,
                                    bool strict) {
            for (unsigned low = 0; low < 128U; ++low) {
                for (unsigned high = 0; high < 128U; ++high) {
                    auto file = retained_sample(model);
                    file.system_bulk_bytes[base + low_offset] = static_cast<std::byte>(low);
                    file.system_bulk_bytes[base + high_offset] = static_cast<std::byte>(high);
                    for (unsigned request = 1; request <= 3U; ++request) {
                        axk::SampleParameters patch;
                        if ((request & 1U) != 0)
                            select_low(patch) = static_cast<std::uint8_t>(low);
                        if ((request & 2U) != 0)
                            select_high(patch) = static_cast<std::uint8_t>(high);
                        const auto result = axk::patch_system_registered_sample(file, patch, model);
                        ASSERT_EQ(result.has_value(), strict ? low < high : low <= high)
                            << low_offset << ':' << low << ':' << high << ':' << request;
                        if (result)
                            EXPECT_EQ(*result, file);
                    }
                }
            }
        };
        check_pair([](auto &p) -> auto & { return p.filter_scaling_break1; },
                   [](auto &p) -> auto & { return p.filter_scaling_break2; }, 0x64, 0x65, true);
        check_pair([](auto &p) -> auto & { return p.level_scaling_break1; },
                   [](auto &p) -> auto & { return p.level_scaling_break2; }, 0x74, 0x75, true);
        check_pair([](auto &p) -> auto & { return p.velocity_low; }, [](auto &p) -> auto & { return p.velocity_high; },
                   0x73, 0x72, false);
    }
}

TEST(SystemRegisteredSampleWrite, SpecialKeysRepairOnlyImplicitConflictsUsingTheActualRoot) {
    for (auto model : models) {
        const auto base = sample_offset(model);
        for (const auto root : {0U, 17U, 60U, 100U, 127U}) {
            for (unsigned raw = 0; raw < 256U; ++raw) {
                auto file = retained_sample(model);
                file.system_bulk_bytes[base + 0x2eU] = static_cast<std::byte>(root);
                file.system_bulk_bytes[base + 0x3bU] = static_cast<std::byte>(raw);
                file.system_bulk_bytes[base + 0x3aU] = std::byte{127};
                axk::SampleParameters patch;
                patch.key_high = axk::sampler_original_key_high_limit;
                auto result = axk::patch_system_registered_sample(file, patch, model);
                ASSERT_EQ(result.has_value(), raw <= 127U || raw == 255U);
                if (result) {
                    auto expected = file;
                    expected.system_bulk_bytes[base + 0x3aU] = std::byte{128};
                    if (raw <= 127U && raw > root)
                        expected.system_bulk_bytes[base + 0x3bU] = static_cast<std::byte>(root);
                    EXPECT_EQ(*result, expected);
                }
                file.system_bulk_bytes[base + 0x3aU] = static_cast<std::byte>(raw);
                file.system_bulk_bytes[base + 0x3bU] = std::byte{};
                patch = {};
                patch.key_low = axk::sampler_original_key_low_limit;
                result = axk::patch_system_registered_sample(file, patch, model);
                ASSERT_EQ(result.has_value(), raw <= 128U);
                if (result) {
                    auto expected = file;
                    expected.system_bulk_bytes[base + 0x3bU] = std::byte{255};
                    if (raw < root)
                        expected.system_bulk_bytes[base + 0x3aU] = static_cast<std::byte>(root);
                    EXPECT_EQ(*result, expected);
                }
            }
        }
        auto file = retained_sample(model);
        file.system_bulk_bytes[base + 0x2eU] = std::byte{100};
        axk::SampleParameters patch;
        patch.key_high = 128;
        patch.key_low = 101;
        EXPECT_FALSE(axk::patch_system_registered_sample(file, patch, model));
        patch.key_low = 80;
        EXPECT_TRUE(axk::patch_system_registered_sample(file, patch, model));
        patch.key_high = 99;
        patch.key_low = 255;
        EXPECT_FALSE(axk::patch_system_registered_sample(file, patch, model));
        patch.key_high = 120;
        EXPECT_TRUE(axk::patch_system_registered_sample(file, patch, model));
        file.system_bulk_bytes[base + 0x2eU] = std::byte{255};
        EXPECT_FALSE(axk::patch_system_registered_sample(file, patch, model));
        patch.key_low = 0;
        EXPECT_TRUE(axk::patch_system_registered_sample(file, patch, model));
    }
}

TEST(SystemRegisteredSampleWrite, ControllerDeviceTypeAndRangePreserveGenerationDomainsAndWholeRecords) {
    for (auto model : models) {
        const bool native = model == axk::ASeriesModel::a3000;
        const auto base = sample_offset(model);
        const auto file = retained_sample(model);
        for (std::size_t slot = 0; slot < 6U; ++slot) {
            const auto offset = base + (native ? 0U : 0xbcU) + slot * 4U;
            const auto exercise = [&](auto select, int minimum, int maximum, std::size_t field) {
                for (unsigned raw = 0; raw < 256U; ++raw) {
                    axk::SampleParameters patch;
                    auto &leaf = select(patch.controls[slot]);
                    using Value = typename std::remove_cvref_t<decltype(leaf)>::value_type;
                    const auto value = std::bit_cast<Value>(static_cast<std::uint8_t>(raw));
                    leaf = value;
                    const auto result = axk::patch_system_registered_sample(file, patch, model);
                    const bool valid = static_cast<int>(value) >= minimum && static_cast<int>(value) <= maximum;
                    ASSERT_EQ(result.has_value(), valid) << slot << ':' << field << ':' << raw;
                    if (!result)
                        continue;
                    auto expected = file;
                    expected.system_bulk_bytes[offset + field] = static_cast<std::byte>(raw);
                    if (!native) {
                        for (std::size_t i = 0; i < 4U; ++i)
                            expected.system_bulk_bytes[base + slot * 4U + i] = expected.system_bulk_bytes[offset + i];
                        expected.system_bulk_bytes[base + slot * 4U + 1U] = std::byte{};
                    }
                    EXPECT_EQ(*result, expected);
                }
            };
            exercise([](auto &p) -> auto & { return p.device; }, 0, native ? 125 : 126, 0);
            exercise([](auto &p) -> auto & { return p.type; }, 0, 3, 2);
            exercise([](auto &p) -> auto & { return p.range; }, -63, 63, 3);
        }
    }
}

TEST(SystemRegisteredSampleWrite, CurrentPortamentoUpdatesOnlyItsByteAndLegacyEnableBit) {
    for (auto model : {axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        const auto base = sample_offset(model);
        for (unsigned flags = 0; flags < 256U; ++flags) {
            auto file = retained_sample(model);
            file.system_bulk_bytes[base + 0x29U] = static_cast<std::byte>(flags);
            for (unsigned type = 0; type < 256U; ++type) {
                axk::SampleParameters patch;
                patch.portamento_type = static_cast<std::uint8_t>(type);
                const auto result = axk::patch_system_registered_sample(file, patch, model);
                ASSERT_EQ(result.has_value(), type <= 5U);
                if (result) {
                    auto expected = file;
                    expected.system_bulk_bytes[base + 0xdaU] = static_cast<std::byte>(type);
                    expected.system_bulk_bytes[base + 0x29U] =
                        static_cast<std::byte>(type == 1U ? flags | 1U : flags & 0xfeU);
                    EXPECT_EQ(*result, expected);
                }
            }
        }
    }
}

TEST(SystemRegisteredSampleWrite, LoopTempoUsesBigEndianStorageWithoutChangingWindows) {
    for (auto model : models) {
        const auto file = retained_sample(model);
        for (const auto value : {0U, 7999U, 8000U, 8001U, 12345U, 15998U, 15999U, 16000U, 65535U}) {
            axk::SampleParameters patch;
            patch.loop_tempo_hundredths = static_cast<std::uint16_t>(value);
            const auto result = axk::patch_system_registered_sample(file, patch, model);
            ASSERT_EQ(result.has_value(), value >= 8000U && value <= 15999U);
            if (result) {
                auto expected = file;
                axk::ByteWriter writer{expected.system_bulk_bytes};
                ASSERT_TRUE(writer.write_be16(sample_offset(model) + 0x3eU, static_cast<std::uint16_t>(value)));
                EXPECT_EQ(*result, expected);
            }
        }
    }
}

TEST(SystemRegisteredSampleWrite, SharedEqReferencesApplyToNativePeakDipAndCurrentTypes) {
    std::ifstream source{std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/sample-eq-vectors.json"};
    ASSERT_TRUE(source);
    const auto references = nlohmann::json::parse(source);
    std::size_t native_count{};
    for (auto model : models) {
        const bool native = model == axk::ASeriesModel::a3000;
        for (const auto &reference : references) {
            const auto type = reference.at("type").get<std::uint8_t>();
            if (native && type != 0U)
                continue;
            native_count += native ? 1U : 0U;
            SCOPED_TRACE(reference.at("name").get<std::string>());
            auto file = retained_sample(model);
            const auto base = sample_offset(model);
            file.system_bulk_bytes[base + 0x29U] = std::byte{0xff};
            axk::SampleParameters patch;
            if (!native)
                patch.sample_eq_type = type;
            patch.sample_eq_frequency = reference.at("frequency").get<std::uint8_t>();
            patch.sample_eq_gain_db = reference.at("gain_db").get<std::int8_t>();
            patch.sample_eq_width_tenths = reference.at("width_tenths").get<std::uint8_t>();
            const auto result = axk::patch_system_registered_sample(file, patch, model);
            ASSERT_TRUE(result);
            const auto values = axk::decode_system_registered_sample(*result).value().eq_coefficients;
            const auto expected = reference.at("coefficients_q13").get<std::array<std::int32_t, 5>>();
            const auto tolerance = reference.at("tolerance_q13").get<std::int32_t>();
            ASSERT_GE(tolerance, 0);
            ASSERT_LE(tolerance, 1);
            for (std::size_t i = 0; i < expected.size(); ++i)
                EXPECT_LE(std::abs(static_cast<std::int32_t>(values[i]) - expected[i]), tolerance);
        }
    }
    EXPECT_EQ(native_count, 6U);
}
