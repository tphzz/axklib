#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/effects.hpp"
#include "axklib/system_file.hpp"

namespace {

axk::DecodedSystemFile retained_file(axk::ASeriesModel model, std::uint8_t revision = 0) {
    const auto native = model == axk::ASeriesModel::a3000;
    const auto body_size = native ? 0x400U : 0x1000U;
    std::vector<std::byte> bytes(axk::current_record_envelope_size + body_size);
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::byte>((index * 73U + 19U) & 255U);
    axk::ByteWriter writer{bytes};
    EXPECT_TRUE(writer.write_ascii_field(0, 12, "FSFSDEV3SPLX", std::byte{}));
    EXPECT_TRUE(writer.write_ascii_field(12, 4, "PRF3", std::byte{}));
    EXPECT_TRUE(writer.write_be32(0x14, 4));
    EXPECT_TRUE(writer.write_be32(0x18, 0x36));
    EXPECT_TRUE(writer.write_be32(0x1c, body_size + 8U));
    EXPECT_TRUE(writer.write_be32(0x30, native ? 0x21520531U : 0xdeadfaceU));
    bytes[0x3e] = static_cast<std::byte>(revision);
    auto result = axk::decode_system_file(
        native ? axk::SystemFileKind::a3000_system : axk::SystemFileKind::a4000_a5000_system2, bytes);
    EXPECT_TRUE(result);
    return result.value();
}

std::size_t effect_offset(axk::ASeriesModel model, std::size_t slot) {
    return (model == axk::ASeriesModel::a3000 ? 0x110U : 0x320U) + slot * 40U;
}

constexpr std::array models{axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000};

axk::DecodedSystemFile configured_file(axk::ASeriesModel model) {
    auto file = retained_file(model, model == axk::ASeriesModel::a5000 ? 1 : 0);
    constexpr std::array<std::uint8_t, 27> values{2,   1, 0, 0, 1,   1,   0,    20,   7, 0, 0, 127, 60, 0,
                                                  255, 1, 1, 0, 100, 100, 0x2e, 0xe0, 4, 0, 0, 36,  0};
    const auto offset = effect_offset(model, 0) + 120U;
    std::ranges::transform(values, file.system_bulk_bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                           [](auto value) { return static_cast<std::byte>(value); });
    if (model != axk::ASeriesModel::a3000)
        file.system_bulk_bytes[offset + 0x33U] = std::byte{0};
    return file;
}

} // namespace

TEST(SystemRecordingWrite, EmptyAndScalarPatchesPreserveEveryOtherByte) {
    for (const auto model : models) {
        auto source = retained_file(model);
        std::array<axk::ProgramEffectParameters, 3> patches;
        const auto empty = axk::patch_system_recording_effects(source, patches, model);
        ASSERT_TRUE(empty);
        EXPECT_EQ(*empty, source);
        const auto original = source;
        auto expected = source;
        for (std::size_t slot = 0; slot < patches.size(); ++slot) {
            auto &p = patches[slot];
            p.enabled = false;
            p.input_level = 0;
            p.output_level = 127;
            p.pan = -63;
            p.destination = 5;
            p.width = -126;
            const std::array header{std::byte{0},   std::byte{0}, std::byte{127},
                                    std::byte{193}, std::byte{5}, std::byte{130}};
            std::ranges::copy(header, expected.system_bulk_bytes.begin() +
                                          static_cast<std::ptrdiff_t>(effect_offset(model, slot)));
        }
        const auto result = axk::patch_system_recording_effects(source, patches, model);
        ASSERT_TRUE(result);
        EXPECT_EQ(*result, expected);
        EXPECT_EQ(source, original);
        EXPECT_TRUE(axk::encode_system_file(*result));
    }
}

