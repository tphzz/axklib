#include <array>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <variant>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/lookups.hpp"
#include "axklib/object.hpp"
#include "axklib/sfs.hpp"

TEST(ObjectHeader, RejectsTruncationAndInvalidMagic) {
    const std::array<std::byte, 65> truncated{};
    const auto short_result = axk::decode_object_header(truncated);
    ASSERT_FALSE(short_result);
    EXPECT_EQ(short_result.error().code, axk::ErrorCode::container_truncated);

    std::array<std::byte, 66> invalid{};
    const auto invalid_result = axk::decode_object_header(invalid);
    ASSERT_FALSE(invalid_result);
    EXPECT_EQ(invalid_result.error().code, axk::ErrorCode::object_malformed);
}

TEST(CurrentLookups, ExposesCanonicalParameterAndProgramLabels) {
    EXPECT_EQ(axk::current_label(axk::CurrentLookup::sample_eq_frequency_ui_labels, 26), "630Hz");
    EXPECT_EQ(axk::current_label(axk::CurrentLookup::sample_control_function_ui_labels, 4), "Cutoff Bias");
    EXPECT_EQ(axk::current_label(axk::CurrentLookup::prog_slot_kind_target_category, 0x11), "SBAC");
    EXPECT_TRUE(axk::current_label(axk::CurrentLookup::prog_slot_kind_target_category, 0x7f).empty());
}

TEST(CurrentSmpl, MatchesMaintainedSemanticContract) {
    const auto path = std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/images/sampler-authored/"
                                                               "HD00_512_single_sbnk_authored.hds";
    const auto container = axk::open_image(path);
    ASSERT_TRUE(container);
    const auto payload =
        container->read_record_data(axk::PartitionIndex{0}, axk::SfsId{9}, std::numeric_limits<std::size_t>::max());
    ASSERT_TRUE(payload);
    const auto decoded = axk::decode_object(*payload);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->header.type, axk::ObjectType::smpl);
    EXPECT_EQ(decoded->header.raw_type, "SMPL");
    EXPECT_EQ(decoded->header.name, "sine wave");
    ASSERT_TRUE(std::holds_alternative<axk::CurrentSmpl>(decoded->payload));
    const auto &wave_data = std::get<axk::CurrentSmpl>(decoded->payload);
    EXPECT_EQ(wave_data.sample_rate.value, 48000U);
    EXPECT_EQ(wave_data.duplicate_sample_rate.value, 48000U);
    EXPECT_EQ(wave_data.stored_sample_width_bytes.value, 2U);
    EXPECT_EQ(wave_data.root_key.value, 66U);
    EXPECT_EQ(wave_data.fine_tune_cents.value, -20);
    EXPECT_EQ(wave_data.loop_mode.value, 1U);
    EXPECT_EQ(wave_data.loop_mode_label, "->0");
    EXPECT_EQ(wave_data.wave_length_frames.value, 128U);
    EXPECT_EQ(wave_data.loop_start_frame.value, 0U);
    EXPECT_EQ(wave_data.loop_length_frames.value, 128U);
    EXPECT_EQ(wave_data.loop_end_frame_inclusive, 127U);
    EXPECT_EQ(wave_data.loop_end_frame_exclusive, 128U);
    EXPECT_EQ(wave_data.stored_pcm_offset, decoded->header.header_size);
    EXPECT_EQ(wave_data.stored_pcm_bytes, decoded->header.payload_bytes_0x1c);
    EXPECT_EQ(wave_data.sample_rate.source.offset, 0x28U);
    EXPECT_EQ(wave_data.sample_rate.source.verification, axk::Verification::corroborated);
}

