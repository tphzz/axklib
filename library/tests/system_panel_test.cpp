#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/system_file.hpp"

namespace {

axk::DecodedSystemFile retained_panel(bool native, std::uint8_t revision, unsigned value) {
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
    const std::size_t offset = native ? 0x120U : 0x330U;
    for (std::size_t i = 0; i < 64U; ++i)
        bytes[offset + i] = static_cast<std::byte>(value);
    return axk::decode_system_file(
               native ? axk::SystemFileKind::a3000_system : axk::SystemFileKind::a4000_a5000_system2, bytes)
        .value();
}

} // namespace

TEST(SystemPanel, ImportAndCdrDecodeEveryStoredValueWithoutInventingNativeMeanings) {
    for (const bool native : std::array{false, true}) {
        for (std::uint8_t revision = 0; revision <= (native ? 0U : 1U); ++revision) {
            for (unsigned value = 0; value < 256U; ++value) {
                const auto file = retained_panel(native, revision, value);
                const auto before = axk::encode_system_file(file).value();
                const auto decoded = axk::decode_system_panel(file).value();
                const auto check = [native, value](const auto &choice, unsigned maximum) {
                    EXPECT_EQ(choice.has_value(), !native && value <= maximum);
                    if (choice)
                        EXPECT_EQ(static_cast<unsigned>(*choice), value);
                };
                check(decoded.parameters.import_view, 3);
                check(decoded.parameters.cdr_scsi_id, 7);
                check(decoded.parameters.cdr_write_speed, 4);
                for (const auto byte : decoded.raw_bytes)
                    EXPECT_EQ(byte, static_cast<std::byte>(value));
                EXPECT_EQ(axk::encode_system_file(file).value(), before);
            }
        }
    }
}

TEST(SystemPanel, ControlPreferencesDecodeEveryStoredValueAndRevisionWithoutNormalization) {
    for (const bool native : std::array{false, true}) {
        for (std::uint8_t revision = 0; revision <= (native ? 0U : 1U); ++revision) {
            for (unsigned value = 0; value < 256U; ++value) {
                const auto file = retained_panel(native, revision, value);
                const auto before = axk::encode_system_file(file).value();
                const auto p = axk::decode_system_panel(file).value().parameters;
                const auto check = [value](const auto &choice, unsigned maximum) {
                    EXPECT_EQ(choice.has_value(), value <= maximum);
                    if (choice)
                        EXPECT_EQ(static_cast<unsigned>(*choice), value);
                };
                check(p.effect_edit_mode, 1);
                for (const auto &type : p.knob_control_types)
                    check(type, 4);
                check(p.assignable_key_function, 5);
                check(p.audition_trigger_mode, 1);
                check(p.knob_1_type, 1);
                EXPECT_EQ(axk::encode_system_file(file).value(), before);
            }
        }
    }
}