TEST(SystemRecordingConfigurationWrite, IndependentPreferencesPreserveTheCompleteRetainedRecord) {
    for (const auto model : models) {
        const auto source = retained_file(model);
        const auto empty = axk::patch_system_recording_configuration(source, {}, model);
        ASSERT_TRUE(empty);
        EXPECT_EQ(*empty, source);
        axk::SystemRecordingParameters patch;
        patch.pre_trigger_time = 5;
        patch.click_tempo_hundredths = 15999;
        patch.external_scsi_id = -1;
        patch.external_track = 99;
        patch.monitor_enabled = false;
        auto expected = source;
        const auto offset = effect_offset(model, 0) + 120U;
        expected.system_bulk_bytes[offset + 4U] = std::byte{5};
        expected.system_bulk_bytes[offset + 14U] = std::byte{255};
        expected.system_bulk_bytes[offset + 15U] = std::byte{99};
        expected.system_bulk_bytes[offset + 23U] = std::byte{0};
        ASSERT_TRUE(axk::ByteWriter{expected.system_bulk_bytes}.write_be16(offset + 20U, 15999));
        const auto result = axk::patch_system_recording_configuration(source, patch, model);
        ASSERT_TRUE(result);
        EXPECT_EQ(*result, expected);
        EXPECT_TRUE(axk::encode_system_file(*result));
        const auto decoded = axk::decode_system_recording(*result);
        ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded->parameters.click_tempo_hundredths, 15999);
        EXPECT_EQ(decoded->parameters.external_scsi_id, -1);
        EXPECT_EQ(decoded->parameters.monitor_enabled, false);
    }
}

TEST(SystemRecordingWrite, CombinedPatchValidatesBothGroupsBeforeReturningAnyChanges) {
    for (const auto model : models) {
        const auto source = configured_file(model);
        const auto before = source;
        axk::SystemRecordingPatch patch;
        patch.configuration.input = 3;
        patch.configuration.frequency_selection = 2;
        patch.effects[1].input_level = 42;
        const auto result = axk::patch_system_recording(source, patch, model);
        ASSERT_TRUE(result);
        auto expected = source;
        expected.system_bulk_bytes[effect_offset(model, 0) + 122U] = std::byte{3};
        expected.system_bulk_bytes[effect_offset(model, 0) + 123U] = std::byte{2};
        expected.system_bulk_bytes[effect_offset(model, 1) + 1U] = std::byte{42};
        EXPECT_EQ(*result, expected);
        patch.effects[2].type = 255;
        EXPECT_FALSE(axk::patch_system_recording(source, patch, model));
        EXPECT_EQ(source, before);
        patch.effects[2] = {};
        patch.configuration.frequency_selection = 6;
        EXPECT_FALSE(axk::patch_system_recording(source, patch, model));
        EXPECT_EQ(source, before);
        const auto empty = axk::patch_system_recording(source, {}, model);
        ASSERT_TRUE(empty);
        EXPECT_EQ(*empty, source);
    }
}

TEST(SystemRecordingConfigurationWrite, InputNormalizationPreservesAllButRequiredDependents) {
    for (const auto model : models) {
        auto source = configured_file(model);
        const auto offset = effect_offset(model, 0) + 120U;
        source.system_bulk_bytes[offset + 1U] = std::byte{0};
        source.system_bulk_bytes[offset + 3U] = std::byte{6};
        source.system_bulk_bytes[offset + 17U] = std::byte{5};
        const auto before = source;
        for (const auto input : {3U, 4U}) {
            axk::SystemRecordingParameters patch;
            patch.input = static_cast<std::uint8_t>(input);
            auto expected = source;
            expected.system_bulk_bytes[offset + 1U] = std::byte{1};
            expected.system_bulk_bytes[offset + 2U] = static_cast<std::byte>(input);
            expected.system_bulk_bytes[offset + 3U] = std::byte{3};
            expected.system_bulk_bytes[offset + 17U] = std::byte{0};
            const auto result = axk::patch_system_recording_configuration(source, patch, model);
            ASSERT_TRUE(result);
            EXPECT_EQ(*result, expected);
            patch.frequency_selection = 2;
            expected.system_bulk_bytes[offset + 3U] = std::byte{2};
            const auto explicit_frequency = axk::patch_system_recording_configuration(source, patch, model);
            ASSERT_TRUE(explicit_frequency);
            EXPECT_EQ(*explicit_frequency, expected);
            patch.frequency_selection = 4;
            EXPECT_FALSE(axk::patch_system_recording_configuration(source, patch, model));
            patch.frequency_selection.reset();
            patch.stereo = false;
            EXPECT_FALSE(axk::patch_system_recording_configuration(source, patch, model));
            patch.stereo.reset();
            patch.monitor_output = 1;
            EXPECT_FALSE(axk::patch_system_recording_configuration(source, patch, model));
            patch.monitor_output.reset();
            patch.monitor_level = 100;
            EXPECT_FALSE(axk::patch_system_recording_configuration(source, patch, model));
        }
        EXPECT_EQ(source, before);
    }
}

