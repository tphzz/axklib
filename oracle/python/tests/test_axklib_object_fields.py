from axklib.parameters import current as object_fields


def test_pitch_base_formula_matches_formula_representatives() -> None:
    assert object_fields.estimated_pitch_base_word(60, 44100, 0) == 0x13AB
    assert object_fields.estimated_pitch_base_word(48, 48000, -4) == 0x0F32
    assert object_fields.estimated_pitch_base_word(60, 22050, 0) == 0x17AA
    assert object_fields.estimated_pitch_base_word(60, 12345, 0) == 0x1B03
    assert object_fields.estimated_pitch_base_word(60, 0, 0) is None


def test_pitch_base_status_distinguishes_inactive_and_stale_values() -> None:
    assert (
        object_fields.pitch_base_word_status(0x1600, 0x1600)
        == object_fields.PITCH_BASE_STATUS_MATCHES_FORMULA
    )
    assert (
        object_fields.pitch_base_word_status(0x1601, 0x1600)
        == object_fields.PITCH_BASE_STATUS_STORED_EXCEPTION
    )
    assert (
        object_fields.pitch_base_word_status(0x15BF, None, active=False)
        == object_fields.PITCH_BASE_STATUS_INACTIVE
    )


def test_decode_sbnk_program_link_bitmaps_uses_known_program_order() -> None:
    payload = bytearray(0x0D0)
    payload[0x0C0:0x0C4] = (0x80000001).to_bytes(4, "big")
    payload[0x0C4:0x0C8] = (0x00000001).to_bytes(4, "big")
    payload[0x0C8:0x0CC] = (0x00000001).to_bytes(4, "big")
    payload[0x0CC:0x0D0] = (0x00000001).to_bytes(4, "big")

    programs, words = object_fields.decode_sbnk_program_link_bitmaps(bytes(payload))

    assert programs == [1, 32, 33, 65, 97]
    assert words == (0x80000001, 0x00000001, 0x00000001, 0x00000001)


def test_iter_prog_assignments_decodes_current_easy_edit_rows() -> None:
    payload = bytearray(
        object_fields.PROG_ASSIGNMENT_START + object_fields.PROG_ASSIGNMENT_SIZE * 2
    )
    first = object_fields.PROG_ASSIGNMENT_START
    second = first + object_fields.PROG_ASSIGNMENT_SIZE
    payload[first : first + 16] = b"Bank01".ljust(16, b" ")
    payload[first + 0x10 : first + 0x14] = (0x12345678).to_bytes(4, "big")
    payload[first + 0x14] = 0x10
    payload[first + 0x15] = 0xFF
    payload[first + 0x16] = 0x13
    payload[first + 0x17] = 0xD4
    payload[first + 0x18] = 0xE1
    payload[first + 0x1D] = 0x01
    payload[first + 0x1E] = 72
    payload[first + 0x1F] = 60
    payload[first + 0x23] = 0xC1
    payload[first + 0x2D : first + 0x2F] = b"\x01\x17"
    payload[first + 0x33] = 0x01
    payload[second : second + 16] = b"Group".ljust(16, b" ")
    payload[second + 0x14] = 0x11
    payload[second + 0x15] = 0x05

    rows = object_fields.iter_prog_assignments(bytes(payload))

    assert len(rows) == 2
    assert rows[0].index == 0
    assert rows[0].offset == first
    assert rows[0].name == "Bank01"
    assert rows[0].raw_handle_0x10 == 0x12345678
    assert rows[0].kind_byte_0x14 == 0x10
    assert rows[0].flag_byte_0x15 == 0xFF
    assert rows[0].expected_category == "SBNK"
    assert rows[0].raw_row == bytes(payload[first : first + object_fields.PROG_ASSIGNMENT_SIZE])
    assert rows[0].midi_receive_channel_assign_0x15 == 0xFF
    assert rows[0].level_offset_0x16 == 19
    assert rows[0].velocity_sensitivity_0x17 == -44
    assert rows[0].pan_offset_0x18 == -31
    assert rows[0].output1_0x1d == 1
    assert rows[0].key_limit_high_0x1e == 72
    assert rows[0].key_limit_low_0x1f == 60
    assert rows[0].portamento_mono_key_xfade_flags_0x23 == 0xC1
    assert rows[0].reserved_0x2d_0x2e == b"\x01\x17"
    assert rows[0].midi_control_on_0x33 == 1
    assert object_fields.prog_assignment_offset(1) == second
    assert object_fields.prog_assignment_field_labels_for_offset(0x18) == ["pan_offset"]
    assert (
        object_fields.prog_assignment_field_labels_for_span(0x16, 0x19)
        == "level_offset;pan_offset;velocity_sensitivity"
    )
    assert object_fields.prog_assignment_row(bytes(payload), 0) == rows[0].raw_row
    assert rows[1].expected_category == "SBAC"