TEST(CurrentSmpl, RetainsMetadataOutsideDeclaredPcmAndPreservesWideDerivedLoopEnd) {
    std::vector<std::byte> payload(0xac);
    axk::ByteWriter writer{payload};
    ASSERT_TRUE(writer.write_ascii_field(0, 12, "FSFSDEV3SPLX", std::byte{}));
    ASSERT_TRUE(writer.write_ascii_field(0x0c, 4, "SMPL", std::byte{}));
    ASSERT_TRUE(writer.write_be32(0x10, 0xac));
    ASSERT_TRUE(writer.write_be32(0x1c, 1));
    ASSERT_TRUE(writer.write_be32(0x20, 1));
    ASSERT_TRUE(writer.write_ascii_field(0x32, 16, "bad", std::byte{}));
    ASSERT_TRUE(writer.write_be32(0x96, std::numeric_limits<std::uint32_t>::max()));
    ASSERT_TRUE(writer.write_be32(0x9a, 2));
    const auto outside = axk::decode_object(payload);
    ASSERT_TRUE(outside);
    const auto &wave_data = std::get<axk::CurrentSmpl>(outside->payload);
    EXPECT_EQ(wave_data.stored_pcm_offset, 0xacU);
    EXPECT_EQ(wave_data.stored_pcm_bytes, 1U);
    EXPECT_EQ(wave_data.loop_end_frame_inclusive, 4'294'967'296ULL);
    EXPECT_EQ(wave_data.loop_end_frame_exclusive, 4'294'967'297ULL);
}

TEST(CurrentSbnk, MatchesMaintainedContractAndPreservesInactiveRightLane) {
    const auto path = std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/images/sampler-authored/"
                                                               "HD00_512_single_sbnk_authored.hds";
    const auto container = axk::open_image(path);
    ASSERT_TRUE(container);
    const auto payload =
        container->read_record_data(axk::PartitionIndex{0}, axk::SfsId{10}, std::numeric_limits<std::size_t>::max());
    ASSERT_TRUE(payload);
    const auto decoded = axk::decode_object(*payload);
    ASSERT_TRUE(decoded);
    ASSERT_TRUE(std::holds_alternative<axk::CurrentSbnk>(decoded->payload));
    const auto &sample = std::get<axk::CurrentSbnk>(decoded->payload);
    EXPECT_EQ(sample.sample_name, "sine wave");
    EXPECT_FALSE(sample.right_slot_present);
    EXPECT_EQ(sample.right_link_role, "unused-zero");
    EXPECT_FALSE(sample.right);
    EXPECT_EQ(sample.left.wave_data_name, "sine wave");
    EXPECT_EQ(sample.left.cached_wave_data_reference_value, 23797180U);
    EXPECT_EQ(sample.left.root_key, 66U);
    EXPECT_EQ(sample.left.sample_rate, 48000U);
    EXPECT_EQ(sample.left.fine_tune_cents, -20);
    EXPECT_EQ(sample.left.wave_length_frames, 128U);
    EXPECT_EQ(sample.inactive_right.wave_data_name, "");
    EXPECT_EQ(sample.inactive_right.root_key, 66U);
    EXPECT_EQ(sample.inactive_right.pitch_base_word, 5442U);
    EXPECT_EQ(sample.sample_flags, 2U);
    EXPECT_EQ(sample.key_range_high, 127U);
    EXPECT_EQ(sample.key_range_low, 0U);
    EXPECT_EQ(sample.sample_level, 100U);
    EXPECT_EQ(sample.pan, 0);
    EXPECT_TRUE(sample.linked_program_numbers.empty());
    EXPECT_EQ(sample.control_records[0].device, 74U);
    EXPECT_EQ(sample.control_records[0].range, 32);
    EXPECT_EQ(sample.control_records[2].range, -32);
    EXPECT_EQ(sample.numeric_fields.size(), 105U);
    ASSERT_NE(sample.find_numeric_field("coarse_tune_0x0d5"), nullptr);
    EXPECT_EQ(sample.find_numeric_field("coarse_tune_0x0d5")->value, 0);
    ASSERT_NE(sample.find_numeric_field("left_sample_rate_0x0d8"), nullptr);
    EXPECT_EQ(sample.find_numeric_field("left_sample_rate_0x0d8")->value, 48000);
    ASSERT_NE(sample.find_numeric_field("sample_eq_gain_0x123"), nullptr);
    EXPECT_EQ(sample.find_numeric_field("sample_eq_gain_0x123")->value, 64);
    ASSERT_NE(sample.find_numeric_field("sample_portamento_time_0x184"), nullptr);
    EXPECT_EQ(sample.find_numeric_field("sample_portamento_time_0x184")->value, 90);
}

TEST(CurrentSbac, MatchesMaintainedSlotAndBitmapContracts) {
    const auto path = std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/images/sampler-authored/"
                                                               "HD00_512_single_sbnk_authored.hds";
    const auto container = axk::open_image(path);
    ASSERT_TRUE(container);
    const auto payload =
        container->read_record_data(axk::PartitionIndex{0}, axk::SfsId{23}, std::numeric_limits<std::size_t>::max());
    ASSERT_TRUE(payload);
    const auto decoded = axk::decode_object(*payload);
    ASSERT_TRUE(decoded);
    ASSERT_TRUE(std::holds_alternative<axk::CurrentSbac>(decoded->payload));
    const auto &sample_bank = std::get<axk::CurrentSbac>(decoded->payload);
    EXPECT_EQ(sample_bank.active_slot_count, 1U);
    EXPECT_EQ(sample_bank.maximum_slot_count, 9U);
    ASSERT_EQ(sample_bank.slots.size(), 1U);
    EXPECT_EQ(sample_bank.slots[0].name, "_NewSample");
    EXPECT_EQ(sample_bank.slots[0].raw_handle, 21249456U);
    EXPECT_EQ(sample_bank.value_enable_words[0], 2130756064U);
    EXPECT_EQ(sample_bank.enabled_parameter_numbers.front(), 5U);
    EXPECT_EQ(sample_bank.enabled_parameter_numbers.back(), 85U);
    EXPECT_EQ(sample_bank.enabled_numbers_outside_table, (std::vector<std::uint8_t>{89, 90, 91, 92, 93}));
}

TEST(CurrentProg, PreservesEmptyVisibleAndUnsupportedAssignmentRows) {
    std::vector<std::byte> payload(0x390);
    axk::ByteWriter writer{payload};
    ASSERT_TRUE(writer.write_ascii_field(0, 12, "FSFSDEV3SPLX", std::byte{}));
    ASSERT_TRUE(writer.write_ascii_field(0x0c, 4, "PROG", std::byte{}));
    ASSERT_TRUE(writer.write_ascii_field(0x32, 16, "001", std::byte{}));
    ASSERT_TRUE(writer.write_ascii_field(0x120, 16, "Sample Bank", std::byte{' '}));
    ASSERT_TRUE(writer.write_be32(0x130, 0x12345678));
    ASSERT_TRUE(writer.write_u8(0x134, 2));
    ASSERT_TRUE(writer.write_u8(0x135, 1));
    ASSERT_TRUE(writer.write_u8(0x136, 244));
    ASSERT_TRUE(writer.write_u8(0x138, 249));
    ASSERT_TRUE(writer.write_u8(0x13e, 100));
    ASSERT_TRUE(writer.write_u8(0x13f, 12));
    ASSERT_TRUE(writer.write_u8(0x141, 110));
    ASSERT_TRUE(writer.write_u8(0x142, 4));
    ASSERT_TRUE(writer.write_u8(0x16c, 0xff));

    const auto decoded = axk::decode_object(payload);
    ASSERT_TRUE(decoded);
    ASSERT_TRUE(std::holds_alternative<axk::CurrentProg>(decoded->payload));
    const auto &program = std::get<axk::CurrentProg>(decoded->payload);
    ASSERT_EQ(program.assignments.size(), 11U);
    EXPECT_EQ(program.assignments[0].name, "Sample Bank");
    EXPECT_EQ(program.assignments[0].raw_handle, 0x12345678U);
    EXPECT_EQ(program.assignments[0].kind, 2U);
    EXPECT_EQ(program.assignments[0].level_offset, -12);
    EXPECT_EQ(program.assignments[0].pan_offset, -7);
    EXPECT_EQ(program.assignments[0].key_limit_high, 100U);
    EXPECT_EQ(program.assignments[0].key_limit_low, 12U);
    EXPECT_EQ(program.assignments[0].velocity_limit_high, 110U);
    EXPECT_EQ(program.assignments[0].velocity_limit_low, 4U);
    EXPECT_TRUE(program.assignments[1].name.empty());
    EXPECT_EQ(program.control_records[0].device, 0U);
}

TEST(CurrentSummary, RetainsSequenceAndProfilePayloads) {
    for (const auto &[type, expected] : std::array{
             std::pair{"SEQU", axk::ObjectType::sequ},
             std::pair{"PRF3", axk::ObjectType::prf3},
         }) {
        std::vector<std::byte> payload(0x80);
        axk::ByteWriter writer{payload};
        ASSERT_TRUE(writer.write_ascii_field(0, 12, "FSFSDEV3SPLX", std::byte{}));
        ASSERT_TRUE(writer.write_ascii_field(0x0c, 4, type, std::byte{}));
        ASSERT_TRUE(writer.write_ascii_field(0x32, 16, "summary", std::byte{}));
        const auto decoded = axk::decode_object(payload);
        ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded->header.type, expected);
        if (expected == axk::ObjectType::sequ) {
            EXPECT_EQ(std::get<axk::CurrentSequence>(decoded->payload).raw_payload, payload);
        } else {
            EXPECT_EQ(std::get<axk::CurrentProfile>(decoded->payload).raw_payload, payload);
        }
    }
}