TEST(SystemRecordingConfigurationWrite, MapModeTransitionsNormalizeOnlyUnrequestedDependents) {
    for (const auto model : models) {
        auto source = configured_file(model);
        const auto offset = effect_offset(model, 0) + 120U;
        source.system_bulk_bytes[offset + 9U] = std::byte{2};
        axk::SystemRecordingParameters patch;
        patch.record_type = 1;
        auto expected = source;
        expected.system_bulk_bytes[offset] = std::byte{1};
        expected.system_bulk_bytes[offset + 9U] = std::byte{1};
        const auto result = axk::patch_system_recording_configuration(source, patch, model);
        ASSERT_TRUE(result);
        EXPECT_EQ(*result, expected);
        patch.map_destination = 2;
        EXPECT_FALSE(axk::patch_system_recording_configuration(source, patch, model));
        patch.record_type = 2;
        ASSERT_TRUE(axk::patch_system_recording_configuration(source, patch, model));
        for (const auto type : {0U, 3U}) {
            patch.record_type = static_cast<std::uint8_t>(type);
            EXPECT_FALSE(axk::patch_system_recording_configuration(source, patch, model));
            patch.map_destination.reset();
            const auto saved_type = axk::patch_system_recording_configuration(source, patch, model);
            ASSERT_EQ(saved_type.has_value(), type == 0 || model != axk::ASeriesModel::a3000);
            if (saved_type) {
                expected = source;
                expected.system_bulk_bytes[offset] = static_cast<std::byte>(type);
                EXPECT_EQ(*saved_type, expected);
            }
            patch.map_destination = 1;
        }
    }
}

TEST(SystemRecordingConfigurationWrite, AtomicKeyRangesRequireConsistentEffectiveEndpoints) {
    for (const auto model : models) {
        auto source = configured_file(model);
        const auto offset = effect_offset(model, 0) + 120U;
        source.system_bulk_bytes[offset + 10U] = std::byte{70};
        source.system_bulk_bytes[offset + 11U] = std::byte{80};
        axk::SystemRecordingParameters patch;
        patch.key_high = 69;
        EXPECT_FALSE(axk::patch_system_recording_configuration(source, patch, model));
        patch.key_low = 60;
        ASSERT_TRUE(axk::patch_system_recording_configuration(source, patch, model));
        patch.key_low = -1;
        patch.original_key = 70;
        EXPECT_FALSE(axk::patch_system_recording_configuration(source, patch, model));
        patch.original_key = 69;
        ASSERT_TRUE(axk::patch_system_recording_configuration(source, patch, model));
        patch.key_low = 70;
        patch.key_high = 128;
        EXPECT_FALSE(axk::patch_system_recording_configuration(source, patch, model));
        patch.original_key = 71;
        const auto valid = axk::patch_system_recording_configuration(source, patch, model);
        ASSERT_TRUE(valid);
        auto expected = source;
        expected.system_bulk_bytes[offset + 11U] = std::byte{128};
        expected.system_bulk_bytes[offset + 12U] = std::byte{71};
        EXPECT_EQ(*valid, expected);
        patch.key_low = -1;
        patch.original_key = 0;
        ASSERT_TRUE(axk::patch_system_recording_configuration(source, patch, model));
        patch.original_key = 127;
        ASSERT_TRUE(axk::patch_system_recording_configuration(source, patch, model));
    }
}