def test_shared_raw_field_helpers_preserve_bounds_behavior() -> None:
    payload = b"\x01\x80\x12\x34"

    assert object_fields.raw_u8(payload, 1) == 0x80
    assert object_fields.raw_u8(payload, 9) is None
    assert object_fields.raw_hex(0x80) == "0x80"
    assert object_fields.raw_hex(None) == ""
    assert object_fields.s8(0x80) == -128
    assert object_fields.be16(payload, 2) == 0x1234
    assert object_fields.be16(payload, 3) is None
    assert object_fields.be32(payload, 0) == 0x01801234
    assert object_fields.raw_slice_hex(payload, 1, 3) == "8012"
    assert len(object_fields.raw_slice_sha1(payload, 0, 4)) == 40


def test_decode_current_sbnk_members_keeps_relationship_labels_out() -> None:
    payload = bytearray(0x185)
    payload[0x32:0x42] = b"Bank".ljust(16, b" ")
    payload[0x50:0x68] = b"Instrument".ljust(24, b" ")
    payload[0x78:0x88] = b"Left".ljust(16, b" ")
    payload[0x88:0x98] = b"Right".ljust(16, b" ")
    payload[0xA0:0xA4] = (0x11111111).to_bytes(4, "big")
    payload[0xA4:0xA8] = (0x22222222).to_bytes(4, "big")
    payload[0xC0:0xC4] = (0x00000001).to_bytes(4, "big")
    payload[0xD6] = 60
    payload[0xD7] = 64
    payload[0xD8:0xDA] = (44100).to_bytes(2, "big")
    payload[0xDA:0xDC] = (48000).to_bytes(2, "big")
    payload[0xDC] = 0xFB
    payload[0xDD] = 3
    payload[0xDE:0xE0] = (0x1234).to_bytes(2, "big")
    payload[0xE0:0xE2] = (0x2345).to_bytes(2, "big")
    payload[0xF0:0xF4] = (1000).to_bytes(4, "big")
    payload[0xF4:0xF8] = (2000).to_bytes(4, "big")
    payload[0xF8:0xFC] = (100).to_bytes(4, "big")
    payload[0xFC:0x100] = (200).to_bytes(4, "big")
    payload[0x100:0x104] = (400).to_bytes(4, "big")
    payload[0x104:0x108] = (500).to_bytes(4, "big")

    decoded = object_fields.decode_current_sbnk_members(bytes(payload))

    assert decoded.bank_name == "Bank"
    assert decoded.instrument_name == "Instrument"
    assert decoded.bank_topology == "two-member"
    assert decoded.right_slot_present
    assert decoded.right_link_role == "sample-reference"
    assert decoded.linked_program_numbers == (1,)
    assert decoded.left.sample_name == "Left"
    assert decoded.left.smpl_link_id == 0x11111111
    assert decoded.left.root_key == 60
    assert decoded.left.sample_rate == 44100
    assert decoded.left.fine_tune_cents == -5
    assert decoded.left.pitch_base_word == 0x1234
    assert decoded.left.loop_length_frames == 400
    assert decoded.right is not None
    assert decoded.right.sample_name == "Right"
    assert decoded.right.smpl_link_id == 0x22222222
    assert decoded.right.sample_rate == 48000
    assert decoded.right.fine_tune_cents == 3
    assert decoded.inactive_right.sample_name == "Right"


