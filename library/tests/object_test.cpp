#include <algorithm>
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

TEST(CurrentRecordEnvelope, DecodesTheSharedHeaderWithoutRequiringANamedObject) {
    std::array<std::byte, axk::current_record_envelope_size> bytes{};
    axk::ByteWriter writer{bytes};
    ASSERT_TRUE(writer.write_ascii_field(0U, 12U, "FSFSDEV3SPLX", std::byte{}));
    ASSERT_TRUE(writer.write_ascii_field(0x0cU, 4U, "PRF3", std::byte{}));
    ASSERT_TRUE(writer.write_be32(0x14U, 4U));
    ASSERT_TRUE(writer.write_be32(0x1cU, 0x1008U));

    const auto envelope = axk::decode_current_record_envelope(bytes);

    ASSERT_TRUE(envelope) << envelope.error().message;
    EXPECT_EQ(envelope->type, axk::ObjectType::prf3);
    EXPECT_EQ(envelope->raw_type, "PRF3");
    EXPECT_EQ(envelope->raw_bytes, bytes);
    EXPECT_FALSE(axk::decode_object_header(bytes));
}

TEST(CurrentLookups, ExposesCanonicalParameterAndProgramLabels) {
    EXPECT_EQ(axk::current_label(axk::CurrentLookup::sample_eq_frequency_ui_labels, 26), "630Hz");
    EXPECT_EQ(axk::current_label(axk::CurrentLookup::sample_control_function_ui_labels, 4), "Cutoff Bias");
    EXPECT_EQ(axk::current_label(axk::CurrentLookup::sample_control_device_ui_labels, 2), "002/BrthCtl");
    EXPECT_EQ(axk::current_label(axk::CurrentLookup::sample_control_device_ui_labels, 67), "067/SoftPdl");
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
    EXPECT_EQ(wave_data.wave_start_frame.value, 0U);
    EXPECT_EQ(wave_data.wave_start_frame.source.offset, 0x8eU);
    EXPECT_EQ(wave_data.wave_length_frames.value, 128U);
    EXPECT_EQ(wave_data.wave_end_frame_exclusive, 128U);
    EXPECT_EQ(wave_data.loop_start_frame.value, 0U);
    EXPECT_EQ(wave_data.loop_length_frames.value, 128U);
    EXPECT_EQ(wave_data.loop_end_frame_inclusive, 127U);
    EXPECT_EQ(wave_data.loop_end_frame_exclusive, 128U);
    EXPECT_EQ(wave_data.stored_pcm_offset, decoded->header.header_size);
    EXPECT_EQ(wave_data.stored_pcm_bytes, decoded->header.payload_bytes_0x1c);
    EXPECT_EQ(wave_data.stored_segment_offset, decoded->header.payload_offset_0x24);
    EXPECT_EQ(wave_data.stored_segment_bytes, decoded->header.payload_bytes_0x20);
    EXPECT_EQ(wave_data.stored_segment_offset, 0U);
    EXPECT_EQ(wave_data.stored_segment_bytes, wave_data.stored_pcm_bytes);
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
    ASSERT_TRUE(writer.write_ascii_field(0x54, 16, "Embedded Volume", std::byte{' '}));
    ASSERT_TRUE(writer.write_be32(0x74, 0x12345678U));
    ASSERT_TRUE(writer.write_u8(0x84, 0x30U));
    ASSERT_TRUE(writer.write_be32(0x8e, std::numeric_limits<std::uint32_t>::max()));
    ASSERT_TRUE(writer.write_be32(0x92, 2));
    ASSERT_TRUE(writer.write_be32(0x96, std::numeric_limits<std::uint32_t>::max()));
    ASSERT_TRUE(writer.write_be32(0x9a, 2));
    ASSERT_TRUE(writer.write_be16(0xaa, 3U));
    const auto outside = axk::decode_object(payload);
    ASSERT_TRUE(outside);
    const auto &wave_data = std::get<axk::CurrentSmpl>(outside->payload);
    EXPECT_EQ(wave_data.stored_pcm_offset, 0xacU);
    EXPECT_EQ(wave_data.stored_pcm_bytes, 1U);
    EXPECT_EQ(wave_data.stored_segment_offset, 0U);
    EXPECT_EQ(wave_data.stored_segment_bytes, 1U);
    EXPECT_EQ(wave_data.embedded_container_name.value, "Embedded Volume");
    EXPECT_EQ(wave_data.embedded_container_name.source.offset, 0x54U);
    EXPECT_EQ(wave_data.embedded_container_name.source.verification, axk::Verification::verified);
    EXPECT_EQ(wave_data.transient_name_hash_next_handle.value, 0x12345678U);
    EXPECT_EQ(wave_data.transient_name_hash_next_handle.source.offset, 0x74U);
    EXPECT_EQ(wave_data.pcm_transfer_control.value, 0x30U);
    EXPECT_EQ(wave_data.pcm_transfer_format_selector, 0x30U);
    EXPECT_EQ(wave_data.transient_512_byte_block_counter.value, 3U);
    EXPECT_EQ(wave_data.transient_512_byte_block_counter.source.offset, 0xaaU);
    EXPECT_EQ(wave_data.wave_end_frame_exclusive, 4'294'967'297ULL);
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
    EXPECT_EQ(sample.common.embedded_container_name.value, "");
    EXPECT_EQ(sample.common.embedded_container_name.source.offset, 0x54U);
    EXPECT_EQ(sample.common.embedded_container_name.source.size, 16U);
    EXPECT_EQ(sample.common.state_0x42.value, std::to_integer<std::uint8_t>((*payload)[0x42U]));
    EXPECT_EQ(sample.common.state_0x42.source.offset, 0x42U);
    EXPECT_EQ(sample.common.state_0x42.source.verification, axk::Verification::unknown);
    EXPECT_TRUE(sample.common.transient_name_hash_alias_matches);
    EXPECT_TRUE(sample.common.body_prefix_alias_matches);
    EXPECT_EQ(sample.common.transient_name_hash_alias.value, 0x01443c30U);
    EXPECT_EQ(sample.common.transient_name_hash_next_handle.value, 0x01443c30U);
    EXPECT_EQ(sample.common.body_prefix_alias.value, (std::array{std::byte{'s'}, std::byte{'i'}, std::byte{'n'}}));
    EXPECT_EQ(sample.common.saver_residue_0x43_0x49.value,
              (std::array{std::byte{0xb8}, std::byte{0}, std::byte{0x0a}, std::byte{0xf6}, std::byte{0x7a},
                          std::byte{0x01}, std::byte{0x54}}));
    EXPECT_FALSE(sample.right_slot_present);
    EXPECT_EQ(sample.right_link_role, "unused-zero");
    EXPECT_FALSE(sample.right);
    EXPECT_EQ(sample.left.wave_data_name, "sine wave");
    EXPECT_EQ(sample.left.cached_wave_data_reference_value, 23797180U);
    EXPECT_EQ(sample.left.root_key, 66U);
    EXPECT_EQ(sample.left.sample_rate, 48000U);
    EXPECT_EQ(sample.left.fine_tune_cents, -20);
    EXPECT_EQ(sample.left.wave_start_frame, 0U);
    EXPECT_EQ(sample.left.wave_length_frames, 128U);
    EXPECT_EQ(sample.loop_mode, 1U);
    EXPECT_EQ(sample.loop_mode_label, "->0");
    EXPECT_EQ(sample.inactive_right.wave_data_name, "");
    EXPECT_EQ(sample.inactive_right.root_key, 66U);
    EXPECT_EQ(sample.inactive_right.pitch_base_word, 5442U);
    EXPECT_EQ(sample.sample_flags, 2U);
    EXPECT_FALSE(sample.mono_mode);
    EXPECT_EQ(sample.key_range_high, 127U);
    EXPECT_EQ(sample.key_range_low, 0U);
    EXPECT_EQ(sample.sample_level, 100U);
    EXPECT_EQ(sample.pan, 0);
    EXPECT_TRUE(sample.linked_program_numbers.empty());
    EXPECT_EQ(sample.control_records[0].device, 74U);
    EXPECT_EQ(sample.control_records[0].range, 32);
    EXPECT_EQ(sample.control_records[2].range, -32);
    EXPECT_EQ(sample.control_record_storage_offset, 0x164U);
    EXPECT_TRUE(sample.control_record_tail_copy_present);
    ASSERT_TRUE(sample.control_record_copies_match);
    EXPECT_TRUE(*sample.control_record_copies_match);
    EXPECT_EQ(sample.raw_parameter_window.size(), 0xe0U);
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

TEST(CurrentSbnk, DecodesExtendedControllerTailAndReportsCopyMismatch) {
    const auto path = std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/images/sampler-authored/"
                                                               "HD00_512_single_sbnk_authored.hds";
    const auto container = axk::open_image(path);
    ASSERT_TRUE(container);
    auto payload =
        container->read_record_data(axk::PartitionIndex{0}, axk::SfsId{10}, std::numeric_limits<std::size_t>::max());
    ASSERT_TRUE(payload);
    payload->resize(0x188U);
    axk::ByteWriter writer{*payload};
    ASSERT_TRUE(writer.write_be32(0x1cU, 0x158U));
    (*payload)[0x164U] = std::byte{1};
    (*payload)[0x0d1U] |= std::byte{0x0b};
    (*payload)[0x182U] = std::byte{1};

    const auto decoded = axk::decode_object(*payload);

    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto &sample = std::get<axk::CurrentSbnk>(decoded->payload);
    EXPECT_EQ(sample.control_record_storage_offset, 0x164U);
    EXPECT_TRUE(sample.control_record_tail_copy_present);
    ASSERT_TRUE(sample.control_record_copies_match);
    EXPECT_FALSE(*sample.control_record_copies_match);
    ASSERT_EQ(sample.control_records.size(), 6U);
    EXPECT_EQ(sample.control_records[0].device, 1U);
    EXPECT_EQ(sample.control_records[0].function, 4U);
    EXPECT_EQ(sample.control_records[2].range, -32);
    EXPECT_TRUE(sample.mono_mode);
    EXPECT_TRUE(sample.uses_program_portamento);
    EXPECT_TRUE(sample.legacy_velocity_xfade_default_5);
    EXPECT_EQ(sample.raw_parameter_window.size(), 0xe0U);
}

TEST(CurrentSbnk, DecodesCompatibilityControllersWhenTheDeclaredObjectEndsBeforeTheTail) {
    const auto path = std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/images/sampler-authored/"
                                                               "HD00_512_single_sbnk_authored.hds";
    const auto container = axk::open_image(path);
    ASSERT_TRUE(container);
    auto payload =
        container->read_record_data(axk::PartitionIndex{0}, axk::SfsId{10}, std::numeric_limits<std::size_t>::max());
    ASSERT_TRUE(payload);
    payload->resize(0x164U);
    axk::ByteWriter writer{*payload};
    ASSERT_TRUE(writer.write_be32(0x1cU, 0x134U));

    const auto decoded = axk::decode_object(*payload);

    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto &sample = std::get<axk::CurrentSbnk>(decoded->payload);
    EXPECT_EQ(sample.control_record_storage_offset, 0x0a8U);
    EXPECT_FALSE(sample.control_record_tail_copy_present);
    EXPECT_FALSE(sample.control_record_copies_match);
    ASSERT_EQ(sample.control_records.size(), 6U);
    EXPECT_EQ(sample.control_records[0].device, 74U);
    EXPECT_EQ(sample.control_records[0].function, 4U);
    EXPECT_EQ(sample.control_records[2].range, -32);
    EXPECT_EQ(sample.raw_parameter_window.size(), 0xbcU);
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
    EXPECT_EQ(sample_bank.storage_layout, axk::SbacStorageLayout::current_split_parameter_tail);
    ASSERT_TRUE(sample_bank.parameter_tail_offset);
    EXPECT_EQ(*sample_bank.parameter_tail_offset, payload->size() - 0x24U);
    EXPECT_EQ(sample_bank.stored_member_count, 1U);
    EXPECT_EQ(sample_bank.maximum_member_count, 8U);
    ASSERT_EQ(sample_bank.slots.size(), 1U);
    EXPECT_EQ(sample_bank.slots[0].name, "_NewSample");
    EXPECT_EQ(sample_bank.slots[0].transient_member_pointer, 21249456U);
    EXPECT_EQ(sample_bank.pending_parameter_propagation_words, (std::array<std::uint32_t, 3>{0U, 0U, 0U}));
    EXPECT_TRUE(sample_bank.pending_parameter_numbers.empty());
    EXPECT_TRUE(sample_bank.reserved_pending_parameter_numbers.empty());
    EXPECT_EQ(sample_bank.effective_member_count, 1U);
    EXPECT_TRUE(sample_bank.slots[0].active);
    EXPECT_TRUE(std::ranges::equal(std::span{*payload}.subspan(0x78U, 0xbcU),
                                   std::span{sample_bank.raw_sample_parameter_block}.first(0xbcU)));
    EXPECT_TRUE(std::ranges::equal(std::span{*payload}.last(0x24U),
                                   std::span{sample_bank.raw_sample_parameter_block}.last(0x24U)));
}

TEST(CurrentSbac, ReconstructsLegacyParameterTailWithoutConsumingMemberRows) {
    std::vector<std::byte> payload(0x14cU + 2U * 0x14U);
    axk::ByteWriter writer{payload};
    ASSERT_TRUE(writer.write_ascii_field(0U, 12U, "FSFSDEV3SPLX", std::byte{}));
    ASSERT_TRUE(writer.write_ascii_field(0x0cU, 4U, "SBAC", std::byte{}));
    ASSERT_TRUE(writer.write_be32(0x14U, 2U));
    for (std::size_t index = 0; index < 0xbcU; ++index)
        payload[0x78U + index] = static_cast<std::byte>(index);
    ASSERT_TRUE(writer.write_be32(0x134U, 0x80000005U));
    ASSERT_TRUE(writer.write_be32(0x138U, 0x00000003U));
    ASSERT_TRUE(writer.write_be32(0x13cU, 0x03000001U));
    ASSERT_TRUE(writer.write_u8(0x144U, 2U));
    ASSERT_TRUE(writer.write_ascii_field(0x14cU, 16U, "Legacy One", std::byte{' '}));
    ASSERT_TRUE(writer.write_be32(0x15cU, 0x01020304U));
    ASSERT_TRUE(writer.write_ascii_field(0x160U, 16U, "Legacy Two", std::byte{' '}));
    ASSERT_TRUE(writer.write_be32(0x170U, 0x05060708U));

    const auto decoded = axk::decode_object(payload);

    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto &sample_bank = std::get<axk::CurrentSbac>(decoded->payload);
    EXPECT_EQ(sample_bank.storage_layout, axk::SbacStorageLayout::legacy_without_parameter_tail);
    EXPECT_FALSE(sample_bank.parameter_tail_offset);
    EXPECT_EQ(sample_bank.stored_member_count, 2U);
    EXPECT_EQ(sample_bank.maximum_member_count, 2U);
    EXPECT_EQ(sample_bank.pending_parameter_propagation_words,
              (std::array<std::uint32_t, 3>{0x80000005U, 0x00000003U, 0x03000001U}));
    EXPECT_EQ(sample_bank.pending_parameter_numbers, (std::vector<std::uint8_t>{0U, 2U, 31U, 32U, 33U, 64U, 88U}));
    EXPECT_EQ(sample_bank.reserved_pending_parameter_numbers, (std::vector<std::uint8_t>{89U}));
    EXPECT_EQ(sample_bank.effective_member_count, 2U);
    EXPECT_TRUE(std::ranges::equal(std::span{payload}.subspan(0x78U, 0xbcU),
                                   std::span{sample_bank.raw_sample_parameter_block}.first(0xbcU)));
    EXPECT_TRUE(std::ranges::all_of(std::span{sample_bank.raw_sample_parameter_block}.last(0x24U),
                                    [](std::byte value) { return value == std::byte{}; }));
    ASSERT_EQ(sample_bank.slots.size(), 2U);
    EXPECT_EQ(sample_bank.slots[0].name, "Legacy One");
    EXPECT_EQ(sample_bank.slots[1].name, "Legacy Two");
}

TEST(CurrentSbac, PreservesBlankCountedRowsAndDistinguishesEffectiveMembers) {
    constexpr std::size_t first_member_offset = 0x14cU;
    constexpr std::size_t member_size = 0x14U;
    std::vector<std::byte> payload(first_member_offset + 3U * member_size);
    axk::ByteWriter writer{payload};
    ASSERT_TRUE(writer.write_ascii_field(0U, 12U, "FSFSDEV3SPLX", std::byte{}));
    ASSERT_TRUE(writer.write_ascii_field(0x0cU, 4U, "SBAC", std::byte{}));
    ASSERT_TRUE(writer.write_be32(0x14U, 2U));
    ASSERT_TRUE(writer.write_u8(0x144U, 3U));
    ASSERT_TRUE(writer.write_ascii_field(first_member_offset, 16U, "First", std::byte{' '}));
    ASSERT_TRUE(writer.write_be32(first_member_offset + 0x10U, 0x0144'4068U));
    ASSERT_TRUE(writer.write_be32(first_member_offset + member_size + 0x10U, 0xdead'beefU));
    ASSERT_TRUE(writer.write_ascii_field(first_member_offset + 2U * member_size, 16U, "Third", std::byte{' '}));
    ASSERT_TRUE(writer.write_be32(first_member_offset + 2U * member_size + 0x10U, 0x0144'40c8U));

    const auto decoded = axk::decode_object(payload);

    ASSERT_TRUE(decoded) << decoded.error().message;
    const auto &sample_bank = std::get<axk::CurrentSbac>(decoded->payload);
    EXPECT_EQ(sample_bank.stored_member_count, 3U);
    EXPECT_EQ(sample_bank.effective_member_count, 2U);
    ASSERT_EQ(sample_bank.slots.size(), 3U);
    EXPECT_TRUE(sample_bank.slots[0].active);
    EXPECT_FALSE(sample_bank.slots[1].active);
    EXPECT_TRUE(sample_bank.slots[2].active);
    EXPECT_TRUE(sample_bank.slots[1].name.empty());
    EXPECT_EQ(sample_bank.slots[1].transient_member_pointer, 0xdead'beefU);
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
        std::vector<std::byte> payload(expected == axk::ObjectType::sequ ? 0x90U : 0x80U);
        axk::ByteWriter writer{payload};
        ASSERT_TRUE(writer.write_ascii_field(0, 12, "FSFSDEV3SPLX", std::byte{}));
        ASSERT_TRUE(writer.write_ascii_field(0x0c, 4, type, std::byte{}));
        ASSERT_TRUE(writer.write_ascii_field(0x32, 16, "summary", std::byte{}));
        if (expected == axk::ObjectType::sequ) {
            ASSERT_TRUE(writer.write_be16(0x7cU, 1U));
            ASSERT_TRUE(writer.write_be16(0x7eU, 96U));
            ASSERT_TRUE(writer.write_be32(0x80U, 0U));
            ASSERT_TRUE(writer.write_be32(0x84U, 0U));
            ASSERT_TRUE(writer.write_be16(0x88U, 1U));
            ASSERT_TRUE(writer.write_u8(0x8aU, 0xffU));
            ASSERT_TRUE(writer.write_u8(0x8bU, 0x2fU));
            ASSERT_TRUE(writer.write_u8(0x8cU, 0U));
            ASSERT_TRUE(writer.write_u8(0x8dU, 0xfdU));
        }
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