TEST(SystemRecordingConfigurationWrite, UndecodedDependenciesRequireExplicitReplacementNotGuessing) {
    for (const auto model : models) {
        auto source = configured_file(model);
        const auto offset = effect_offset(model, 0) + 120U;
        source.system_bulk_bytes[offset + 2U] = std::byte{255};
        source.system_bulk_bytes[offset + 3U] = std::byte{255};
        axk::SystemRecordingParameters patch;
        patch.stereo = true;
        EXPECT_FALSE(axk::patch_system_recording_configuration(source, patch, model));
        patch.input = 3;
        EXPECT_FALSE(axk::patch_system_recording_configuration(source, patch, model));
        patch.frequency_selection = 2;
        ASSERT_TRUE(axk::patch_system_recording_configuration(source, patch, model));
        source.system_bulk_bytes[offset + 9U] = std::byte{255};
        patch = {};
        patch.record_type = 1;
        EXPECT_FALSE(axk::patch_system_recording_configuration(source, patch, model));
        patch.map_destination = 0;
        ASSERT_TRUE(axk::patch_system_recording_configuration(source, patch, model));
        source.system_bulk_bytes[offset + 10U] = std::byte{128};
        patch = {};
        patch.original_key = 60;
        EXPECT_FALSE(axk::patch_system_recording_configuration(source, patch, model));
        patch.key_low = -1;
        ASSERT_TRUE(axk::patch_system_recording_configuration(source, patch, model));
    }
}

TEST(SystemRecordingConfigurationWrite, EveryScalarDomainUsesExactBytesAndGenerationBounds) {
    using P = axk::SystemRecordingParameters;
    for (const auto model : models) {
        const auto native = model == axk::ASeriesModel::a3000;
        const auto source = configured_file(model);
        const auto offset = effect_offset(model, 0) + 120U;
        const auto check = [&]<typename T>(std::optional<T> P::*member, std::size_t relative, int minimum,
                                           int maximum) {
            const auto count = std::is_same_v<T, bool> ? 2U : 256U;
            for (unsigned raw = 0; raw < count; ++raw) {
                SCOPED_TRACE(::testing::Message()
                             << "model=" << static_cast<int>(model) << " offset=" << relative << " raw=" << raw);
                const auto numeric =
                    std::is_same_v<T, std::int8_t> && raw >= 128U ? static_cast<int>(raw) - 256 : static_cast<int>(raw);
                P patch;
                patch.*member = static_cast<T>(numeric);
                const auto result = axk::patch_system_recording_configuration(source, patch, model);
                ASSERT_EQ(result.has_value(), numeric >= minimum && numeric <= maximum);
                if (result) {
                    auto expected = source;
                    expected.system_bulk_bytes[offset + relative] = static_cast<std::byte>(raw);
                    EXPECT_EQ(*result, expected);
                    const auto decoded = axk::decode_system_recording(*result);
                    ASSERT_TRUE(decoded);
                    EXPECT_EQ(decoded->parameters.*member, patch.*member);
                }
            }
        };
        check(&P::record_type, 0, 0, native ? 2 : 3);
        check(&P::stereo, 1, 0, 1);
        check(&P::input, 2, 0, 4);
        check(&P::frequency_selection, 3, 0, 6);
        check(&P::pre_trigger_time, 4, 0, 5);
        check(&P::start_trigger, 5, 0, 1);
        check(&P::stop_trigger, 6, 0, 1);
        check(&P::start_edge_level, 7, 0, 63);
        check(&P::stop_edge_level, 8, 0, 63);
        check(&P::map_destination, 9, 0, 2);
        check(&P::key_low, 10, -1, 127);
        check(&P::key_high, 11, 0, 128);
        check(&P::original_key, 12, 0, 127);
        check(&P::auto_normalize, 13, 0, 1);
        check(&P::external_scsi_id, 14, -1, 7);
        check(&P::external_track, 15, 1, native ? 255 : 99);
        check(&P::external_index, 16, 1, native ? 255 : 99);
        check(&P::monitor_output, 17, 0, 5);
        check(&P::monitor_level, 18, 0, 127);
        check(&P::click_level, 19, 0, 127);
        check(&P::click_beat, 22, 1, 15);
        check(&P::monitor_enabled, 23, 0, 1);
        check(&P::map_auto, 24, 0, 1);
        check(&P::map_original_key, 25, 0, 127);
        check(&P::map_all_keys, 26, 0, 1);
        check(&P::ad_input_gain, 51, 0, native ? -1 : 1);
        for (const auto tempo : {0U, 7999U, 8000U, 8001U, 15998U, 15999U, 16000U, 65535U}) {
            P patch;
            patch.click_tempo_hundredths = static_cast<std::uint16_t>(tempo);
            const auto result = axk::patch_system_recording_configuration(source, patch, model);
            ASSERT_EQ(result.has_value(), tempo >= 8000U && tempo <= 15999U);
            if (result) {
                auto expected = source;
                ASSERT_TRUE(axk::ByteWriter{expected.system_bulk_bytes}.write_be16(offset + 20U,
                                                                                   static_cast<std::uint16_t>(tempo)));
                EXPECT_EQ(*result, expected);
            }
        }
    }
}