def test_decode_current_sbac_fields_includes_value_enable_bitmaps() -> None:
    payload = bytearray(object_fields.SBAC_SLOT_START + object_fields.SBAC_SLOT_SIZE * 2)
    payload[
        object_fields.SBAC_VALUE_ENABLE_BITMAP_START : object_fields.SBAC_VALUE_ENABLE_BITMAP_START
        + 4
    ] = (0x80000005).to_bytes(4, "big")
    payload[
        object_fields.SBAC_VALUE_ENABLE_BITMAP_START
        + 4 : object_fields.SBAC_VALUE_ENABLE_BITMAP_START + 8
    ] = (0x00000003).to_bytes(4, "big")
    payload[
        object_fields.SBAC_VALUE_ENABLE_BITMAP_START
        + 8 : object_fields.SBAC_VALUE_ENABLE_BITMAP_START + 12
    ] = (0x03000001).to_bytes(4, "big")
    payload[object_fields.SBAC_BULK_ASSIGNED_SAMPLE_COUNT_OFFSET] = 9
    payload[object_fields.SBAC_SLOT_COUNT_OFFSET] = 1
    payload[object_fields.SBAC_SLOT_START : object_fields.SBAC_SLOT_START + 16] = b"Bank01".ljust(
        16, b" "
    )
    payload[object_fields.SBAC_SLOT_START + 16 : object_fields.SBAC_SLOT_START + 20] = (
        0x01020304
    ).to_bytes(4, "big")

    decoded = object_fields.decode_current_sbac_fields(bytes(payload))

    assert decoded.value_enable_words_0x120_0x12b == (0x80000005, 0x00000003, 0x03000001)
    assert decoded.enabled_sample_parameter_p2 == (0, 2, 31, 32, 33, 64, 88)
    assert decoded.enabled_sample_parameter_p2_over_table_range == (89,)
    assert decoded.bulk_assigned_sample_count_0x130 == 9
    assert decoded.active_slot_count_0x144 == 1
    assert decoded.max_slot_count_from_payload == 2
    assert len(decoded.slots) == 1
    assert decoded.slots[0].name == "Bank01"


def test_iter_sbac_slots_decodes_counted_current_slots() -> None:
    payload = bytearray(object_fields.SBAC_SLOT_START + object_fields.SBAC_SLOT_SIZE * 2)
    payload[object_fields.SBAC_SLOT_COUNT_OFFSET] = 2
    first = object_fields.SBAC_SLOT_START
    second = first + object_fields.SBAC_SLOT_SIZE
    payload[first : first + 16] = b"Bank01".ljust(16, b" ")
    payload[first + 16 : first + 20] = (0x01020304).to_bytes(4, "big")
    payload[second : second + 16] = b"Bank02".ljust(16, b" ")
    payload[second + 16 : second + 20] = (0x05060708).to_bytes(4, "big")

    slot_count, max_slots, slots = object_fields.iter_sbac_slots(bytes(payload))

    assert slot_count == 2
    assert max_slots == 2
    assert [slot.name for slot in slots] == ["Bank01", "Bank02"]
    assert slots[0].offset == first
    assert slots[0].raw_handle_0x10 == 0x01020304
    assert slots[1].raw_handle_0x10 == 0x05060708


