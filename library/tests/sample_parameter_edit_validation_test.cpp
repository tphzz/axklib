#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/audio_export_wav_source.hpp"
#include "axklib/bytes.hpp"
#include "axklib/object.hpp"
#include "axklib/writer.hpp"
#include "axklib/writer_internal.hpp"

namespace {

TEST(SampleParameterEditBounds, VelocityEndpointsMustBeSevenBitValues) {
    axk::SampleParameters value;
    value.velocity_high = 128U;
    EXPECT_FALSE(axk::detail::validate_sample_parameters(value));
    value.velocity_high = 255U;
    value.velocity_low = 128U;
    EXPECT_FALSE(axk::detail::validate_sample_parameters(value));
}

TEST(SampleParameterEditBounds, FreshAuthoringUsesDefaultRootForOriginalKeyLimits) {
    axk::SampleSpec sample;
    sample.name = "Fresh";
    sample.parameters.key_low = 255U;
    sample.parameters.key_high = 50U;
    EXPECT_FALSE(axk::detail::prepare_sbnk_payload(sample, {"Wave", 0x100U, 44'100U, 64U}));
    sample.parameters.root_key = 40U;
    EXPECT_TRUE(axk::detail::prepare_sbnk_payload(sample, {"Wave", 0x100U, 44'100U, 64U}));
}

class SampleParameterEditValidation : public testing::Test {
  protected:
    std::vector<std::byte> payload;

    void SetUp() override {
        axk::SampleSpec sample;
        sample.name = "Preserved";
        sample.parameters.root_key = 60U;
        sample.parameters.loop_mode = axk::AudioSamplerLoopMode::forward_loop;
        sample.parameters.loop_start_frame = 10U;
        sample.parameters.loop_length_frames = 300U;
        const auto prepared = axk::detail::prepare_sbnk_payload(sample, {"Wave", 0x100U, 44'100U, 400U});
        ASSERT_TRUE(prepared) << prepared.error().message;
        payload = *prepared;
    }

    void expect_level_only_change() {
        const auto decoded = axk::decode_object(payload);
        ASSERT_TRUE(decoded) << decoded.error().message;
        ASSERT_TRUE(std::holds_alternative<axk::CurrentSbnk>(decoded->payload));
        auto expected = payload;
        expected[0x116U] = std::byte{87};
        axk::SampleParameters edits;
        edits.level = 87U;

        const auto changed = axk::detail::apply_sample_parameters_to_payload(payload, edits);

        ASSERT_TRUE(changed) << changed.error().message;
        EXPECT_EQ(payload, expected);
    }

    void expect_rejected_without_changes(const axk::SampleParameters &edits) {
        const auto before = payload;
        const auto changed = axk::detail::apply_sample_parameters_to_payload(payload, edits);
        EXPECT_FALSE(changed);
        EXPECT_EQ(payload, before);
    }

    void set_loop_beyond_member() {
        // The stored loop starts at 390 and lasts 300 frames in a 400-frame member.
        payload[0xf8U] = std::byte{0};
        payload[0xf9U] = std::byte{0};
        payload[0xfaU] = std::byte{1};
        payload[0xfbU] = std::byte{0x86};
    }

    void use_short_parameter_layout() {
        payload.resize(0x164U);
        axk::ByteWriter writer{payload};
        ASSERT_TRUE(writer.write_be32(0x14U, 2U));
        ASSERT_TRUE(writer.write_be32(0x18U, 0x134U));
        ASSERT_TRUE(writer.write_be32(0x1cU, 0U));
        const auto decoded = axk::decode_object(payload);
        ASSERT_TRUE(decoded) << decoded.error().message;
        const auto *sample = std::get_if<axk::CurrentSbnk>(&decoded->payload);
        ASSERT_NE(sample, nullptr);
        ASSERT_FALSE(sample->control_record_tail_copy_present);
        ASSERT_EQ(sample->control_record_storage_offset, 0xa8U);
        ASSERT_EQ(sample->control_records.size(), 6U);
        ASSERT_EQ(sample->raw_parameter_window.size(), 0xbcU);
    }

    void set_duplicate_source_expanded_state() {
        axk::SampleSpec sample;
        sample.name = "Preserved";
        const axk::detail::PreparedWaveformMember member{"Wave", 0x100U, 44'100U, 400U};
        const auto prepared = axk::detail::prepare_sbnk_payload(sample, member, member);
        ASSERT_TRUE(prepared) << prepared.error().message;
        payload = *prepared;
        // Synthetic retained state, not a fresh expanded-mono authoring contract.
        payload[0xd0U] |= std::byte{4};
        payload[0x112U] = std::byte{1};
        payload[0x113U] = std::byte{2};
        const auto decoded = axk::decode_object(payload);
        ASSERT_TRUE(decoded);
        const auto *current = std::get_if<axk::CurrentSbnk>(&decoded->payload);
        ASSERT_NE(current, nullptr);
        ASSERT_TRUE(current->right_slot_present);
        ASSERT_TRUE(current->right);
        EXPECT_EQ(current->right->wave_data_name, current->left.wave_data_name);
        EXPECT_EQ(current->right->cached_wave_data_reference_value, current->left.cached_wave_data_reference_value);
    }
};

TEST_F(SampleParameterEditValidation, LevelEditPreservesOriginalKeySentinelWithInvertedEffectiveRange) {
    payload[0xe3U] = std::byte{0xff};
    payload[0xe2U] = std::byte{50};

    expect_level_only_change();
}

TEST_F(SampleParameterEditValidation, PartialOriginalKeyRangeUsesTheStoredRoot) {
    axk::SampleParameters root;
    root.root_key = 40U;
    ASSERT_TRUE(axk::detail::apply_sample_parameters_to_payload(payload, root));
    axk::SampleParameters range;
    range.key_low = 255U;
    range.key_high = 50U;
    const auto updated = axk::detail::apply_sample_parameters_to_payload(payload, range);
    ASSERT_TRUE(updated) << updated.error().message;
    EXPECT_EQ(payload[0xd6U], std::byte{40});
    EXPECT_EQ(payload[0xe2U], std::byte{50});
    EXPECT_EQ(payload[0xe3U], std::byte{255});
}

TEST(SampleParameterAuthoring, ExplicitZeroLoopsPreserveNormalizedWindows) {
    for (const bool stereo : {false, true}) {
        for (const bool offset : {false, true}) {
            axk::SampleSpec sample;
            sample.name = "Window";
            sample.parameters.loop_mode = axk::AudioSamplerLoopMode::forward_one_shot;
            sample.parameters.loop_start_frame = 0U;
            sample.parameters.loop_length_frames = 0U;
            if (offset)
                sample.playback_window = axk::SamplePlaybackWindow{10U, 20U};
            const axk::detail::PreparedWaveformMember left{"Left", 0x100U, 44'100U, 64U};
            const axk::detail::PreparedWaveformMember right{"Right", 0x200U, 44'100U, 64U};
            const auto prepared =
                axk::detail::prepare_sbnk_payload(sample, left, stereo ? std::optional{right} : std::nullopt);
            ASSERT_TRUE(prepared) << prepared.error().message;
            const auto decoded = axk::decode_object(*prepared);
            ASSERT_TRUE(decoded);
            const auto &value = std::get<axk::CurrentSbnk>(decoded->payload);
            EXPECT_EQ(value.left.loop_start_frame, offset ? 10U : 0U);
            EXPECT_EQ(value.left.loop_length_frames, offset ? 20U : 64U);
            EXPECT_EQ(*axk::ByteReader{*prepared}.be32(0x160U), offset ? 30U : 64U);
            if (stereo) {
                ASSERT_TRUE(value.right);
                EXPECT_EQ(value.right->loop_start_frame, value.left.loop_start_frame);
                EXPECT_EQ(value.right->loop_length_frames, value.left.loop_length_frames);
                axk::SampleExport logical;
                logical.decoded = value;
                logical.key_high = 127U;
                axk::PhysicalWaveformExport physical;
                physical.waveform.format = {1U, 2U, 44'100U};
                physical.waveform.frame_count = 64U;
                physical.waveform.pcm.resize(128U);
                const auto exported = axk::audio_export_detail::stereo_sample_wav_source(logical, physical, physical);
                EXPECT_TRUE(exported.sampler.smpl);
                EXPECT_TRUE(exported.sampler.inst);
                EXPECT_TRUE(exported.warnings.empty());
            }
        }
    }
}

TEST_F(SampleParameterEditValidation, KeyAndRootEditsStillValidateOriginalKeySentinelDependencies) {
    payload[0xe3U] = std::byte{0xff};
    payload[0xe2U] = std::byte{50};
    axk::SampleParameters key_edit;
    key_edit.key_high = 55U;
    key_edit.level = 87U;
    expect_rejected_without_changes(key_edit);
    axk::SampleParameters root_edit;
    root_edit.root_key = 65U;
    root_edit.level = 87U;
    expect_rejected_without_changes(root_edit);
}

TEST_F(SampleParameterEditValidation, LevelEditPreservesUnknownLoopMode) {
    payload[0xe5U] = std::byte{0xff};

    expect_level_only_change();
}

TEST_F(SampleParameterEditValidation, LevelEditPreservesOutOfBoundsStoredLoopWindow) {
    set_loop_beyond_member();

    expect_level_only_change();
}

TEST_F(SampleParameterEditValidation, LoopEditsStillValidateMergedModeAndMemberWindowAtomically) {
    const auto original = payload;
    payload[0xe5U] = std::byte{0xff};
    axk::SampleParameters length_edit;
    length_edit.loop_length_frames = 200U;
    length_edit.level = 87U;
    expect_rejected_without_changes(length_edit);
    payload = original;
    set_loop_beyond_member();
    axk::SampleParameters mode_edit;
    mode_edit.loop_mode = axk::AudioSamplerLoopMode::forward_loop_release;
    mode_edit.level = 87U;
    expect_rejected_without_changes(mode_edit);
}

TEST_F(SampleParameterEditValidation, LevelEditPreservesDuplicateSourceExpandedState) {
    set_duplicate_source_expanded_state();
    ASSERT_FALSE(HasFatalFailure());

    expect_level_only_change();
}

TEST_F(SampleParameterEditValidation, ExpandEditsStillRejectUnsupportedNamedRightSlotTopologyAtomically) {
    set_duplicate_source_expanded_state();
    ASSERT_FALSE(HasFatalFailure());
    axk::SampleParameters detune_edit;
    detune_edit.expand_detune = 2;
    detune_edit.level = 87U;
    expect_rejected_without_changes(detune_edit);
    axk::SampleParameters dephase_edit;
    dephase_edit.expand_dephase = 3;
    dephase_edit.level = 87U;
    expect_rejected_without_changes(dephase_edit);
}

TEST_F(SampleParameterEditValidation, LevelEditPreservesUnrelatedInvertedScalingAndVelocityRanges) {
    payload[0x10cU] = std::byte{100};
    payload[0x10dU] = std::byte{20};
    payload[0x11cU] = std::byte{100};
    payload[0x11dU] = std::byte{20};
    payload[0x11aU] = std::byte{20};
    payload[0x11bU] = std::byte{100};

    expect_level_only_change();
}

TEST_F(SampleParameterEditValidation, ScalingAndVelocityEditsStillValidateUnchangedDependentEndpoints) {
    axk::SampleParameters edit;
    edit.filter_scaling_break1 = 126U;
    payload[0x10dU] = std::byte{50};
    expect_rejected_without_changes(edit);
    edit = {};
    edit.level_scaling_break1 = 126U;
    payload[0x10dU] = std::byte{127};
    payload[0x11dU] = std::byte{50};
    expect_rejected_without_changes(edit);
    edit = {};
    edit.velocity_low = 126U;
    payload[0x11dU] = std::byte{127};
    payload[0x11aU] = std::byte{50};
    expect_rejected_without_changes(edit);
}

TEST_F(SampleParameterEditValidation, ControllerEditsProjectCompleteRecordsWithBoundedLegacyFunctions) {
    const auto original = payload;
    for (std::size_t slot = 0; slot < 6U; ++slot) {
        for (std::uint8_t function = 0; function <= 36U; ++function) {
            for (std::size_t field = 0; field < 4U; ++field) {
                SCOPED_TRACE(slot);
                SCOPED_TRACE(function);
                SCOPED_TRACE(field);
                payload = original;
                const auto prefix = 0xa8U + 4U * slot;
                const auto tail = 0x164U + 4U * slot;
                const std::array canonical{std::byte{74}, static_cast<std::byte>(function), std::byte{1},
                                           std::byte{20}};
                for (std::size_t byte = 0; byte < 4U; ++byte) {
                    payload[prefix + byte] = std::byte{9};
                    payload[tail + byte] = canonical[byte];
                }
                auto expected = payload;
                axk::SampleParameters edit;
                auto &control = edit.controls[slot];
                if (field == 0U)
                    control.device = 71U;
                if (field == 1U)
                    control.function = static_cast<std::uint8_t>((function + 1U) % 37U);
                if (field == 2U)
                    control.type = 3U;
                if (field == 3U)
                    control.range = -20;
                expected[tail + field] = field == 0U   ? std::byte{71}
                                         : field == 1U ? static_cast<std::byte>(*control.function)
                                         : field == 2U ? std::byte{3}
                                                       : std::byte{236};
                for (std::size_t byte = 0; byte < 4U; ++byte)
                    expected[prefix + byte] = expected[tail + byte];
                if (std::to_integer<unsigned>(expected[prefix + 1U]) > 21U)
                    expected[prefix + 1U] = std::byte{0};
                ASSERT_TRUE(axk::detail::apply_sample_parameters_to_payload(payload, edit));
                EXPECT_EQ(payload, expected);
            }
        }
    }
}

TEST_F(SampleParameterEditValidation, NoopControllerEditsPreserveUnequalLegacyRecords) {
    payload[0xa8U] = std::byte{9};
    const auto original = payload;
    axk::SampleParameters edit;
    edit.controls[0].device = std::to_integer<std::uint8_t>(payload[0x164U]);
    ASSERT_TRUE(axk::detail::apply_sample_parameters_to_payload(payload, edit));
    EXPECT_EQ(payload, original);
    expect_level_only_change();
}

TEST_F(SampleParameterEditValidation, ShortLayoutEditsAllSixCompatibilityControllersWithoutGrowingTheObject) {
    use_short_parameter_layout();
    ASSERT_FALSE(HasFatalFailure());
    auto expected = payload;
    axk::SampleParameters edits;
    for (std::size_t index = 0; index < edits.controls.size(); ++index) {
        auto &control = edits.controls[index];
        control.device = static_cast<std::uint8_t>(10U + index);
        control.function = static_cast<std::uint8_t>(16U + index);
        control.type = 3U;
        control.range = static_cast<std::int8_t>(static_cast<int>(index) - 5);
        const auto offset = 0xa8U + index * 4U;
        expected[offset] = static_cast<std::byte>(*control.device);
        expected[offset + 1U] = static_cast<std::byte>(*control.function);
        expected[offset + 2U] = static_cast<std::byte>(*control.type);
        expected[offset + 3U] = static_cast<std::byte>(static_cast<std::uint8_t>(*control.range));
    }

    const auto changed = axk::detail::apply_sample_parameters_to_payload(payload, edits);

    ASSERT_TRUE(changed) << changed.error().message;
    EXPECT_EQ(payload, expected);
    const auto decoded = axk::decode_object(payload);
    ASSERT_TRUE(decoded);
    const auto *sample = std::get_if<axk::CurrentSbnk>(&decoded->payload);
    ASSERT_NE(sample, nullptr);
    ASSERT_EQ(sample->control_records.size(), edits.controls.size());
    EXPECT_FALSE(sample->control_record_tail_copy_present);
    for (std::size_t index = 0; index < edits.controls.size(); ++index) {
        EXPECT_EQ(sample->control_records[index].device, *edits.controls[index].device);
        EXPECT_EQ(sample->control_records[index].function, *edits.controls[index].function);
        EXPECT_EQ(sample->control_records[index].type, *edits.controls[index].type);
        EXPECT_EQ(sample->control_records[index].range, *edits.controls[index].range);
    }
}

TEST_F(SampleParameterEditValidation, NativeEditsKeepTheFormatAndExtensionEditsRequireExplicitConversion) {
    use_short_parameter_layout();
    ASSERT_FALSE(HasFatalFailure());
    std::vector<axk::SampleParameters> edits(9U);
    edits[0].velocity_xfade_high = 10U;
    edits[1].velocity_xfade_low = 10U;
    edits[2].output1_destination = 2U;
    edits[3].output1_level = 90U;
    edits[4].output2_destination = 1U;
    edits[5].output2_level = 90U;
    edits[6].portamento_type = 1U;
    edits[7].portamento_rate = 50U;
    edits[8].portamento_time = 50U;
    const auto original = payload;
    for (std::size_t index = 0; index < edits.size(); ++index) {
        SCOPED_TRACE(index);
        edits[index].level = 87U;
        edits[index].controls[5].device = 74U;
        payload = original;
        const auto changed = axk::detail::apply_sample_parameters_to_payload(payload, edits[index]);
        if (index < 2U || index > 6U) {
            EXPECT_FALSE(changed);
            EXPECT_EQ(payload, original);
            continue;
        }
        ASSERT_TRUE(changed) << changed.error().message;
        EXPECT_EQ(payload.size(), 0x164U);
        EXPECT_EQ(axk::ByteReader{payload}.be32(0x18U), 0x134U);
        EXPECT_EQ(axk::ByteReader{payload}.be32(0x1cU), 0U);
        EXPECT_EQ(payload[0x116U], std::byte{87});
        EXPECT_EQ(payload[0xa8U + 20U], std::byte{74});
        EXPECT_TRUE(std::equal(original.begin() + 0x20U, original.begin() + 0xa8U, payload.begin() + 0x20U));
    }
}

TEST_F(SampleParameterEditValidation, PaddedShortLayoutPreservesPaddingAndUsesPrefixControllers) {
    use_short_parameter_layout();
    ASSERT_FALSE(HasFatalFailure());
    payload.resize(0x200U, std::byte{0x5a});
    auto expected = payload;
    expected[0xa8U] = std::byte{74};
    expected[0x116U] = std::byte{87};
    axk::SampleParameters edit;
    edit.controls[0].device = 74U;
    edit.level = 87U;
    ASSERT_TRUE(axk::detail::apply_sample_parameters_to_payload(payload, edit));
    EXPECT_EQ(payload, expected);
    edit.output1_level = 80U;
    ASSERT_TRUE(axk::detail::apply_sample_parameters_to_payload(payload, edit));
    EXPECT_EQ(payload.size(), 0x200U);
    EXPECT_TRUE(
        std::all_of(payload.begin() + 0x164U, payload.end(), [](std::byte value) { return value == std::byte{0x5a}; }));
    EXPECT_EQ(payload[0x14eU], std::byte{80});
}

TEST_F(SampleParameterEditValidation, FailedShortLayoutUpgradeLeavesEveryByteUnchanged) {
    use_short_parameter_layout();
    axk::SampleParameters edit;
    edit.output1_level = 255U;
    expect_rejected_without_changes(edit);
}

} // namespace