TEST(SystemRecordingConfigurationWrite, DigitalDependenciesApplyWithoutReselectingInputAndCanBeReplacedAtomically) {
    for (const auto model : models) {
        auto source = configured_file(model);
        const auto offset = effect_offset(model, 0) + 120U;
        source.system_bulk_bytes[offset + 2U] = std::byte{4};
        for (unsigned frequency = 0; frequency < 7U; ++frequency) {
            axk::SystemRecordingParameters patch;
            patch.frequency_selection = static_cast<std::uint8_t>(frequency);
            EXPECT_EQ(axk::patch_system_recording_configuration(source, patch, model).has_value(), frequency <= 3U);
        }
        axk::SystemRecordingParameters patch;
        patch.stereo = false;
        EXPECT_FALSE(axk::patch_system_recording_configuration(source, patch, model));
        patch.stereo.reset();
        patch.monitor_level = 0;
        EXPECT_FALSE(axk::patch_system_recording_configuration(source, patch, model));
        patch.monitor_output = 5;
        patch.frequency_selection = 6;
        patch.stereo = false;
        patch.input = 0;
        const auto analog = axk::patch_system_recording_configuration(source, patch, model);
        ASSERT_TRUE(analog);
        auto expected = source;
        expected.system_bulk_bytes[offset + 1U] = std::byte{0};
        expected.system_bulk_bytes[offset + 2U] = std::byte{0};
        expected.system_bulk_bytes[offset + 3U] = std::byte{6};
        expected.system_bulk_bytes[offset + 17U] = std::byte{5};
        expected.system_bulk_bytes[offset + 18U] = std::byte{0};
        EXPECT_EQ(*analog, expected);
        source.system_bulk_bytes[offset + 1U] = std::byte{0};
        patch = {};
        patch.frequency_selection = 2;
        EXPECT_FALSE(axk::patch_system_recording_configuration(source, patch, model));
        patch.stereo = true;
        ASSERT_TRUE(axk::patch_system_recording_configuration(source, patch, model));
    }
}

TEST(SystemRecordingConfigurationWrite, InvalidModelFramingOrLateFieldNeverChangesTheSource) {
    for (const auto model : models) {
        const auto source = configured_file(model);
        const auto before = source;
        axk::SystemRecordingParameters patch;
        patch.pre_trigger_time = 5;
        patch.click_beat = 0;
        EXPECT_FALSE(axk::patch_system_recording_configuration(source, patch, model));
        EXPECT_EQ(source, before);
        EXPECT_FALSE(axk::patch_system_recording_configuration(source, {}, static_cast<axk::ASeriesModel>(255)));
        const auto mismatch = model == axk::ASeriesModel::a3000 ? axk::ASeriesModel::a4000 : axk::ASeriesModel::a3000;
        EXPECT_FALSE(axk::patch_system_recording_configuration(source, {}, mismatch));
        auto stale = source;
        stale.record_envelope.raw_bytes[0] = std::byte{0};
        EXPECT_FALSE(axk::patch_system_recording_configuration(stale, {}, model));
        stale = source;
        stale.system_bulk_bytes.pop_back();
        EXPECT_FALSE(axk::patch_system_recording_configuration(stale, {}, model));
        stale = source;
        stale.storage_revision = 2;
        EXPECT_FALSE(axk::patch_system_recording_configuration(stale, {}, model));
        if (model != axk::ASeriesModel::a3000) {
            const auto revision_one = retained_file(model, 1);
            const auto result = axk::patch_system_recording_configuration(revision_one, {}, model);
            ASSERT_TRUE(result);
            EXPECT_EQ(*result, revision_one);
        }
    }
}