def test_decode_prog_effect_common_blocks_keeps_effect_labels_out() -> None:
    payload = bytearray(0x110)
    block_start = 0x098
    payload[block_start : block_start + 8] = b"\x01\x5b\x49\xe1\x00\x82\x01\x01"
    payload[block_start + 0x08 : block_start + 0x0A] = (95).to_bytes(2, "big")
    payload[block_start + 0x0C : block_start + 0x0E] = (37).to_bytes(2, "big")
    payload[block_start + 0x0E : block_start + 0x10] = (58).to_bytes(2, "big")

    blocks = object_fields.iter_prog_effect_common_blocks(bytes(payload))
    effect1 = blocks[0]

    assert [block.effect_number for block in blocks] == [1, 2, 3]
    assert effect1.block_start == block_start
    assert effect1.active_or_bypass_u8 == 1
    assert effect1.input_level_u8 == 91
    assert effect1.output_level_u8 == 73
    assert effect1.pan_s8 == -31
    assert effect1.output_u8 == 0
    assert effect1.width_raw_s8 == -126
    assert effect1.width_display == -63
    assert effect1.type_u8 == 1
    assert effect1.type_mirror_or_reserved_u8 == 1
    assert effect1.parameter_value(1) == 95
    assert effect1.parameter_value(3) == 37
    assert effect1.parameter_value(4) == 58
    assert object_fields.prog_effect_parameter_offset(block_start, 3) == 0x0A4


def test_decode_prog_common_fields_includes_common_bytes_and_control_records() -> None:
    payload = bytearray(0x368)
    payload[0x080] = 0x40
    payload[0x081] = 0xC5
    payload[0x08F] = 0xFE
    payload[0x090] = 0x03
    payload[0x091] = 0x29
    payload[0x092] = 0x4D
    payload[0x093] = 0x3D
    payload[0x094] = 0x89
    payload[0x095] = 0x3C
    payload[0x068:0x078] = bytes(range(0x10))
    payload[0x082:0x086] = b"\x12\x34\x56\x78"
    payload[0x086] = 0x9A
    payload[0x087:0x08B] = b"\x01\x02\x03\x04"
    payload[0x096:0x098] = b"\xbe\xef"
    payload[0x110:0x120] = b"\x01\x05\x00\x17\x7d\x02\x01\xe1\x7c\x01\x02\x2d\x00\x00\x03\xf7"
    payload[0x358:0x368] = bytes(range(0x60, 0x70))

    common = object_fields.decode_prog_common_fields(bytes(payload))

    assert common.value("program_flags_ad_source_effect_connection_lfo_sync_0x080") == 0x40
    assert common.value("program_lfo_cycle_wave_initial_phase_0x081") == 0xC5
    assert common.value("program_lfo_reset_midi_channel_0x08f") == 0xFE
    assert common.value("program_portamento_type_0x090") == 0x03
    assert common.value("program_lfo_reset_note_0x095") == 0x3C
    assert common.raw_0x068_0x077.hex() == "000102030405060708090a0b0c0d0e0f"
    assert common.raw_0x082_0x085.hex() == "12345678"
    assert common.raw_0x086_u8 == 0x9A
    assert common.raw_0x087_0x08a.hex() == "01020304"
    assert common.raw_0x096_0x097.hex() == "beef"
    assert common.control_raw_0x110_0x11f.hex() == "010500177d0201e17c01022d000003f7"
    assert common.control_tail_raw_0x358_0x367.hex() == "606162636465666768696a6b6c6d6e6f"
    assert len(common.control_records) == 4
    assert common.control_records[0].device_u8 == 1
    assert common.control_records[0].function_u8 == 5
    assert common.control_records[0].type_u8 == 0
    assert common.control_records[0].range_s8 == 23
    assert common.control_records[1].range_s8 == -31
    assert common.control_records[3].type_u8 == 3
    assert common.control_records[3].range_s8 == -9
    assert (
        object_fields.prog_lfo_sync_raw(
            common.value("program_flags_ad_source_effect_connection_lfo_sync_0x080")
        )
        == 1
    )
    assert (
        object_fields.prog_effect_connection_raw(
            common.value("program_flags_ad_source_effect_connection_lfo_sync_0x080")
        )
        == 0
    )
    assert (
        object_fields.prog_lfo_cycle_raw(common.value("program_lfo_cycle_wave_initial_phase_0x081"))
        == 5
    )
    assert object_fields.label_from_map(5, object_fields.PROG_LFO_CYCLE_LABELS) == "q x 8"
    assert (
        object_fields.prog_lfo_wave_raw(common.value("program_lfo_cycle_wave_initial_phase_0x081"))
        == 0
    )
    assert (
        object_fields.prog_lfo_initial_phase_raw(
            common.value("program_lfo_cycle_wave_initial_phase_0x081")
        )
        == 3
    )
    assert object_fields.prog_reset_channel_label(0xFE) == "off"
    assert object_fields.prog_reset_channel_label(0x10) == "Bch"
    assert object_fields.prog_reset_channel_label(0x0F) == "16"
    assert (
        object_fields.prog_reset_note_display(0x3C, raw_reset_channel=0xFE, lfo_sync_raw=0) == "(-)"
    )
    assert (
        object_fields.prog_reset_note_display(0xFF, raw_reset_channel=0x00, lfo_sync_raw=0) == "all"
    )
    assert (
        object_fields.prog_reset_note_display(0x3C, raw_reset_channel=0x00, lfo_sync_raw=0) == "60"
    )
    assert object_fields.prog_portamento_effective_rate_time(3, 41, 77) == (77, "time_0x092")
    assert object_fields.prog_portamento_effective_rate_time(1, 41, 77) == (41, "rate_0x091")
    assert object_fields.prog_sample_and_hold_display(0x3D) == 62
    assert object_fields.prog_channel_bitmap_labels(0x0003) == ("A01", "A02")
    assert (
        object_fields.prog_control_record_summary(common.control_records[0])
        == "1:dev=0x01;fn=0x05;type=+offset;range=+23"
    )
    assert object_fields.prog_control_records_summary(common.control_records).startswith(
        "1:dev=0x01;fn=0x05"
    )