TEST(SystemPanel, ImportAndCdrPatchesValidateEveryChoiceAndOwnExactlyOneByte) {
    for (const auto model : std::array{axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        const bool native = model == axk::ASeriesModel::a3000;
        for (std::uint8_t revision = 0; revision <= (native ? 0U : 1U); ++revision) {
            const auto file = retained_panel(native, revision, 255);
            const auto before = axk::encode_system_file(file).value();
            const auto exercise = [&](auto select, const auto &choices, std::size_t index) {
                for (unsigned value = 0; value < 256U; ++value) {
                    SCOPED_TRACE(static_cast<unsigned>(model));
                    SCOPED_TRACE(revision);
                    SCOPED_TRACE(index);
                    SCOPED_TRACE(value);
                    axk::SystemPanelPatch patch;
                    using Choice = typename std::remove_cvref_t<decltype(choices)>::value_type;
                    select(patch) = value < choices.size() ? choices[value] : static_cast<Choice>(value);
                    const auto result = axk::patch_system_panel(file, patch, model);
                    ASSERT_EQ(result.has_value(), !native && value < choices.size());
                    if (result) {
                        auto expected = before;
                        expected[0x330U + index] = static_cast<std::byte>(value);
                        EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
                        const auto decoded = axk::decode_system_panel(*result).value().parameters;
                        EXPECT_EQ(select(decoded), select(patch));
                        EXPECT_EQ(axk::patch_system_panel(*result, patch, model).value(), *result);
                    } else {
                        EXPECT_EQ(result.error().code, axk::ErrorCode::invalid_argument);
                        axk::SystemFilePatch mixed;
                        mixed.global.master_fine_tune = 7;
                        mixed.panel = patch;
                        mixed.panel.end_type = axk::SystemEndType::beat;
                        EXPECT_FALSE(axk::patch_system_file(file, mixed, model));
                    }
                    EXPECT_EQ(axk::encode_system_file(file).value(), before);
                }
            };
            exercise([](auto &p) -> auto & { return p.import_view; },
                     std::array{axk::SystemImportView::all, axk::SystemImportView::sample_bank,
                                axk::SystemImportView::sample, axk::SystemImportView::sequence},
                     16);
            exercise([](auto &p) -> auto & { return p.cdr_scsi_id; },
                     std::array<std::uint8_t, 8>{0, 1, 2, 3, 4, 5, 6, 7}, 25);
            exercise([](auto &p) -> auto & { return p.cdr_write_speed; },
                     std::array{axk::SystemCdrWriteSpeed::x1, axk::SystemCdrWriteSpeed::x2,
                                axk::SystemCdrWriteSpeed::x4, axk::SystemCdrWriteSpeed::x6,
                                axk::SystemCdrWriteSpeed::x8},
                     26);
            EXPECT_EQ(axk::patch_system_panel(file, {}, model).value(), file);
        }
    }
}

TEST(SystemPanel, ImportAndCdrCombinedPatchPreservesInvalidUnrequestedPreferences) {
    for (const auto model : std::array{axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        for (std::uint8_t revision = 0; revision <= 1U; ++revision) {
            const auto file = retained_panel(false, revision, 255);
            const auto before = axk::encode_system_file(file).value();
            axk::SystemFilePatch patch;
            patch.global.master_fine_tune = 7;
            patch.panel.import_view = axk::SystemImportView::sample_bank;
            patch.panel.cdr_scsi_id = 6;
            patch.panel.cdr_write_speed = axk::SystemCdrWriteSpeed::x6;
            const auto result = axk::patch_system_file(file, patch, model);
            ASSERT_TRUE(result) << result.error().message;
            auto expected = before;
            expected[0x60] = std::byte{7};
            expected[0x340] = std::byte{1};
            expected[0x349] = std::byte{6};
            expected[0x34a] = std::byte{3};
            EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
            EXPECT_EQ(axk::encode_system_file(file).value(), before);
        }
    }
}

TEST(SystemPanel, PreservesAllBytesWithoutLoaderNormalizationAcrossEveryStoredValue) {
    for (const bool native : std::array{false, true}) {
        for (unsigned value = 0; value < 256U; ++value) {
            SCOPED_TRACE(value);
            const auto file = retained_panel(native, native ? 0 : 1, value);
            const auto before = axk::encode_system_file(file).value();
            const auto decoded = axk::decode_system_panel(file);
            ASSERT_TRUE(decoded);
            for (const auto byte : decoded->raw_bytes)
                EXPECT_EQ(byte, static_cast<std::byte>(value));
            const auto &p = decoded->parameters;
            EXPECT_EQ(p.format_drive_id.has_value(), value <= (native ? 7U : 9U));
            EXPECT_EQ(p.format_type.has_value(), value <= (native ? 2U : 5U));
            EXPECT_EQ(p.function_selection.has_value(), value <= 2U);
            EXPECT_EQ(p.page_selection.has_value(), value <= 1U);
            EXPECT_EQ(p.note_display.has_value(), value <= 1U);
            EXPECT_EQ(p.end_type.has_value(), value <= (native ? 4U : 3U));
            EXPECT_EQ(p.layer_selection_scope.has_value(), value <= 1U);
            EXPECT_EQ(p.sample_name_order_selection.has_value(), native && value <= 2U);
            EXPECT_EQ(p.program_on_placement.has_value(), native && value <= 1U);
            EXPECT_EQ(p.bank_member_visibility.has_value(), native && value <= 1U);
            EXPECT_EQ(p.audition_name_view.has_value(), native && value <= 1U);
            EXPECT_EQ(p.midi_to_sample_name_view.has_value(), native && value <= 1U);
            EXPECT_EQ(p.sample_sort.has_value(), !native && value <= 2U);
            EXPECT_EQ(p.tree_sort.has_value(), !native && value <= 2U);
            EXPECT_EQ(p.sample_bank_sort.has_value(), !native && value <= 2U);
            EXPECT_EQ(axk::encode_system_file(file).value(), before);
        }
    }
}

TEST(SystemPanel, ControlPatchesValidateEveryRequestAndPreserveEveryUnrequestedByte) {
    for (const auto model : std::array{axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        const bool native = model == axk::ASeriesModel::a3000;
        for (std::uint8_t revision = 0; revision <= (native ? 0U : 1U); ++revision) {
            const auto file = retained_panel(native, revision, 255);
            const auto before = axk::encode_system_file(file).value();
            const auto exercise = [&](auto select, const auto &choices, std::size_t index) {
                for (unsigned value = 0; value < 256U; ++value) {
                    SCOPED_TRACE(static_cast<unsigned>(model));
                    SCOPED_TRACE(revision);
                    SCOPED_TRACE(index);
                    SCOPED_TRACE(value);
                    axk::SystemPanelPatch patch;
                    // The semantic choice list independently fixes the expected disk order.
                    using Choice = typename std::remove_cvref_t<decltype(choices)>::value_type;
                    select(patch) = value < choices.size() ? choices[value] : static_cast<Choice>(value);
                    const auto result = axk::patch_system_panel(file, patch, model);
                    ASSERT_EQ(result.has_value(), value < choices.size());
                    if (result) {
                        auto expected = before;
                        expected[(native ? 0x120U : 0x330U) + index] = static_cast<std::byte>(value);
                        EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
                        const auto decoded = axk::decode_system_panel(*result).value().parameters;
                        EXPECT_EQ(select(decoded), select(patch));
                        const auto repeated = axk::patch_system_panel(*result, patch, model);
                        ASSERT_TRUE(repeated);
                        EXPECT_EQ(axk::encode_system_file(*repeated).value(), expected);
                    } else {
                        EXPECT_EQ(result.error().code, axk::ErrorCode::invalid_argument);
                    }
                    EXPECT_EQ(axk::encode_system_file(file).value(), before);
                }
            };
            exercise([](auto &p) -> auto & { return p.effect_edit_mode; },
                     std::array{axk::SystemEffectEditMode::full, axk::SystemEffectEditMode::favorite}, 1);
            for (std::size_t knob = 0; knob < 4U; ++knob)
                exercise([knob](auto &p) -> auto & { return p.knob_control_types[knob]; },
                         std::array{axk::SystemKnobControlType::off, axk::SystemKnobControlType::on,
                                    axk::SystemKnobControlType::step_1, axk::SystemKnobControlType::step_2,
                                    axk::SystemKnobControlType::step_3},
                         2U + knob);
            exercise([](auto &p) -> auto & { return p.assignable_key_function; },
                     std::array{axk::SystemAssignableKeyFunction::knob_control, axk::SystemAssignableKeyFunction::damp,
                                axk::SystemAssignableKeyFunction::controller_reset,
                                axk::SystemAssignableKeyFunction::function_key_play,
                                axk::SystemAssignableKeyFunction::knob_and_function_key,
                                axk::SystemAssignableKeyFunction::midi_to_sample},
                     6);
            exercise([](auto &p) -> auto & { return p.audition_trigger_mode; },
                     std::array{axk::SystemAuditionTriggerMode::normal, axk::SystemAuditionTriggerMode::toggle}, 7);
            exercise([](auto &p) -> auto & { return p.knob_1_type; },
                     std::array{axk::SystemKnob1Type::page, axk::SystemKnob1Type::sample}, 17);
        }
    }
}

TEST(SystemPanel, MixedControlPatchRejectsInvalidScalarOrAnyKnobWithoutMutatingOtherGroups) {
    for (const auto model : std::array{axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        const bool native = model == axk::ASeriesModel::a3000;
        const auto file = retained_panel(native, native ? 0 : 1, 255);
        const auto before = axk::encode_system_file(file).value();
        axk::SystemFilePatch patch;
        patch.panel.effect_edit_mode = axk::SystemEffectEditMode::favorite;
        patch.panel.knob_control_types.fill(axk::SystemKnobControlType::on);
        patch.panel.assignable_key_function = axk::SystemAssignableKeyFunction::function_key_play;
        patch.panel.audition_trigger_mode = axk::SystemAuditionTriggerMode::toggle;
        patch.panel.knob_1_type = static_cast<axk::SystemKnob1Type>(2);
        patch.global.master_fine_tune = 7;
        EXPECT_FALSE(axk::patch_system_file(file, patch, model));
        patch.panel.knob_1_type = axk::SystemKnob1Type::sample;
        for (std::size_t knob = 0; knob < 4U; ++knob) {
            patch.panel.knob_control_types[knob] = static_cast<axk::SystemKnobControlType>(5);
            EXPECT_FALSE(axk::patch_system_file(file, patch, model));
            EXPECT_EQ(axk::encode_system_file(file).value(), before);
            patch.panel.knob_control_types[knob] = axk::SystemKnobControlType::on;
        }
        const auto result = axk::patch_system_file(file, patch, model);
        ASSERT_TRUE(result) << result.error().message;
        auto expected = before;
        const auto base = native ? 0x120U : 0x330U;
        for (std::size_t i = 1; i <= 7U; ++i)
            expected[base + i] = i == 6U ? std::byte{3} : std::byte{1};
        expected[base + 17U] = std::byte{1};
        expected[0x60] = std::byte{7};
        EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
        EXPECT_EQ(axk::encode_system_file(file).value(), before);
    }
}

TEST(SystemPanel, UsesGenerationSpecificFormatAndScopeMappings) {
    for (unsigned value = 0; value < 6U; ++value) {
        const auto current = axk::decode_system_panel(retained_panel(false, 0, value)).value();
        ASSERT_TRUE(current.parameters.format_type);
        EXPECT_EQ(static_cast<unsigned>(*current.parameters.format_type), value);
        const auto native = axk::decode_system_panel(retained_panel(true, 0, value)).value();
        if (value < 3U) {
            ASSERT_TRUE(native.parameters.format_type);
            EXPECT_EQ(static_cast<unsigned>(*native.parameters.format_type), value + 3U);
        } else {
            EXPECT_FALSE(native.parameters.format_type);
        }
    }
    const auto native = axk::decode_system_panel(retained_panel(true, 0, 1)).value().parameters;
    const auto current = axk::decode_system_panel(retained_panel(false, 1, 1)).value().parameters;
    EXPECT_EQ(native.layer_selection_scope, axk::SystemLayerSelectionScope::selection_page);
    EXPECT_EQ(current.layer_selection_scope, axk::SystemLayerSelectionScope::tree_page);
    EXPECT_EQ(native.audition_name_view, false);
    EXPECT_EQ(native.midi_to_sample_name_view, false);
    EXPECT_EQ(native.program_on_placement, axk::SystemProgramOnPlacement::mixed);
    EXPECT_EQ(native.bank_member_visibility, axk::SystemBankMemberVisibility::show);
    EXPECT_EQ(native.function_selection, axk::SystemFunctionSelection::last);
    EXPECT_EQ(native.page_selection, axk::SystemPageSelection::last);
    EXPECT_EQ(native.note_display, axk::SystemNoteDisplay::number);
    const auto enabled = axk::decode_system_panel(retained_panel(true, 0, 0)).value().parameters;
    EXPECT_EQ(enabled.audition_name_view, true);
    EXPECT_EQ(enabled.midi_to_sample_name_view, true);
}

TEST(SystemPanel, NativeNameViewPatchesChangeOnlyTheirOwnedByte) {
    for (unsigned stored = 0; stored < 256U; ++stored) {
        const auto file = retained_panel(true, 0, stored);
        const auto before = axk::encode_system_file(file).value();
        for (const bool enabled : std::array{false, true}) {
            for (const bool audition : std::array{false, true}) {
                SCOPED_TRACE(stored);
                SCOPED_TRACE(enabled);
                SCOPED_TRACE(audition);
                axk::SystemPanelPatch patch;
                (audition ? patch.audition_name_view : patch.midi_to_sample_name_view) = enabled;
                const auto result = axk::patch_system_panel(file, patch, axk::ASeriesModel::a3000);
                ASSERT_TRUE(result);
                auto expected = before;
                expected[0x120U + (audition ? 18U : 20U)] = enabled ? std::byte{} : std::byte{1};
                EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
                const auto decoded = axk::decode_system_panel(*result).value().parameters;
                EXPECT_EQ(audition ? decoded.audition_name_view : decoded.midi_to_sample_name_view, enabled);
                const auto repeated = axk::patch_system_panel(*result, patch, axk::ASeriesModel::a3000);
                ASSERT_TRUE(repeated);
                EXPECT_EQ(axk::encode_system_file(*repeated).value(), expected);
                EXPECT_EQ(axk::encode_system_file(file).value(), before);
            }
        }
    }
}

TEST(SystemPanel, NameViewPatchesRejectMainModelsEvenWhenFalseAndDoNotApplyMixedRequests) {
    for (const auto model : std::array{axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        for (std::uint8_t revision = 0; revision <= 1U; ++revision) {
            const auto file = retained_panel(false, revision, 255);
            const auto before = axk::encode_system_file(file).value();
            for (const bool enabled : std::array{false, true}) {
                for (const bool audition : std::array{false, true}) {
                    axk::SystemPanelPatch patch;
                    patch.end_type = axk::SystemEndType::beat;
                    patch.sample_sort = axk::SystemSampleSort::name;
                    (audition ? patch.audition_name_view : patch.midi_to_sample_name_view) = enabled;
                    const auto result = axk::patch_system_panel(file, patch, model);
                    ASSERT_FALSE(result);
                    EXPECT_EQ(result.error().code, axk::ErrorCode::invalid_argument);
                    EXPECT_EQ(axk::encode_system_file(file).value(), before);
                }
            }
        }
    }
}

TEST(SystemPanel, FormatSelectionsValidateEveryRequestAndChangeOnlyTheRequestedByte) {
    for (const auto model : std::array{axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        const bool native = model == axk::ASeriesModel::a3000;
        for (std::uint8_t revision = 0; revision <= (native ? 0U : 1U); ++revision) {
            const auto file = retained_panel(native, revision, 255);
            const auto before = axk::encode_system_file(file).value();
            for (unsigned value = 0; value < 256U; ++value) {
                for (const bool drive : std::array{false, true}) {
                    SCOPED_TRACE(static_cast<unsigned>(model));
                    SCOPED_TRACE(revision);
                    SCOPED_TRACE(value);
                    SCOPED_TRACE(drive);
                    axk::SystemPanelPatch patch;
                    if (drive)
                        patch.format_drive_id = static_cast<std::uint8_t>(value);
                    else
                        patch.format_type = static_cast<axk::SystemFormatType>(value);
                    const auto result = axk::patch_system_panel(file, patch, model);
                    const bool valid = drive ? value <= (native ? 7U : 9U) : value <= 5U && (!native || value >= 3U);
                    ASSERT_EQ(result.has_value(), valid);
                    if (valid) {
                        auto expected = before;
                        expected[(native ? 0x120U : 0x330U) + (drive ? 0U : 11U)] =
                            static_cast<std::byte>(value - (!drive && native ? 3U : 0U));
                        EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
                        const auto decoded = axk::decode_system_panel(*result).value().parameters;
                        if (drive)
                            EXPECT_EQ(decoded.format_drive_id, patch.format_drive_id);
                        else
                            EXPECT_EQ(decoded.format_type, static_cast<axk::SystemFormatType>(value));
                        const auto repeated = axk::patch_system_panel(*result, patch, model);
                        ASSERT_TRUE(repeated);
                        EXPECT_EQ(axk::encode_system_file(*repeated).value(), expected);
                    } else {
                        EXPECT_EQ(result.error().code, axk::ErrorCode::invalid_argument);
                    }
                    EXPECT_EQ(axk::encode_system_file(file).value(), before);
                }
            }
        }
    }
}

TEST(SystemPanel, InvalidFormatSelectionsRejectCombinedEditsAtomically) {
    for (const auto model : std::array{axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        const bool native = model == axk::ASeriesModel::a3000;
        const auto file = retained_panel(native, native ? 0 : 1, 255);
        const auto before = axk::encode_system_file(file).value();
        axk::SystemPanelPatch patch;
        patch.end_type = axk::SystemEndType::beat;
        patch.format_drive_id = 0;
        patch.format_type = native ? axk::SystemFormatType::logical : static_cast<axk::SystemFormatType>(255);
        EXPECT_FALSE(axk::patch_system_panel(file, patch, model));
        patch.format_type = axk::SystemFormatType::floppy_quick;
        patch.format_drive_id = native ? 8 : 10;
        EXPECT_FALSE(axk::patch_system_panel(file, patch, model));
        EXPECT_EQ(axk::encode_system_file(file).value(), before);
    }
}

TEST(SystemPanel, RejectsUnsupportedLayoutsButNotInvalidParameterBytes) {
    auto file = retained_panel(false, 1, 255);
    EXPECT_TRUE(axk::decode_system_panel(file));
    file.storage_revision = 2;
    EXPECT_FALSE(axk::decode_system_panel(file));
    file.storage_revision = 1;
    file.system_bulk_bytes.pop_back();
    EXPECT_FALSE(axk::decode_system_panel(file));
    file = retained_panel(true, 0, 255);
    file.storage_revision = 1;
    EXPECT_FALSE(axk::decode_system_panel(file));
    file = retained_panel(true, 0, 0);
    file.kind = static_cast<axk::SystemFileKind>(255);
    EXPECT_FALSE(axk::decode_system_panel(file));
}

TEST(SystemPanel, EndTypeUsesCommonChoicesAndNativeOnlyGraphAcrossSupportedRevisions) {
    const auto choices = std::array{axk::SystemEndType::address, axk::SystemEndType::length, axk::SystemEndType::time,
                                    axk::SystemEndType::beat, axk::SystemEndType::graph};
    for (const bool native : std::array{false, true}) {
        for (std::uint8_t revision = 0; revision <= (native ? 0U : 1U); ++revision) {
            for (unsigned value = 0; value < 256U; ++value) {
                SCOPED_TRACE(native);
                SCOPED_TRACE(revision);
                SCOPED_TRACE(value);
                auto file = retained_panel(native, revision, 255);
                file.system_bulk_bytes[(native ? 0xd0U : 0x2e0U) + 15U] = static_cast<std::byte>(value);
                const auto before = axk::encode_system_file(file).value();
                const auto decoded = axk::decode_system_panel(file);
                ASSERT_TRUE(decoded);
                axk::SystemPanelParameters expected;
                if (value < (native ? 5U : 4U))
                    expected.end_type = choices[value];
                EXPECT_EQ(decoded->parameters, expected);
                EXPECT_EQ(decoded->raw_bytes[15], static_cast<std::byte>(value));
                EXPECT_EQ(axk::encode_system_file(file).value(), before);
            }
        }
    }
}

TEST(SystemPanel, EachProjectionUsesOnlyItsAssignedPanelByte) {
    for (const bool native : std::array{false, true}) {
        for (std::size_t index = 0; index < 64U; ++index) {
            auto file = retained_panel(native, 0, 255);
            file.system_bulk_bytes[(native ? 0xd0U : 0x2e0U) + index] = std::byte{};
            const auto p = axk::decode_system_panel(file).value().parameters;
            EXPECT_EQ(p.format_drive_id.has_value(), index == 0U);
            EXPECT_EQ(p.effect_edit_mode.has_value(), index == 1U);
            for (std::size_t knob = 0; knob < 4U; ++knob)
                EXPECT_EQ(p.knob_control_types[knob].has_value(), index == 2U + knob);
            EXPECT_EQ(p.assignable_key_function.has_value(), index == 6U);
            EXPECT_EQ(p.audition_trigger_mode.has_value(), index == 7U);
            EXPECT_EQ(p.knob_1_type.has_value(), index == 17U);
            EXPECT_EQ(p.function_selection.has_value(), index == 8U);
            EXPECT_EQ(p.page_selection.has_value(), index == 9U);
            EXPECT_EQ(p.note_display.has_value(), index == 10U);
            EXPECT_EQ(p.format_type.has_value(), index == 11U);
            EXPECT_EQ(p.sample_name_order_selection.has_value(), native && index == 12U);
            EXPECT_EQ(p.program_on_placement.has_value(), native && index == 13U);
            EXPECT_EQ(p.bank_member_visibility.has_value(), native && index == 14U);
            EXPECT_EQ(p.end_type.has_value(), index == 15U);
            EXPECT_EQ(p.audition_name_view.has_value(), native && index == 18U);
            EXPECT_EQ(p.layer_selection_scope.has_value(), index == 19U);
            EXPECT_EQ(p.midi_to_sample_name_view.has_value(), native && index == 20U);
            EXPECT_EQ(p.sample_sort.has_value(), !native && index == 22U);
            EXPECT_EQ(p.tree_sort.has_value(), !native && index == 23U);
            EXPECT_EQ(p.sample_bank_sort.has_value(), !native && index == 24U);
            EXPECT_EQ(p.import_view.has_value(), !native && index == 16U);
            EXPECT_EQ(p.cdr_scsi_id.has_value(), !native && index == 25U);
            EXPECT_EQ(p.cdr_write_speed.has_value(), !native && index == 26U);
        }
    }
}

TEST(SystemPanel, SortingPatchesValidateEveryValueAndModelWithoutNormalizingOtherState) {
    constexpr std::array<std::size_t, 6> indices{12U, 13U, 14U, 22U, 23U, 24U};
    for (const auto model : {axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        const bool native = model == axk::ASeriesModel::a3000;
        for (std::uint8_t revision = 0; revision <= (native ? 0U : 1U); ++revision) {
            const auto file = retained_panel(native, revision, 255);
            const auto original = axk::encode_system_file(file).value();
            for (std::size_t field = 0; field < indices.size(); ++field) {
                for (unsigned value = 0; value < 256U; ++value) {
                    SCOPED_TRACE(static_cast<unsigned>(model));
                    SCOPED_TRACE(revision);
                    SCOPED_TRACE(field);
                    SCOPED_TRACE(value);
                    axk::SystemPanelPatch patch;
                    switch (field) {
                    case 0:
                        patch.sample_name_order_selection = static_cast<std::uint8_t>(value);
                        break;
                    case 1:
                        patch.program_on_placement = static_cast<axk::SystemProgramOnPlacement>(value);
                        break;
                    case 2:
                        patch.bank_member_visibility = static_cast<axk::SystemBankMemberVisibility>(value);
                        break;
                    case 3:
                        patch.sample_sort = static_cast<axk::SystemSampleSort>(value);
                        break;
                    case 4:
                        patch.tree_sort = static_cast<axk::SystemStatusSort>(value);
                        break;
                    case 5:
                        patch.sample_bank_sort = static_cast<axk::SystemStatusSort>(value);
                        break;
                    }
                    const bool valid = (native == (field < 3U)) && value <= ((field == 1U || field == 2U) ? 1U : 2U);
                    const auto result = axk::patch_system_panel(file, patch, model);
                    if (valid) {
                        ASSERT_TRUE(result) << result.error().message;
                        auto expected = original;
                        expected[(native ? 0x120U : 0x330U) + indices[field]] = static_cast<std::byte>(value);
                        EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
                        const auto p = axk::decode_system_panel(*result).value().parameters;
                        EXPECT_EQ(p.sample_name_order_selection, patch.sample_name_order_selection);
                        EXPECT_EQ(p.program_on_placement, patch.program_on_placement);
                        EXPECT_EQ(p.bank_member_visibility, patch.bank_member_visibility);
                        EXPECT_EQ(p.sample_sort, patch.sample_sort);
                        EXPECT_EQ(p.tree_sort, patch.tree_sort);
                        EXPECT_EQ(p.sample_bank_sort, patch.sample_bank_sort);
                        EXPECT_EQ(axk::patch_system_panel(*result, patch, model).value(), *result);
                    } else {
                        ASSERT_FALSE(result);
                        EXPECT_EQ(result.error().code, axk::ErrorCode::invalid_argument);
                    }
                    EXPECT_EQ(axk::encode_system_file(file).value(), original);
                }
            }
        }
    }
}

TEST(SystemPanel, SortingMixedModelRejectionIsAtomicEvenForZeroValuedRequests) {
    for (const auto model : {axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        const auto file = retained_panel(model == axk::ASeriesModel::a3000, 0, 255);
        const auto original = axk::encode_system_file(file).value();
        axk::SystemFilePatch patch;
        patch.panel.end_type = axk::SystemEndType::beat;
        patch.panel.sample_name_order_selection = 0;
        patch.panel.sample_sort = axk::SystemSampleSort::off;
        EXPECT_FALSE(axk::patch_system_file(file, patch, model));
        EXPECT_EQ(axk::encode_system_file(file).value(), original);
    }
    const auto p = axk::decode_system_panel(retained_panel(false, 1, 2)).value().parameters;
    EXPECT_EQ(p.sample_sort, axk::SystemSampleSort::receive_channel_and_name);
    EXPECT_EQ(p.tree_sort, axk::SystemStatusSort::status_and_name);
    EXPECT_EQ(p.sample_bank_sort, axk::SystemStatusSort::status_and_name);
}

TEST(SystemPanel, EndTypePatchChangesOnlyOneByteAcrossEveryRequestAndModel) {
    for (const auto model : {axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        const bool native = model == axk::ASeriesModel::a3000;
        for (std::uint8_t revision = 0; revision <= (native ? 0U : 1U); ++revision) {
            const auto file = retained_panel(native, revision, 255);
            const auto original = axk::encode_system_file(file).value();
            for (unsigned value = 0; value < 256U; ++value) {
                SCOPED_TRACE(static_cast<unsigned>(model));
                SCOPED_TRACE(revision);
                SCOPED_TRACE(value);
                axk::SystemPanelPatch patch;
                patch.end_type = static_cast<axk::SystemEndType>(value);
                const auto result = axk::patch_system_panel(file, patch, model);
                if (value <= (native ? 4U : 3U)) {
                    ASSERT_TRUE(result) << result.error().message;
                    auto expected = original;
                    expected[native ? 0x12fU : 0x33fU] = static_cast<std::byte>(value);
                    EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
                    EXPECT_EQ(axk::decode_system_panel(*result)->parameters.end_type, patch.end_type);
                    EXPECT_EQ(axk::patch_system_panel(*result, patch, model).value(), *result);
                } else {
                    ASSERT_FALSE(result);
                    EXPECT_EQ(result.error().code, axk::ErrorCode::invalid_argument);
                }
                EXPECT_EQ(axk::encode_system_file(file).value(), original);
            }
            EXPECT_EQ(axk::patch_system_panel(file, {}, model).value(), file);
        }
    }
}

TEST(SystemPanel, EndTypePatchRejectsMismatchedModelsAndInconsistentRetainedRecords) {
    axk::SystemPanelPatch patch;
    patch.end_type = axk::SystemEndType::beat;
    for (const bool native : {false, true}) {
        const auto file = retained_panel(native, 0, 255);
        const auto model = native ? axk::ASeriesModel::a3000 : axk::ASeriesModel::a4000;
        EXPECT_FALSE(axk::patch_system_panel(file, patch, static_cast<axk::ASeriesModel>(255)));
        EXPECT_FALSE(
            axk::patch_system_panel(file, patch, native ? axk::ASeriesModel::a4000 : axk::ASeriesModel::a3000));
        for (unsigned malformed = 0; malformed < 6U; ++malformed) {
            auto bad = file;
            if (malformed == 0U)
                bad.storage_revision = 2;
            else if (malformed == 1U)
                bad.system_bulk_bytes.pop_back();
            else if (malformed == 2U)
                bad.system_header_bytes[0] ^= std::byte{1};
            else if (malformed == 3U)
                bad.system_bulk_bytes[0x16U] ^= std::byte{1}; // Cached Omni context must agree.
            else if (malformed == 4U)
                bad.reserved_tail_bytes.push_back(std::byte{});
            else
                bad.kind = static_cast<axk::SystemFileKind>(255);
            const auto before = bad;
            EXPECT_FALSE(axk::patch_system_panel(bad, patch, model)) << malformed;
            EXPECT_FALSE(axk::patch_system_panel(bad, {}, model)) << malformed;
            EXPECT_EQ(bad, before);
        }
    }
}

TEST(SystemPanel, CustomPreferencePatchesValidateEveryEnumAndPreserveAllOtherBytes) {
    for (const auto model : {axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        const bool native = model == axk::ASeriesModel::a3000;
        for (std::uint8_t revision = 0; revision <= (native ? 0U : 1U); ++revision) {
            const auto file = retained_panel(native, revision, 255);
            const auto original = axk::encode_system_file(file).value();
            for (unsigned field = 0; field < 4U; ++field) {
                for (unsigned value = 0; value < 256U; ++value) {
                    SCOPED_TRACE(static_cast<unsigned>(model));
                    SCOPED_TRACE(revision);
                    SCOPED_TRACE(field);
                    SCOPED_TRACE(value);
                    axk::SystemPanelPatch patch;
                    if (field == 0U)
                        patch.function_selection = static_cast<axk::SystemFunctionSelection>(value);
                    else if (field == 1U)
                        patch.page_selection = static_cast<axk::SystemPageSelection>(value);
                    else if (field == 2U)
                        patch.note_display = static_cast<axk::SystemNoteDisplay>(value);
                    else
                        patch.layer_selection_scope = static_cast<axk::SystemLayerSelectionScope>(value);
                    const bool valid = field == 0U  ? value <= 2U
                                       : field < 3U ? value <= 1U
                                                    : value == 0U || value == (native ? 1U : 2U);
                    const auto result = axk::patch_system_panel(file, patch, model);
                    if (valid) {
                        ASSERT_TRUE(result) << result.error().message;
                        auto expected = original;
                        const auto index = std::array<std::size_t, 4>{8, 9, 10, 19}[field];
                        expected[(native ? 0x120U : 0x330U) + index] =
                            static_cast<std::byte>(field == 3U ? (value == 0U ? 0U : 1U) : value);
                        EXPECT_EQ(axk::encode_system_file(*result).value(), expected);
                        EXPECT_EQ(axk::patch_system_panel(*result, patch, model).value(), *result);
                        const auto p = axk::decode_system_panel(*result)->parameters;
                        EXPECT_EQ(p.function_selection, patch.function_selection);
                        EXPECT_EQ(p.page_selection, patch.page_selection);
                        EXPECT_EQ(p.note_display, patch.note_display);
                        EXPECT_EQ(p.layer_selection_scope, patch.layer_selection_scope);
                    } else {
                        ASSERT_FALSE(result);
                        EXPECT_EQ(result.error().code, axk::ErrorCode::invalid_argument);
                    }
                    EXPECT_EQ(axk::encode_system_file(file).value(), original);
                }
            }
        }
    }
}

TEST(SystemPanel, MixedPanelPatchIsAtomicWhenOnePreferenceIsInvalid) {
    for (const auto model : {axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        const auto file = retained_panel(model == axk::ASeriesModel::a3000, 0, 255);
        const auto original = axk::encode_system_file(file).value();
        axk::SystemFilePatch patch;
        patch.global.master_fine_tune = 7;
        patch.panel.end_type = axk::SystemEndType::beat;
        patch.panel.function_selection = axk::SystemFunctionSelection::hold;
        patch.panel.page_selection = axk::SystemPageSelection::last;
        patch.panel.note_display = axk::SystemNoteDisplay::number;
        patch.panel.layer_selection_scope = model == axk::ASeriesModel::a3000
                                                ? axk::SystemLayerSelectionScope::tree_page
                                                : axk::SystemLayerSelectionScope::selection_page;
        EXPECT_FALSE(axk::patch_system_panel(file, patch.panel, model));
        EXPECT_FALSE(axk::patch_system_file(file, patch, model));
        EXPECT_EQ(axk::encode_system_file(file).value(), original);
    }
}