TEST(SystemRecordingWrite, TypeChangesResetExactlyTheNativeOrCurrentWordsWithoutLegacyProjection) {
    for (const auto model : models) {
        const auto native = model == axk::ASeriesModel::a3000;
        const auto profile = native ? axk::EffectProfile::a3000 : axk::EffectProfile::a4000;
        for (unsigned type = 0; type < (native ? 55U : 97U); ++type) {
            auto source = retained_file(model, model == axk::ASeriesModel::a5000 ? 1 : 0);
            auto expected = source;
            std::array<axk::ProgramEffectParameters, 3> patches;
            for (std::size_t slot = 0; slot < patches.size(); ++slot) {
                const auto offset = effect_offset(model, slot);
                const auto type_offset = offset + (native ? 7U : 6U);
                source.system_bulk_bytes[type_offset] = std::byte{255};
                expected.system_bulk_bytes[type_offset] = static_cast<std::byte>(type);
                patches[slot].type = static_cast<std::uint8_t>(type);
                const auto info = axk::effect_write_info(static_cast<std::uint16_t>(type), profile);
                ASSERT_TRUE(info);
                axk::ByteWriter writer{expected.system_bulk_bytes};
                for (std::size_t index = 0; index < 16U; ++index)
                    ASSERT_TRUE(writer.write_be16(offset + 8U + index * 2U, info->reset_words[index]));
            }
            const auto result = axk::patch_system_recording_effects(source, patches, model);
            ASSERT_TRUE(result) << type;
            EXPECT_EQ(*result, expected) << type;
            const auto read = axk::decode_system_recording(*result);
            ASSERT_TRUE(read);
            for (const auto &effect : read->effects)
                EXPECT_EQ(effect.type, type);
        }
    }
}

TEST(SystemRecordingWrite, SameTypePreservesHiddenWordsAndExplicitValuesOverrideResets) {
    for (const auto model : models) {
        auto source = retained_file(model);
        const auto offset = effect_offset(model, 1);
        const auto type_offset = offset + (model == axk::ASeriesModel::a3000 ? 7U : 6U);
        source.system_bulk_bytes[type_offset] = std::byte{1};
        std::array<axk::ProgramEffectParameters, 3> patches;
        patches[1].type = 1;
        auto unchanged = axk::patch_system_recording_effects(source, patches, model);
        ASSERT_TRUE(unchanged);
        EXPECT_EQ(*unchanged, source);
        patches[1].parameters[0] = 17;
        auto expected = source;
        ASSERT_TRUE(axk::ByteWriter{expected.system_bulk_bytes}.write_be16(offset + 8, 17));
        const auto changed = axk::patch_system_recording_effects(source, patches, model);
        ASSERT_TRUE(changed);
        EXPECT_EQ(*changed, expected);
        source.system_bulk_bytes[type_offset] = std::byte{0};
        const auto reset = axk::patch_system_recording_effects(source, patches, model);
        ASSERT_TRUE(reset);
        EXPECT_EQ(axk::ByteReader{reset->system_bulk_bytes}.be16(offset + 8), 17);
    }
}