def test_decode_sbnk_member_parameter_window_keeps_labels_out() -> None:
    payload = bytearray(0x185)
    payload[0x0D0] = 0x07
    payload[0x0D1] = 0x94
    payload[0x0D2] = 0x02
    payload[0x0D3] = 0x01
    payload[0x0D4] = 7
    payload[0x0D5] = 0xF4
    payload[0x0D6] = 64
    payload[0x0D7] = 65
    payload[0x0D8:0x0DA] = (44100).to_bytes(2, "big")
    payload[0x0DA:0x0DC] = (48000).to_bytes(2, "big")
    payload[0x0DC] = 0xFB
    payload[0x0DD] = 3
    payload[0x0DE:0x0E0] = (0x1234).to_bytes(2, "big")
    payload[0x0E0:0x0E2] = (0x2345).to_bytes(2, "big")
    payload[0x0E2] = 72
    payload[0x0E3] = 60
    payload[0x0E6:0x0E8] = (12340).to_bytes(2, "big")
    payload[0x0E8:0x0EC] = (0x00002000).to_bytes(4, "big")
    payload[0x0EC:0x0F0] = (0x00003000).to_bytes(4, "big")
    payload[0x0F0:0x0F4] = (116476).to_bytes(4, "big")
    payload[0x0F4:0x0F8] = (116000).to_bytes(4, "big")
    payload[0x0F8:0x0FC] = (2048).to_bytes(4, "big")
    payload[0x0FC:0x100] = (4096).to_bytes(4, "big")
    payload[0x100:0x104] = (114428).to_bytes(4, "big")
    payload[0x104:0x108] = (112380).to_bytes(4, "big")
    payload[0x108] = 0xDB
    payload[0x109] = 15
    payload[0x10A] = 91
    payload[0x10B] = 23
    payload[0x10E] = 0xD3
    payload[0x110] = 0x23
    payload[0x111] = 0xE5
    payload[0x112] = 0xFB
    payload[0x116] = 80
    payload[0x117] = 0xE0
    payload[0x118] = 24
    payload[0x119] = 0xF4
    payload[0x11A] = 100
    payload[0x11B] = 40
    payload[0x120] = 64
    payload[0x121] = 5
    payload[0x122] = 33
    payload[0x123] = 69
    payload[0x124] = 80
    payload[0x125] = 0xE1
    payload[0x126] = 31
    payload[0x129] = 0xD8
    payload[0x131] = 17
    payload[0x134] = 0xDC
    payload[0x13C] = 23
    payload[0x141] = 67
    payload[0x145] = 37
    payload[0x146] = 2
    payload[0x147] = 72
    payload[0x149] = 0x07
    payload[0x151] = 0xF3
    payload[0x15C:0x160] = (0x0001C6FC).to_bytes(4, "big")
    payload[0x160:0x164] = (0x0001C6FC).to_bytes(4, "big")
    payload[0x164:0x168] = b"\x79\x05\x00\x17"
    payload[0x178:0x17C] = b"\x7e\x1f\x01\xf7"
    payload[0x17C] = 96
    payload[0x17D] = 32
    payload[0x17E] = 7
    payload[0x17F] = 90
    payload[0x180] = 2
    payload[0x181] = 45
    payload[0x182] = 4
    payload[0x183] = 37
    payload[0x184] = 91

    window = object_fields.decode_sbnk_member_parameter_window(bytes(payload))

    assert window.sample_parameter_base_0x0a8 == 0x0A8
    assert window.sample_flags_0x0d0 == 0x07
    assert window.mapout_flags_0x0d1 == 0x94
    assert window.coarse_tune_0x0d5 == -12
    assert window.left_root_key_0x0d6 == 64
    assert window.right_sample_rate_0x0da == 48000
    assert window.left_fine_tune_cents_0x0dc == -5
    assert window.pitch_base_word_0x0de == 0x1234
    assert window.loop_tempo_0x0e6 == 12340
    assert window.left_wave_start_low16_0x0ea == 0x2000
    assert window.left_wave_length_frames_0x0f0 == 116476
    assert window.left_loop_start_frame_0x0f8 == 2048
    assert window.start_address_velocity_sensitivity_0x108 == -37
    assert window.filter_type_0x109 == 15
    assert window.filter_cutoff_key_scaling_level1_0x10e == -45
    assert window.filter_q_width_velocity_sensitivity_0x111 == -27
    assert window.expand_detune_0x112 == -5
    assert window.sample_level_0x116 == 80
    assert window.pan_0x117 == -32
    assert window.velocity_offset_0x119 == -12
    assert window.velocity_range_high_0x11a == 100
    assert window.velocity_sensitivity_0x120 == 64
    assert window.sample_eq_frequency_0x122 == 33
    assert window.filter_cutoff_distance_0x125 == -31
    assert window.feg_attack_rate_0x126 == 31
    assert window.feg_init_level_0x129 == -40
    assert window.peg_attack_rate_0x131 == 17
    assert window.aeg_sustain_level_0x141 == 67
    assert window.lfo_flags_0x149 == 0x07
    assert window.filter_gain_0x151 == -13
    assert window.sample_control_records[0].offset == 0x164
    assert window.sample_control_records[0].device_u8 == 121
    assert window.sample_control_records[0].function_u8 == 5
    assert window.sample_control_records[0].range_s8 == 23
    assert window.sample_control_records[5].offset == 0x178
    assert window.sample_control_records[5].device_u8 == 126
    assert window.sample_control_records[5].range_s8 == -9
    assert window.velocity_xfade_high_0x17c == 96
    assert window.output1_0x17e == 7
    assert window.output2_level_0x181 == 45
    assert window.sample_portamento_type_0x182 == 4
    assert window.sample_portamento_time_0x184 == 91
    assert not hasattr(window, "filter_type_ui_label")