TEST(SystemRecordingWrite, RejectsInvalidPatchesAtomicallyAndRequiresMatchingExplicitModel) {
    for (const auto model : models) {
        const auto source = retained_file(model);
        const auto original = source;
        std::array<axk::ProgramEffectParameters, 3> patches;
        patches[0].enabled = true;
        patches[2].type = model == axk::ASeriesModel::a3000 ? 55 : 97;
        EXPECT_FALSE(axk::patch_system_recording_effects(source, patches, model));
        patches[2] = {};
        patches[2].type = 1;
        patches[2].parameters[0] = 65535;
        EXPECT_FALSE(axk::patch_system_recording_effects(source, patches, model));
        patches[2].parameters[0].reset();
        patches[2].parameters[15] = 0;
        EXPECT_FALSE(axk::patch_system_recording_effects(source, patches, model));
        patches[2] = {};
        patches[2].destination = 8;
        EXPECT_EQ(axk::patch_system_recording_effects(source, patches, model).has_value(),
                  model == axk::ASeriesModel::a5000);
        patches[2].destination = 9;
        EXPECT_FALSE(axk::patch_system_recording_effects(source, patches, model));
        patches[2] = {};
        if (model != axk::ASeriesModel::a3000) {
            patches[2].type = 91;
            patches[2].parameters[14] = 0;
            EXPECT_FALSE(axk::patch_system_recording_effects(source, patches, model));
        }
        EXPECT_FALSE(axk::patch_system_recording_effects(source, {}, static_cast<axk::ASeriesModel>(255)));
        const auto mismatch = model == axk::ASeriesModel::a3000 ? axk::ASeriesModel::a4000 : axk::ASeriesModel::a3000;
        EXPECT_FALSE(axk::patch_system_recording_effects(source, {}, mismatch));
        EXPECT_EQ(source, original);
        auto stale = source;
        stale.record_envelope.raw_bytes[0] = std::byte{0};
        EXPECT_FALSE(axk::patch_system_recording_effects(stale, {}, model));
        stale = source;
        stale.system_bulk_bytes.pop_back();
        EXPECT_FALSE(axk::patch_system_recording_effects(stale, {}, model));
        stale = source;
        stale.storage_revision = 2;
        EXPECT_FALSE(axk::patch_system_recording_effects(stale, {}, model));
        patches = {};
        patches[0].parameters[0] = 1;
        stale = source;
        stale.system_bulk_bytes[effect_offset(model, 0) + (model == axk::ASeriesModel::a3000 ? 7U : 6U)] =
            std::byte{255};
        EXPECT_FALSE(axk::patch_system_recording_effects(stale, patches, model));
    }
}

TEST(SystemRecordingWrite, ChecksAllScalarByteValuesAndChangesOnlyTheSelectedByte) {
    using Parameters = axk::ProgramEffectParameters;
    struct UnsignedField {
        std::optional<std::uint8_t> Parameters::*member;
        std::size_t offset;
        unsigned maximum;
    };
    struct SignedField {
        std::optional<std::int8_t> Parameters::*member;
        std::size_t offset;
        int minimum;
        int maximum;
    };
    constexpr std::array unsigned_fields{
        UnsignedField{&Parameters::input_level, 1, 127},
        UnsignedField{&Parameters::output_level, 2, 127},
        UnsignedField{&Parameters::destination, 4, 5},
    };
    constexpr std::array signed_fields{
        SignedField{&Parameters::pan, 3, -63, 63},
        SignedField{&Parameters::width, 5, -126, 0},
    };
    for (const auto model : models) {
        const auto source = retained_file(model);
        for (unsigned raw = 0; raw < 256U; ++raw) {
            for (const auto &field : unsigned_fields) {
                std::array<Parameters, 3> patches;
                patches[2].*field.member = static_cast<std::uint8_t>(raw);
                const auto result = axk::patch_system_recording_effects(source, patches, model);
                const auto maximum = field.offset == 4 && model == axk::ASeriesModel::a5000 ? 8U : field.maximum;
                ASSERT_EQ(result.has_value(), raw <= maximum);
                if (result) {
                    auto expected = source;
                    expected.system_bulk_bytes[effect_offset(model, 2) + field.offset] = static_cast<std::byte>(raw);
                    EXPECT_EQ(*result, expected);
                }
            }
            const auto signed_value = raw < 128U ? static_cast<int>(raw) : static_cast<int>(raw) - 256;
            for (const auto &field : signed_fields) {
                std::array<Parameters, 3> patches;
                patches[1].*field.member = static_cast<std::int8_t>(signed_value);
                const auto result = axk::patch_system_recording_effects(source, patches, model);
                ASSERT_EQ(result.has_value(), signed_value >= field.minimum && signed_value <= field.maximum);
                if (result) {
                    auto expected = source;
                    expected.system_bulk_bytes[effect_offset(model, 1) + field.offset] = static_cast<std::byte>(raw);
                    EXPECT_EQ(*result, expected);
                }
            }
        }
    }
}
