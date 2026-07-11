"""Current SBNK payload parser/serializer contract.

This module contains stable current-format SBNK object payload contract logic
used by reports and controlled sample check generation. It deliberately keeps
report row construction, object ownership heuristics, and CSV/JSON output out of
the library contract.
"""

from __future__ import annotations

from dataclasses import dataclass

from axklib.parameters import current as object_fields


def be16(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 2], "big")


def be32(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], "big")


def s8(value: int) -> int:
    return value - 0x100 if value >= 0x80 else value


def u32(value: int) -> int:
    return value & 0xFFFFFFFF


def u32_delta(stored: int, expected: int) -> int:
    return u32(stored) - u32(expected)


def smpl_unaligned_u32_at_0x092(data: bytes) -> int:
    return be32(data, 0x092)


ROOT_KEY_SEMITONE_FRACTIONS = object_fields.ROOT_KEY_SEMITONE_FRACTIONS
PITCH_REFERENCE_SAMPLE_RATE = object_fields.PITCH_REFERENCE_SAMPLE_RATE
PITCH_UNITS_PER_LN2 = object_fields.PITCH_UNITS_PER_LN2
PITCH_BASE_STATUS_MATCHES_FORMULA = object_fields.PITCH_BASE_STATUS_MATCHES_FORMULA
PITCH_BASE_STATUS_STORED_EXCEPTION = object_fields.PITCH_BASE_STATUS_STORED_EXCEPTION
PITCH_BASE_STATUS_FORMULA_UNAVAILABLE = object_fields.PITCH_BASE_STATUS_FORMULA_UNAVAILABLE
PITCH_BASE_STATUS_INACTIVE = object_fields.PITCH_BASE_STATUS_INACTIVE
CURRENT_OBJECT_MAGIC = b"FSFSDEV3SPLX"
CURRENT_SBNK_OBJECT_TYPE = b"SBNK"
CURRENT_SBNK_CONTRACT_PAYLOAD_SIZE = 0x200
CURRENT_SBNK_LOOP_CACHE_POLICY_PRESERVE_TEMPLATE = "preserve-template"
CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_ONE_SHOT = "single-member-one-shot"
CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_FORWARD = "single-member-forward"
CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_FORWARD_TO_ZERO = "single-member-forward-to-zero"
CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_FORWARD_TO_ZERO_FORWARD = (
    "single-member-forward-to-zero-forward"
)
CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_REVERSE = "single-member-reverse"
CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_ONE_SHOT_REVERSE = "single-member-one-shot-reverse"
CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_FORWARD = "two-member-forward"
CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_FORWARD_TO_ZERO = "two-member-forward-to-zero"
CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_FORWARD_TO_ZERO_FORWARD = (
    "two-member-forward-to-zero-forward"
)
CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_REVERSE = "two-member-reverse"
CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_ONE_SHOT = "two-member-one-shot"
CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_ONE_SHOT_REVERSE = "two-member-one-shot-reverse"
CURRENT_SBNK_SINGLE_MEMBER_INACTIVE_RIGHT_POLICY_PRESERVE_TEMPLATE = "preserve-template"
CURRENT_SBNK_SINGLE_MEMBER_INACTIVE_RIGHT_POLICY_GENERATED_MIRROR = "generated-mirror"
CURRENT_SBNK_SINGLE_MEMBER_INACTIVE_RIGHT_POLICY_ZERO = "zero"
CURRENT_SBNK_SINGLE_MEMBER_INACTIVE_RIGHT_POLICIES = {
    CURRENT_SBNK_SINGLE_MEMBER_INACTIVE_RIGHT_POLICY_PRESERVE_TEMPLATE,
    CURRENT_SBNK_SINGLE_MEMBER_INACTIVE_RIGHT_POLICY_GENERATED_MIRROR,
    CURRENT_SBNK_SINGLE_MEMBER_INACTIVE_RIGHT_POLICY_ZERO,
}
CURRENT_SBNK_LOOP_CACHE_POLICIES = {
    CURRENT_SBNK_LOOP_CACHE_POLICY_PRESERVE_TEMPLATE,
    CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_ONE_SHOT,
    CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_FORWARD,
    CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_FORWARD_TO_ZERO,
    CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_FORWARD_TO_ZERO_FORWARD,
    CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_REVERSE,
    CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_ONE_SHOT_REVERSE,
    CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_FORWARD,
    CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_FORWARD_TO_ZERO,
    CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_FORWARD_TO_ZERO_FORWARD,
    CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_REVERSE,
    CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_ONE_SHOT,
    CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_ONE_SHOT_REVERSE,
}
CURRENT_SBNK_SAMPLE_PARAMETER_BASE = object_fields.SBNK_SAMPLE_PARAMETER_BASE
CURRENT_SBNK_OBJECT_HEADER_DEFAULT_0X10_0X1F = bytes.fromhex("00000000000000040000013400000158")
CURRENT_SBNK_NAME_BLOCK_DEFAULT_PREFIX_0X30_0X31 = bytes.fromhex("100c")
CURRENT_SBNK_NAME_BLOCK_DEFAULT_SUFFIX_0X43_0X49 = bytes.fromhex("b8000af67a0154")
CURRENT_SBNK_OBJECT_HANDLE_DEFAULT = 0x01443C30
CURRENT_SBNK_MEMBER_PARAMETER_BASE_DEFAULT_0X0A8_0X0B7 = bytes.fromhex(
    "4a04012047050120490b01e0480c01e0"
)
CURRENT_SBNK_SINGLE_MEMBER_RESERVED_PLAYBACK_DEFAULT_0X152_0X156 = bytes.fromhex("c1e01e3a20")
CURRENT_SBNK_SINGLE_MEMBER_RESERVED_TONE_DEFAULT_0X158_0X15B = bytes.fromhex("3e20e1c6")
CURRENT_SINGLE_MEMBER_SBNK_DEFAULT_SAMPLE_CONTROL_RECORDS = (
    (74, 4, 1, 32),
    (71, 5, 1, 32),
    (73, 11, 1, -32),
    (72, 12, 1, -32),
    (0, 0, 0, 0),
    (0, 0, 0, 0),
)


def sbnk_sample_parameter_offset(sysex_sample_parameter_offset: int) -> int:
    return object_fields.sbnk_sample_parameter_offset(sysex_sample_parameter_offset)


SAMPLE_CONTROL_RECORD_COUNT = object_fields.SBNK_SAMPLE_CONTROL_RECORD_COUNT
SAMPLE_CONTROL_RECORD_BASE = object_fields.SBNK_SAMPLE_CONTROL_RECORD_BASE
SAMPLE_CONTROL_RECORD_SIZE = object_fields.SBNK_SAMPLE_CONTROL_RECORD_SIZE


def sample_control_record_offset(record_index: int, field_offset: int = 0) -> int:
    return object_fields.sbnk_sample_control_record_offset(record_index, field_offset)


def lfo_speed_ui_value(stored_byte: int) -> int:
    return stored_byte + 1


def lfo_delay_ui_value(stored_byte: int) -> int:
    return stored_byte


def sample_eq_gain_db(stored_byte: int) -> int:
    return stored_byte - 64


def sample_eq_width_ui_value(stored_byte: int) -> float:
    return stored_byte / 10.0


SAMPLE_EQ_FREQUENCY_UI_LABELS = (
    "32Hz",
    "36Hz",
    "40Hz",
    "45Hz",
    "50Hz",
    "56Hz",
    "63Hz",
    "70Hz",
    "80Hz",
    "90Hz",
    "100Hz",
    "110Hz",
    "125Hz",
    "140Hz",
    "160Hz",
    "180Hz",
    "200Hz",
    "225Hz",
    "250Hz",
    "280Hz",
    "315Hz",
    "355Hz",
    "400Hz",
    "450Hz",
    "500Hz",
    "560Hz",
    "630Hz",
    "700Hz",
    "800Hz",
    "900Hz",
    "1.0kHz",
    "1.1kHz",
    "1.2kHz",
    "1.4kHz",
    "1.6kHz",
    "1.8kHz",
    "2.0kHz",
    "2.2kHz",
    "2.5kHz",
    "2.8kHz",
    "3.2kHz",
    "3.6kHz",
    "4.0kHz",
    "4.5kHz",
    "5.0kHz",
    "5.6kHz",
    "6.3kHz",
    "7.0kHz",
    "8.0kHz",
    "9.0kHz",
    "10.0kHz",
    "11.0kHz",
    "12.0kHz",
    "14.0kHz",
    "16.0kHz",
)


def sample_eq_frequency_ui_label(stored_byte: int) -> str:
    index = stored_byte - 4
    if 0 <= index < len(SAMPLE_EQ_FREQUENCY_UI_LABELS):
        return SAMPLE_EQ_FREQUENCY_UI_LABELS[index]
    return ""


SAMPLE_EQ_TYPE_UI_LABELS = {
    0: "PeakDip",
    1: "LoShelv",
    2: "HiShelv",
}


def sample_eq_type_ui_label(stored_bits: int) -> str:
    return SAMPLE_EQ_TYPE_UI_LABELS.get(stored_bits, "")


# Source: Yamaha A4000/A5000 MIDI data format Note 11; the raw enum for
# sample-control function bytes. A4000 validation checks from
# HD00_512_smpctrl-records-01-06.hds confirm raw 2, 3, 6, 10, 25, and 36.
SAMPLE_CONTROL_FUNCTION_UI_LABELS = {
    0: "off",
    1: "PitchModDepth",
    2: "AmpModDepth",
    3: "CutoffMdDpth",
    4: "Cutoff Bias",
    5: "Filter Q/Width",
    6: "Pan Bias",
    7: "Pitch Bias",
    8: "Level",
    9: "LFO Speed",
    10: "LFO Delay",
    11: "AEG Attack Rate",
    12: "AEG Release Rate",
    13: "PEG Attack Rate",
    14: "PEG Release Rate",
    15: "FEG Attack Rate",
    16: "FEG Release Rate",
    17: "Pitch Bend",
    18: "Start Address",
    19: "FEG Level",
    20: "Cutoff Distance",
    21: "Filter Gain",
    22: "Portamento Rate/Time",
    23: "AEG Decay Rate",
    24: "AEG Sustain Level",
    25: "FEG Dcy Rate",
    26: "FEG Init Level",
    27: "FEG Sustain Level",
    28: "PEG Decay Rate",
    29: "PEG Init Level",
    30: "PEG Sustain Level",
    31: "Control1 Range",
    32: "Control2 Range",
    33: "Control3 Range",
    34: "Control4 Range",
    35: "Control5 Range",
    36: "Control6 Range",
}

SAMPLE_CONTROL_TYPE_UI_LABELS = (
    "+offset",
    "-/+offset",
    "+ofst(-exp)",
    "+ofst(+exp)",
)

SAMPLE_CONTROL_DEVICE_UI_LABELS = {
    1: "001/ModWhel",
    7: "007/Volume",
    10: "010/Pan",
    64: "064/Sustain",
    65: "---",
    66: "066/Sostenuto",
}


def sample_control_device_ui_label(stored_byte: int) -> str:
    if stored_byte in SAMPLE_CONTROL_DEVICE_UI_LABELS:
        return SAMPLE_CONTROL_DEVICE_UI_LABELS[stored_byte]
    if 0 <= stored_byte <= 120:
        return f"{stored_byte:03d}"
    if stored_byte == 121:
        return "AfterTouch"
    if stored_byte == 122:
        return "PitchBend"
    if stored_byte == 123:
        return "NoteNumber"
    if stored_byte == 124:
        return "Velocity"
    if stored_byte == 125:
        return "ProgramLFO"
    if stored_byte == 126:
        return "KeyOnRandom"
    return ""


def sample_control_function_ui_label(stored_byte: int) -> str:
    return SAMPLE_CONTROL_FUNCTION_UI_LABELS.get(stored_byte, "")


def sample_control_type_ui_label(stored_byte: int) -> str:
    if 0 <= stored_byte < len(SAMPLE_CONTROL_TYPE_UI_LABELS):
        return SAMPLE_CONTROL_TYPE_UI_LABELS[stored_byte]
    return ""


FILTER_TYPE_UI_LABELS = {
    15: "HPF+Peak",
}
# Output destination bytes are lane-specific in current SBNK records. Direct
# A4000 testing disproved a shared manual-order enum for SBNK+0x17e/0x180.
OUTPUT1_DESTINATION_UI_LABELS = {
    0: "off",
    1: "StereoOut",
    2: "E1-Through",
    3: "E2-Through",
    4: "E3-Through",
    5: "AssnOutL&R",
    6: "AssnOut1&2",
    7: "AssnOut3&4",
    8: "AssnOut5&6",
    9: "DIG&OPT",
    10: "StereoOut",
    11: "StereoOut",
    12: "StereoOut",
}
OUTPUT2_DESTINATION_UI_LABELS = {
    0: "off",
    1: "AssnOutL&R",
    2: "AssnOut1&2",
    3: "AssnOut3&4",
    4: "AssnOut5&6",
    5: "DIG&OPT",
    6: "StereoOut",
    7: "E1-Through",
    8: "E2-Through",
    9: "E3-Through",
    10: "StereoOut",
    11: "StereoOut",
    12: "StereoOut",
}
SAMPLE_PORTAMENTO_TYPE_UI_LABELS = {
    0: "off",
    1: "Pgm",
    2: "rate (fingered)",
    3: "rate (fulltime)",
    4: "time(fingered)",
    5: "time(fulltime)",
}
PITCH_BEND_TYPE_UI_LABELS = {
    0: "Normal",
    1: "Slow",
    2: "Slow&Rev",
    3: "Stop",
    4: "Stop&Rev",
    5: "Up2Dwn3",
    6: "Up2Dwn4",
    7: "Up2Dwn5",
    8: "Up2Dwn12",
    9: "Up3Dwn2",
    10: "Up3Dwn4",
    11: "Up3Dwn5",
    12: "Up3Dwn12",
}


def filter_type_ui_label(stored_byte: int) -> str:
    return FILTER_TYPE_UI_LABELS.get(stored_byte, "")


def output1_destination_ui_label(stored_byte: int) -> str:
    return OUTPUT1_DESTINATION_UI_LABELS.get(stored_byte, "")


def output2_destination_ui_label(stored_byte: int) -> str:
    return OUTPUT2_DESTINATION_UI_LABELS.get(stored_byte, "")


def sample_portamento_type_ui_label(stored_byte: int) -> str:
    return SAMPLE_PORTAMENTO_TYPE_UI_LABELS.get(stored_byte, "")


def pitch_bend_type_ui_label(stored_byte: int) -> str:
    return PITCH_BEND_TYPE_UI_LABELS.get(stored_byte, "")


def root_key_pitch_word(root_key: int) -> int:
    return object_fields.root_key_pitch_word(root_key)


def sample_rate_pitch_term(sample_rate: int) -> int | None:
    return object_fields.sample_rate_pitch_term(sample_rate)


def estimated_pitch_base_word(root_key: int, sample_rate: int, fine_tune: int) -> int | None:
    return object_fields.estimated_pitch_base_word(root_key, sample_rate, fine_tune)


def pitch_base_word_status(stored_word: int, clean_word: int | None, *, active: bool = True) -> str:
    return object_fields.pitch_base_word_status(stored_word, clean_word, active=active)


def clean_ascii(data: bytes) -> str:
    return "".join(chr(byte) if 0x20 <= byte < 0x7F else " " for byte in data).rstrip()


@dataclass(frozen=True)
class CurrentSbnkMemberSpec:
    sample_name: str
    smpl_link_id_0x078: int
    root_key_0x0d6: int
    sample_rate_0x0d8: int
    fine_tune_cents_0x0dc: int
    wave_length_frames_0x0f0: int
    loop_start_frame_0x0f8: int
    loop_length_frames_0x100: int


@dataclass(frozen=True)
class ParsedCurrentSbnkMember:
    sample_name: str
    smpl_link_id: int
    root_key: int
    sample_rate: int
    fine_tune_cents: int
    pitch_base_word: int
    clean_pitch_base_word_for_write: int | None
    pitch_base_word_status: str
    wave_length_frames: int
    loop_start_frame: int
    loop_length_frames: int


@dataclass(frozen=True)
class ParsedCurrentSbnkPayload:
    bank_name: str
    instrument_name: str
    bank_topology: str
    right_slot_present: bool
    sample_parameter_base_0x0a8: int
    linked_programs_001_032_bitmap_0x0c0: int
    linked_programs_033_064_bitmap_0x0c4: int
    linked_programs_065_096_bitmap_0x0c8: int
    linked_programs_097_128_bitmap_0x0cc: int
    sample_flags_0x0d0: int
    sample_bank_member_0x0d0_bit0: bool
    mono_sample_0x0d0_bit1: bool
    expanded_0x0d0_bit2: bool
    mapout_flags_0x0d1: int
    sample_eq_type_0x0d1_b7_6: int
    sample_eq_type_ui_label: str
    key_xfade_on_0x0d1_bit2: bool
    fixed_pitch_on_0x0d1_bit4: bool
    midi_receive_channel_0x0d2: int
    pitch_bend_type_0x0d3: int
    pitch_bend_type_ui_label: str
    pitch_bend_range_0x0d4: int
    coarse_tune_0x0d5: int
    key_range_high_0x0e2: int
    key_range_low_0x0e3: int
    loop_tempo_0x0e6: int
    left_wave_start_address_0x0e8: int
    left_wave_start_low16_0x0ea: int
    right_wave_start_address_0x0ec: int
    right_wave_start_low16_0x0ee: int
    start_address_velocity_sensitivity_0x108: int
    filter_type_0x109: int
    filter_type_ui_label: str
    filter_cutoff_0x10a: int
    filter_q_width_0x10b: int
    filter_cutoff_key_scaling_break1_0x10c: int
    filter_cutoff_key_scaling_break2_0x10d: int
    filter_cutoff_key_scaling_level1_0x10e: int
    filter_cutoff_key_scaling_level2_0x10f: int
    filter_cutoff_velocity_sensitivity_0x110: int
    filter_q_width_velocity_sensitivity_0x111: int
    expand_detune_0x112: int
    expand_dephase_0x113: int
    expand_width_0x114: int
    random_pitch_0x115: int
    level_scaling_break1_0x11c: int
    level_scaling_break2_0x11d: int
    level_scaling_level1_0x11e: int
    level_scaling_level2_0x11f: int
    filter_scaling_bp1_0x10c: int
    filter_scaling_bp2_0x10d: int
    filter_scaling_cutoff1_0x10e: int
    filter_scaling_cutoff2_0x10f: int
    filter_velocity_to_cutoff_0x110: int
    filter_velocity_to_q_width_0x111: int
    sample_level_0x116: int
    pan_0x117: int
    velocity_low_limit_0x118: int
    velocity_offset_0x119: int
    velocity_range_high_0x11a: int
    velocity_range_low_0x11b: int
    velocity_sensitivity_0x120: int
    alternate_group_0x121: int
    sample_eq_frequency_0x122: int
    sample_eq_frequency_ui_label: str
    sample_eq_gain_0x123: int
    sample_eq_gain_db: int
    sample_eq_width_0x124: int
    sample_eq_width_ui_value: float
    filter_cutoff_distance_0x125: int
    feg_attack_rate_0x126: int
    feg_decay_rate_0x127: int
    feg_release_rate_0x128: int
    feg_init_level_0x129: int
    feg_attack_level_0x12a: int
    feg_sustain_level_0x12b: int
    feg_release_level_0x12c: int
    feg_rate_key_scaling_0x12d: int
    feg_rate_velocity_sensitivity_0x12e: int
    feg_attack_level_velocity_sensitivity_0x12f: int
    feg_level_velocity_sensitivity_0x130: int
    peg_attack_rate_0x131: int
    peg_decay_rate_0x132: int
    peg_release_rate_0x133: int
    peg_init_level_0x134: int
    peg_attack_level_0x135: int
    peg_sustain_level_0x136: int
    peg_release_level_0x137: int
    peg_rate_key_scaling_0x138: int
    peg_rate_velocity_sensitivity_0x139: int
    peg_level_velocity_sensitivity_0x13a: int
    peg_range_0x13b: int
    aeg_attack_rate_0x13c: int
    aeg_decay_rate_0x13d: int
    aeg_release_rate_0x13e: int
    aeg_sustain_level_0x141: int
    aeg_attack_mode_0x143: int
    aeg_rate_key_scaling_0x144: int
    aeg_rate_velocity_sensitivity_0x145: int
    lfo_wave_0x146: int
    lfo_speed_0x147: int
    lfo_speed_ui_value: int
    lfo_delay_time_0x148: int
    lfo_delay_ui_value: int
    lfo_flags_0x149: int
    lfo_key_on_sync_0x149_bit0: bool
    lfo_cutoff_mod_phase_invert_0x149_bit1: bool
    lfo_pitch_mod_phase_invert_0x149_bit2: bool
    lfo_cutoff_mod_depth_0x14a: int
    lfo_pitch_mod_depth_0x14b: int
    lfo_amp_mod_depth_0x14c: int
    filter_gain_0x151: int
    wave_end_address_0x15c: int
    expected_wave_end_address_from_start_length: int
    wave_end_address_delta_from_expected: int
    loop_end_address_0x160: int
    expected_loop_end_address_from_start_length: int
    loop_end_address_delta_from_expected: int
    sample_control1_device_0x164: int
    sample_control1_device_ui_label: str
    sample_control1_function_0x165: int
    sample_control1_function_ui_label: str
    sample_control1_type_0x166: int
    sample_control1_type_ui_label: str
    sample_control1_range_0x167: int
    sample_control2_device_0x168: int
    sample_control2_device_ui_label: str
    sample_control2_function_0x169: int
    sample_control2_function_ui_label: str
    sample_control2_type_0x16a: int
    sample_control2_type_ui_label: str
    sample_control2_range_0x16b: int
    sample_control3_device_0x16c: int
    sample_control3_device_ui_label: str
    sample_control3_function_0x16d: int
    sample_control3_function_ui_label: str
    sample_control3_type_0x16e: int
    sample_control3_type_ui_label: str
    sample_control3_range_0x16f: int
    sample_control4_device_0x170: int
    sample_control4_device_ui_label: str
    sample_control4_function_0x171: int
    sample_control4_function_ui_label: str
    sample_control4_type_0x172: int
    sample_control4_type_ui_label: str
    sample_control4_range_0x173: int
    sample_control5_device_0x174: int
    sample_control5_device_ui_label: str
    sample_control5_function_0x175: int
    sample_control5_function_ui_label: str
    sample_control5_type_0x176: int
    sample_control5_type_ui_label: str
    sample_control5_range_0x177: int
    sample_control6_device_0x178: int
    sample_control6_device_ui_label: str
    sample_control6_function_0x179: int
    sample_control6_function_ui_label: str
    sample_control6_type_0x17a: int
    sample_control6_type_ui_label: str
    sample_control6_range_0x17b: int
    velocity_xfade_high_0x17c: int
    velocity_xfade_low_0x17d: int
    output1_0x17e: int
    output1_ui_label: str
    output1_level_0x17f: int
    output2_0x180: int
    output2_ui_label: str
    output2_level_0x181: int
    sample_portamento_type_0x182: int
    sample_portamento_type_ui_label: str
    sample_portamento_rate_0x183: int
    sample_portamento_time_0x184: int
    left: ParsedCurrentSbnkMember
    right: ParsedCurrentSbnkMember | None
    secondary_pitch_base_word_0x0e0: int
    secondary_pitch_base_word_status: str


def require_unsigned_range(name: str, value: int, max_value: int) -> None:
    if not 0 <= value <= max_value:
        raise ValueError(f"{name} out of range: {value}")


def require_signed_byte(name: str, value: int) -> None:
    if not -128 <= value <= 127:
        raise ValueError(f"{name} out of signed-byte range: {value}")


def put_be16(data: bytearray, offset: int, value: int) -> None:
    require_unsigned_range(f"u16 at 0x{offset:03x}", value, 0xFFFF)
    data[offset : offset + 2] = value.to_bytes(2, "big")


def put_be32(data: bytearray, offset: int, value: int) -> None:
    require_unsigned_range(f"u32 at 0x{offset:03x}", value, 0xFFFFFFFF)
    data[offset : offset + 4] = value.to_bytes(4, "big")


def put_s8(data: bytearray, offset: int, value: int) -> None:
    require_signed_byte(f"s8 at 0x{offset:03x}", value)
    data[offset] = value & 0xFF


def put_ascii_field(
    data: bytearray, offset: int, size: int, value: str, *, empty: bool = False
) -> None:
    if empty:
        data[offset : offset + size] = b"\x00" * size
        return
    encoded = value.encode("ascii")
    if len(encoded) > size:
        raise ValueError(f"ASCII field at 0x{offset:03x} is too long: {value!r}")
    data[offset : offset + size] = encoded.ljust(size, b" ")


def clean_pitch_base_word_for_member(member: CurrentSbnkMemberSpec) -> int:
    clean_word = estimated_pitch_base_word(
        member.root_key_0x0d6,
        member.sample_rate_0x0d8,
        member.fine_tune_cents_0x0dc,
    )
    if clean_word is None:
        raise ValueError("cannot serialize SBNK pitch-base word for non-positive sample rate")
    require_unsigned_range("clean pitch-base word", clean_word, 0xFFFF)
    return clean_word


def validate_current_sbnk_template(template: bytes, *, require_single_member: bool) -> None:
    if len(template) < CURRENT_SBNK_CONTRACT_PAYLOAD_SIZE:
        raise ValueError(f"SBNK template is too short: {len(template)} bytes")
    if template[0x00 : 0x00 + len(CURRENT_OBJECT_MAGIC)] != CURRENT_OBJECT_MAGIC:
        raise ValueError("SBNK template does not start with FSFSDEV3SPLX")
    if template[0x0C:0x10] != CURRENT_SBNK_OBJECT_TYPE:
        raise ValueError("SBNK template is not a current SBNK object")
    if require_single_member and clean_ascii(template[0x088:0x098]):
        raise ValueError("single-member SBNK template must have an empty right sample-name field")


def apply_current_single_member_sbnk_explicit_defaults(data: bytearray) -> None:
    data[0x10:0x20] = CURRENT_SBNK_OBJECT_HEADER_DEFAULT_0X10_0X1F
    data[0x30:0x32] = CURRENT_SBNK_NAME_BLOCK_DEFAULT_PREFIX_0X30_0X31
    data[0x43:0x4A] = CURRENT_SBNK_NAME_BLOCK_DEFAULT_SUFFIX_0X43_0X49
    put_be32(data, 0x68, CURRENT_SBNK_OBJECT_HANDLE_DEFAULT)
    put_be32(data, 0x98, CURRENT_SBNK_OBJECT_HANDLE_DEFAULT)
    data[0xA8:0xB8] = CURRENT_SBNK_MEMBER_PARAMETER_BASE_DEFAULT_0X0A8_0X0B7
    data[0x0D0] = 0x02
    data[0x0D1] = 0x00
    data[0x0E4] = 0x30
    data[0x0E5] = 0x01
    data[0x152:0x157] = CURRENT_SBNK_SINGLE_MEMBER_RESERVED_PLAYBACK_DEFAULT_0X152_0X156
    data[0x158:0x15C] = CURRENT_SBNK_SINGLE_MEMBER_RESERVED_TONE_DEFAULT_0X158_0X15B


def write_current_sbnk_sample_control_records(
    data: bytearray,
    records: tuple[tuple[int, int, int, int], ...],
) -> None:
    if len(records) != SAMPLE_CONTROL_RECORD_COUNT:
        raise ValueError(
            f"generated SBNK sample-control records require {SAMPLE_CONTROL_RECORD_COUNT} records"
        )
    for index, (device, function, control_type, control_range) in enumerate(records):
        require_unsigned_range(f"generated SBNK sample-control {index + 1} device", device, 0x7F)
        require_unsigned_range(
            f"generated SBNK sample-control {index + 1} function", function, 0x7F
        )
        require_unsigned_range(
            f"generated SBNK sample-control {index + 1} type", control_type, 0x03
        )
        if not -128 <= control_range <= 127:
            raise ValueError(
                f"generated SBNK sample-control {index + 1} range out of range: {control_range}"
            )
        offset = sample_control_record_offset(index + 1)
        data[offset] = device
        data[offset + 1] = function
        data[offset + 2] = control_type
        put_s8(data, offset + 3, control_range)


def apply_current_sbnk_single_member_inactive_right_policy(
    data: bytearray,
    *,
    left: CurrentSbnkMemberSpec,
    policy: str,
) -> None:
    if policy not in CURRENT_SBNK_SINGLE_MEMBER_INACTIVE_RIGHT_POLICIES:
        raise ValueError(f"unknown current SBNK inactive right-slot policy: {policy!r}")
    if policy == CURRENT_SBNK_SINGLE_MEMBER_INACTIVE_RIGHT_POLICY_PRESERVE_TEMPLATE:
        return
    put_ascii_field(data, 0x088, 16, "", empty=True)
    put_be32(data, 0x0A4, 0)
    if policy == CURRENT_SBNK_SINGLE_MEMBER_INACTIVE_RIGHT_POLICY_GENERATED_MIRROR:
        data[0x0D7] = left.root_key_0x0d6
        put_be16(data, 0x0DA, left.sample_rate_0x0d8)
        put_s8(data, 0x0DD, left.fine_tune_cents_0x0dc)
        put_be16(data, 0x0E0, clean_pitch_base_word_for_member(left))
        put_be32(data, 0x0F4, 0)
        put_be32(data, 0x0FC, 0)
        put_be32(data, 0x104, 0)
        return
    if policy == CURRENT_SBNK_SINGLE_MEMBER_INACTIVE_RIGHT_POLICY_ZERO:
        data[0x0D7] = 0
        put_be16(data, 0x0DA, 0)
        data[0x0DD] = 0
        put_be16(data, 0x0E0, 0)
        put_be32(data, 0x0EC, 0)
        put_be32(data, 0x0F4, 0)
        put_be32(data, 0x0FC, 0)
        put_be32(data, 0x104, 0)
        return
    raise AssertionError(f"unhandled current SBNK inactive right-slot policy: {policy!r}")


def serialize_current_single_member_sbnk_payload(
    *,
    bank_name: str,
    left: CurrentSbnkMemberSpec,
    instrument_name: str = "",
    inactive_right_policy: str = CURRENT_SBNK_SINGLE_MEMBER_INACTIVE_RIGHT_POLICY_ZERO,
    loop_cache_policy: str = CURRENT_SBNK_LOOP_CACHE_POLICY_PRESERVE_TEMPLATE,
    key_range_high_0x0e2: int | None = None,
    key_range_low_0x0e3: int | None = None,
    sample_level_0x116: int | None = None,
) -> bytes:
    """Serialize one generated current single-member SBNK from explicit model fields."""
    payload = bytearray(
        serialize_current_sbnk_contract_payload(
            bank_name=bank_name,
            left=left,
            right=None,
            instrument_name=instrument_name,
            template=None,
            allow_zero_inactive_right_slot_without_template=True,
            loop_cache_policy=loop_cache_policy,
            key_range_high_0x0e2=(127 if key_range_high_0x0e2 is None else key_range_high_0x0e2),
            key_range_low_0x0e3=(0 if key_range_low_0x0e3 is None else key_range_low_0x0e3),
            midi_receive_channel_0x0d2=0,
            pitch_bend_type_0x0d3=0,
            pitch_bend_range_0x0d4=2,
            coarse_tune_0x0d5=0,
            loop_tempo_0x0e6=9000,
            filter_type_0x109=0,
            filter_cutoff_0x10a=127,
            filter_q_width_0x10b=4,
            filter_cutoff_key_scaling_break1_0x10c=0,
            filter_cutoff_key_scaling_break2_0x10d=127,
            filter_cutoff_key_scaling_level1_0x10e=0,
            filter_cutoff_key_scaling_level2_0x10f=0,
            filter_cutoff_velocity_sensitivity_0x110=0,
            filter_q_width_velocity_sensitivity_0x111=0,
            expand_detune_0x112=0,
            expand_dephase_0x113=0,
            expand_width_0x114=63,
            random_pitch_0x115=0,
            sample_level_0x116=(100 if sample_level_0x116 is None else sample_level_0x116),
            pan_0x117=0,
            velocity_low_limit_0x118=0,
            velocity_offset_0x119=0,
            velocity_range_high_0x11a=127,
            velocity_range_low_0x11b=0,
            level_scaling_break1_0x11c=0,
            level_scaling_break2_0x11d=127,
            level_scaling_level1_0x11e=127,
            level_scaling_level2_0x11f=127,
            velocity_sensitivity_0x120=0,
            alternate_group_0x121=0,
            sample_eq_frequency_0x122=26,
            sample_eq_gain_0x123=64,
            sample_eq_width_0x124=10,
            filter_cutoff_distance_0x125=0,
            feg_attack_rate_0x126=127,
            feg_decay_rate_0x127=127,
            feg_release_rate_0x128=127,
            feg_init_level_0x129=0,
            feg_attack_level_0x12a=0,
            feg_sustain_level_0x12b=0,
            feg_release_level_0x12c=0,
            feg_rate_key_scaling_0x12d=0,
            feg_rate_velocity_sensitivity_0x12e=0,
            feg_attack_level_velocity_sensitivity_0x12f=0,
            feg_level_velocity_sensitivity_0x130=0,
            peg_attack_rate_0x131=127,
            peg_decay_rate_0x132=127,
            peg_release_rate_0x133=127,
            peg_init_level_0x134=0,
            peg_attack_level_0x135=0,
            peg_sustain_level_0x136=0,
            peg_release_level_0x137=0,
            peg_rate_key_scaling_0x138=0,
            peg_rate_velocity_sensitivity_0x139=0,
            peg_level_velocity_sensitivity_0x13a=0,
            peg_range_0x13b=12,
            aeg_attack_rate_0x13c=127,
            aeg_decay_rate_0x13d=127,
            aeg_release_rate_0x13e=126,
            aeg_sustain_level_0x141=127,
            aeg_attack_mode_0x143=0,
            aeg_rate_key_scaling_0x144=0,
            aeg_rate_velocity_sensitivity_0x145=0,
            lfo_wave_0x146=1,
            lfo_speed_0x147=39,
            lfo_delay_time_0x148=0,
            lfo_flags_0x149=1,
            lfo_cutoff_mod_depth_0x14a=0,
            lfo_pitch_mod_depth_0x14b=0,
            lfo_amp_mod_depth_0x14c=0,
            start_address_velocity_sensitivity_0x108=0,
            filter_gain_0x151=0,
            sample_control_records_0x164=CURRENT_SINGLE_MEMBER_SBNK_DEFAULT_SAMPLE_CONTROL_RECORDS,
            velocity_xfade_high_0x17c=0,
            velocity_xfade_low_0x17d=0,
            output1_0x17e=1,
            output1_level_0x17f=127,
            output2_0x180=0,
            output2_level_0x181=127,
            sample_portamento_type_0x182=0,
            sample_portamento_rate_0x183=90,
            sample_portamento_time_0x184=90,
        )
    )
    apply_current_single_member_sbnk_explicit_defaults(payload)
    apply_current_sbnk_single_member_inactive_right_policy(
        payload,
        left=left,
        policy=inactive_right_policy,
    )
    return bytes(payload)


def serialize_current_two_member_sbnk_payload(
    *,
    bank_name: str,
    left: CurrentSbnkMemberSpec,
    right: CurrentSbnkMemberSpec,
    instrument_name: str = "",
    key_range_high_0x0e2: int | None = None,
    key_range_low_0x0e3: int | None = None,
    sample_level_0x116: int | None = None,
) -> bytes:
    """Serialize a generated current two-member bank with explicit defaults."""
    payload = bytearray(
        serialize_current_single_member_sbnk_payload(
            bank_name=bank_name,
            left=left,
            instrument_name=instrument_name,
            inactive_right_policy=CURRENT_SBNK_SINGLE_MEMBER_INACTIVE_RIGHT_POLICY_ZERO,
            loop_cache_policy=CURRENT_SBNK_LOOP_CACHE_POLICY_PRESERVE_TEMPLATE,
            key_range_high_0x0e2=key_range_high_0x0e2,
            key_range_low_0x0e3=key_range_low_0x0e3,
            sample_level_0x116=sample_level_0x116,
        )
    )
    write_current_sbnk_member(
        payload,
        member=right,
        sample_name_offset=0x088,
        link_offset=0x0A4,
        root_key_offset=0x0D7,
        sample_rate_offset=0x0DA,
        fine_tune_offset=0x0DD,
        pitch_base_offset=0x0E0,
        wave_length_offset=0x0F4,
        loop_start_offset=0x0FC,
        loop_length_offset=0x104,
    )
    apply_current_sbnk_loop_cache_policy(
        payload,
        left=left,
        right=right,
        loop_cache_policy=CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_FORWARD_TO_ZERO,
    )
    return bytes(payload)


def write_current_sbnk_member(
    data: bytearray,
    *,
    member: CurrentSbnkMemberSpec,
    sample_name_offset: int,
    link_offset: int,
    root_key_offset: int,
    sample_rate_offset: int,
    fine_tune_offset: int,
    pitch_base_offset: int,
    wave_length_offset: int,
    loop_start_offset: int,
    loop_length_offset: int,
) -> None:
    require_unsigned_range("root key", member.root_key_0x0d6, 0xFF)
    put_ascii_field(data, sample_name_offset, 16, member.sample_name)
    put_be32(data, link_offset, member.smpl_link_id_0x078)
    data[root_key_offset] = member.root_key_0x0d6
    put_be16(data, sample_rate_offset, member.sample_rate_0x0d8)
    put_s8(data, fine_tune_offset, member.fine_tune_cents_0x0dc)
    put_be16(data, pitch_base_offset, clean_pitch_base_word_for_member(member))
    put_be32(data, wave_length_offset, member.wave_length_frames_0x0f0)
    put_be32(data, loop_start_offset, member.loop_start_frame_0x0f8)
    put_be32(data, loop_length_offset, member.loop_length_frames_0x100)


def apply_current_sbnk_loop_cache_policy(
    data: bytearray,
    *,
    left: CurrentSbnkMemberSpec,
    right: CurrentSbnkMemberSpec | None,
    loop_cache_policy: str,
) -> None:
    # Historical "loop cache" policy names write the low 16 bits of the
    # SysEx-aligned wave-start address fields: 0x0ea is the low half of the
    # left wave-start address at 0x0e8..0x0eb, and 0x0ee is the low half of the
    # right wave-start address at 0x0ec..0x0ef. The policy remains narrow and
    # sampler-sample-driven; the semantic loop-start fields are 0x0f8/0x0fc.
    if loop_cache_policy not in CURRENT_SBNK_LOOP_CACHE_POLICIES:
        raise ValueError(f"unknown current SBNK wave-start low-half policy: {loop_cache_policy!r}")
    if loop_cache_policy == CURRENT_SBNK_LOOP_CACHE_POLICY_PRESERVE_TEMPLATE:
        return
    if right is not None and loop_cache_policy == CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_FORWARD:
        require_unsigned_range(
            "two-member left wave-start low16 loop-load word",
            left.loop_start_frame_0x0f8,
            0xFFFF,
        )
        put_be16(data, 0x0EA, left.loop_start_frame_0x0f8)
        return
    if (
        right is not None
        and loop_cache_policy == CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_FORWARD_TO_ZERO
    ):
        require_unsigned_range(
            "two-member left wave-start low16 loop-load word",
            left.loop_start_frame_0x0f8,
            0xFFFF,
        )
        data[0x0E5] = 0x01
        put_be16(data, 0x0EA, left.loop_start_frame_0x0f8)
        return
    if (
        right is not None
        and loop_cache_policy == CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_FORWARD_TO_ZERO_FORWARD
    ):
        require_unsigned_range(
            "two-member left wave-start low16 loop-load word",
            left.loop_start_frame_0x0f8,
            0xFFFF,
        )
        data[0x0E5] = 0x02
        put_be16(data, 0x0EA, left.loop_start_frame_0x0f8)
        return
    if right is not None and loop_cache_policy == CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_REVERSE:
        require_unsigned_range(
            "two-member left wave-start low16 loop-load word",
            left.loop_start_frame_0x0f8,
            0xFFFF,
        )
        data[0x0E5] = 0x03
        put_be16(data, 0x0EA, left.loop_start_frame_0x0f8)
        return
    if (
        right is not None
        and loop_cache_policy == CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_ONE_SHOT
    ):
        require_unsigned_range(
            "two-member left wave-start low16 loop-load word",
            left.loop_start_frame_0x0f8,
            0xFFFF,
        )
        data[0x0E5] = 0x04
        put_be16(data, 0x0EA, left.loop_start_frame_0x0f8)
        return
    if (
        right is not None
        and loop_cache_policy == CURRENT_SBNK_LOOP_CACHE_POLICY_TWO_MEMBER_ONE_SHOT_REVERSE
    ):
        require_unsigned_range(
            "two-member left wave-start low16 loop-load word",
            left.loop_start_frame_0x0f8,
            0xFFFF,
        )
        data[0x0E5] = 0x05
        put_be16(data, 0x0EA, left.loop_start_frame_0x0f8)
        return
    if right is not None:
        raise ValueError(
            "single-member wave-start low-half policy cannot be used with a two-member SBNK"
        )
    require_unsigned_range(
        "single-member wave-start low16 loop-load word",
        left.loop_start_frame_0x0f8,
        0xFFFF,
    )
    if loop_cache_policy == CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_ONE_SHOT:
        data[0x0E5] = 0x04
        put_be16(data, 0x0EA, left.loop_start_frame_0x0f8)
        put_be16(data, 0x0EE, left.loop_start_frame_0x0f8)
        return
    if loop_cache_policy == CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_FORWARD:
        put_be16(data, 0x0EA, left.loop_start_frame_0x0f8)
        return
    if loop_cache_policy == CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_FORWARD_TO_ZERO:
        data[0x0E5] = 0x01
        put_be16(data, 0x0EA, left.loop_start_frame_0x0f8)
        return
    if loop_cache_policy == CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_FORWARD_TO_ZERO_FORWARD:
        data[0x0E5] = 0x02
        put_be16(data, 0x0EA, left.loop_start_frame_0x0f8)
        return
    if loop_cache_policy == CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_REVERSE:
        data[0x0E5] = 0x03
        put_be16(data, 0x0EA, left.loop_start_frame_0x0f8)
        return
    if loop_cache_policy == CURRENT_SBNK_LOOP_CACHE_POLICY_SINGLE_MEMBER_ONE_SHOT_REVERSE:
        data[0x0E5] = 0x05
        put_be16(data, 0x0EA, left.loop_start_frame_0x0f8)
        return
    raise AssertionError(
        f"unhandled current SBNK wave-start low-half policy: {loop_cache_policy!r}"
    )


def serialize_current_sbnk_contract_payload(
    *,
    bank_name: str,
    left: CurrentSbnkMemberSpec,
    right: CurrentSbnkMemberSpec | None = None,
    instrument_name: str = "",
    template: bytes | None = None,
    allow_zero_inactive_right_slot_without_template: bool = False,
    stored_pitch_base_word_0x0de: int | None = None,
    stored_secondary_pitch_base_word_0x0e0: int | None = None,
    loop_cache_policy: str = CURRENT_SBNK_LOOP_CACHE_POLICY_PRESERVE_TEMPLATE,
    key_range_high_0x0e2: int | None = None,
    key_range_low_0x0e3: int | None = None,
    midi_receive_channel_0x0d2: int | None = None,
    pitch_bend_type_0x0d3: int | None = None,
    pitch_bend_range_0x0d4: int | None = None,
    coarse_tune_0x0d5: int | None = None,
    loop_tempo_0x0e6: int | None = None,
    filter_type_0x109: int | None = None,
    filter_cutoff_0x10a: int | None = None,
    filter_q_width_0x10b: int | None = None,
    filter_cutoff_key_scaling_break1_0x10c: int | None = None,
    filter_cutoff_key_scaling_break2_0x10d: int | None = None,
    filter_cutoff_key_scaling_level1_0x10e: int | None = None,
    filter_cutoff_key_scaling_level2_0x10f: int | None = None,
    filter_cutoff_velocity_sensitivity_0x110: int | None = None,
    filter_q_width_velocity_sensitivity_0x111: int | None = None,
    expand_detune_0x112: int | None = None,
    expand_dephase_0x113: int | None = None,
    expand_width_0x114: int | None = None,
    random_pitch_0x115: int | None = None,
    level_scaling_break1_0x11c: int | None = None,
    level_scaling_break2_0x11d: int | None = None,
    level_scaling_level1_0x11e: int | None = None,
    level_scaling_level2_0x11f: int | None = None,
    sample_level_0x116: int | None = None,
    pan_0x117: int | None = None,
    velocity_low_limit_0x118: int | None = None,
    velocity_offset_0x119: int | None = None,
    velocity_range_high_0x11a: int | None = None,
    velocity_range_low_0x11b: int | None = None,
    velocity_sensitivity_0x120: int | None = None,
    alternate_group_0x121: int | None = None,
    sample_eq_frequency_0x122: int | None = None,
    sample_eq_gain_0x123: int | None = None,
    sample_eq_width_0x124: int | None = None,
    filter_cutoff_distance_0x125: int | None = None,
    feg_attack_rate_0x126: int | None = None,
    feg_decay_rate_0x127: int | None = None,
    feg_release_rate_0x128: int | None = None,
    feg_init_level_0x129: int | None = None,
    feg_attack_level_0x12a: int | None = None,
    feg_sustain_level_0x12b: int | None = None,
    feg_release_level_0x12c: int | None = None,
    feg_rate_key_scaling_0x12d: int | None = None,
    feg_rate_velocity_sensitivity_0x12e: int | None = None,
    feg_attack_level_velocity_sensitivity_0x12f: int | None = None,
    feg_level_velocity_sensitivity_0x130: int | None = None,
    peg_attack_rate_0x131: int | None = None,
    peg_decay_rate_0x132: int | None = None,
    peg_release_rate_0x133: int | None = None,
    peg_init_level_0x134: int | None = None,
    peg_attack_level_0x135: int | None = None,
    peg_sustain_level_0x136: int | None = None,
    peg_release_level_0x137: int | None = None,
    peg_rate_key_scaling_0x138: int | None = None,
    peg_rate_velocity_sensitivity_0x139: int | None = None,
    peg_level_velocity_sensitivity_0x13a: int | None = None,
    peg_range_0x13b: int | None = None,
    aeg_attack_rate_0x13c: int | None = None,
    aeg_decay_rate_0x13d: int | None = None,
    aeg_release_rate_0x13e: int | None = None,
    aeg_sustain_level_0x141: int | None = None,
    aeg_attack_mode_0x143: int | None = None,
    aeg_rate_key_scaling_0x144: int | None = None,
    aeg_rate_velocity_sensitivity_0x145: int | None = None,
    lfo_wave_0x146: int | None = None,
    lfo_speed_0x147: int | None = None,
    lfo_delay_time_0x148: int | None = None,
    lfo_flags_0x149: int | None = None,
    lfo_cutoff_mod_depth_0x14a: int | None = None,
    lfo_pitch_mod_depth_0x14b: int | None = None,
    lfo_amp_mod_depth_0x14c: int | None = None,
    start_address_velocity_sensitivity_0x108: int | None = None,
    filter_gain_0x151: int | None = None,
    sample_control_records_0x164: tuple[tuple[int, int, int, int], ...] | None = None,
    velocity_xfade_high_0x17c: int | None = None,
    velocity_xfade_low_0x17d: int | None = None,
    output1_0x17e: int | None = None,
    output1_level_0x17f: int | None = None,
    output2_0x180: int | None = None,
    output2_level_0x181: int | None = None,
    sample_portamento_type_0x182: int | None = None,
    sample_portamento_rate_0x183: int | None = None,
    sample_portamento_time_0x184: int | None = None,
) -> bytes:
    """Serialize current-SBNK contract fields plus explicitly labeled diagnostic fields."""
    if (
        stored_pitch_base_word_0x0de is not None
        or stored_secondary_pitch_base_word_0x0e0 is not None
    ):
        raise ValueError(
            "stored pitch-base origin fields are diagnostic fields and not write inputs"
        )
    if right is None and template is None and not allow_zero_inactive_right_slot_without_template:
        raise ValueError(
            "single-member SBNK serialization requires a template for inactive right-slot bytes"
        )
    if template is None:
        data = bytearray(CURRENT_SBNK_CONTRACT_PAYLOAD_SIZE)
    else:
        validate_current_sbnk_template(template, require_single_member=right is None)
        data = bytearray(template[:CURRENT_SBNK_CONTRACT_PAYLOAD_SIZE])

    data[0x00 : 0x00 + len(CURRENT_OBJECT_MAGIC)] = CURRENT_OBJECT_MAGIC
    data[0x0C:0x10] = CURRENT_SBNK_OBJECT_TYPE
    put_ascii_field(data, 0x32, 16, bank_name)
    put_ascii_field(data, 0x50, 24, instrument_name)
    write_current_sbnk_member(
        data,
        member=left,
        sample_name_offset=0x078,
        link_offset=0x0A0,
        root_key_offset=0x0D6,
        sample_rate_offset=0x0D8,
        fine_tune_offset=0x0DC,
        pitch_base_offset=0x0DE,
        wave_length_offset=0x0F0,
        loop_start_offset=0x0F8,
        loop_length_offset=0x100,
    )
    if right is None:
        put_ascii_field(data, 0x088, 16, "", empty=True)
        put_be32(data, 0x0A4, 0)
    else:
        write_current_sbnk_member(
            data,
            member=right,
            sample_name_offset=0x088,
            link_offset=0x0A4,
            root_key_offset=0x0D7,
            sample_rate_offset=0x0DA,
            fine_tune_offset=0x0DD,
            pitch_base_offset=0x0E0,
            wave_length_offset=0x0F4,
            loop_start_offset=0x0FC,
            loop_length_offset=0x104,
        )
    apply_current_sbnk_loop_cache_policy(
        data,
        left=left,
        right=right,
        loop_cache_policy=loop_cache_policy,
    )
    if (key_range_high_0x0e2 is None) != (key_range_low_0x0e3 is None):
        raise ValueError("generated SBNK key range requires both high and low values")
    if key_range_high_0x0e2 is not None and key_range_low_0x0e3 is not None:
        require_unsigned_range("generated SBNK key-range low", key_range_low_0x0e3, 0x7F)
        require_unsigned_range("generated SBNK key-range high", key_range_high_0x0e2, 0x7F)
        if key_range_high_0x0e2 < key_range_low_0x0e3:
            raise ValueError(
                f"generated SBNK key-range high {key_range_high_0x0e2} is below low {key_range_low_0x0e3}"
            )
        data[0x0E2] = key_range_high_0x0e2
        data[0x0E3] = key_range_low_0x0e3
    if midi_receive_channel_0x0d2 is not None:
        require_unsigned_range(
            "generated SBNK MIDI receive channel", midi_receive_channel_0x0d2, 16
        )
        data[0x0D2] = midi_receive_channel_0x0d2
    if pitch_bend_type_0x0d3 is not None:
        require_unsigned_range("generated SBNK pitch bend type", pitch_bend_type_0x0d3, 12)
        data[0x0D3] = pitch_bend_type_0x0d3
    if pitch_bend_range_0x0d4 is not None:
        require_unsigned_range("generated SBNK pitch bend range", pitch_bend_range_0x0d4, 12)
        data[0x0D4] = pitch_bend_range_0x0d4
    if coarse_tune_0x0d5 is not None:
        if not -64 <= coarse_tune_0x0d5 <= 63:
            raise ValueError(f"generated SBNK coarse tune out of range: {coarse_tune_0x0d5}")
        put_s8(data, 0x0D5, coarse_tune_0x0d5)
    if loop_tempo_0x0e6 is not None:
        if not 8000 <= loop_tempo_0x0e6 <= 15999:
            raise ValueError(f"generated SBNK loop tempo out of range: {loop_tempo_0x0e6}")
        put_be16(data, 0x0E6, loop_tempo_0x0e6)
    if filter_type_0x109 is not None:
        require_unsigned_range("generated SBNK filter type", filter_type_0x109, 0x7F)
        data[0x109] = filter_type_0x109
    if filter_cutoff_0x10a is not None:
        require_unsigned_range("generated SBNK filter cutoff", filter_cutoff_0x10a, 0x7F)
        data[0x10A] = filter_cutoff_0x10a
    if filter_q_width_0x10b is not None:
        require_unsigned_range("generated SBNK filter Q/width", filter_q_width_0x10b, 0x1F)
        data[0x10B] = filter_q_width_0x10b
    if (filter_cutoff_key_scaling_break1_0x10c is None) != (
        filter_cutoff_key_scaling_break2_0x10d is None
    ):
        raise ValueError(
            "generated SBNK cutoff key-scaling breaks require both break 1 and break 2"
        )
    if (
        filter_cutoff_key_scaling_break1_0x10c is not None
        and filter_cutoff_key_scaling_break2_0x10d is not None
    ):
        require_unsigned_range(
            "generated SBNK cutoff key-scaling break 1",
            filter_cutoff_key_scaling_break1_0x10c,
            0x7F,
        )
        require_unsigned_range(
            "generated SBNK cutoff key-scaling break 2",
            filter_cutoff_key_scaling_break2_0x10d,
            0x7F,
        )
        if filter_cutoff_key_scaling_break1_0x10c > filter_cutoff_key_scaling_break2_0x10d:
            raise ValueError(
                "generated SBNK cutoff key-scaling break 1 "
                f"{filter_cutoff_key_scaling_break1_0x10c} exceeds break 2 "
                f"{filter_cutoff_key_scaling_break2_0x10d}"
            )
        data[0x10C] = filter_cutoff_key_scaling_break1_0x10c
        data[0x10D] = filter_cutoff_key_scaling_break2_0x10d
    if filter_cutoff_key_scaling_level1_0x10e is not None:
        if not -127 <= filter_cutoff_key_scaling_level1_0x10e <= 127:
            raise ValueError(
                "generated SBNK cutoff key-scaling level 1 out of range: "
                f"{filter_cutoff_key_scaling_level1_0x10e}"
            )
        put_s8(data, 0x10E, filter_cutoff_key_scaling_level1_0x10e)
    if filter_cutoff_key_scaling_level2_0x10f is not None:
        if not -127 <= filter_cutoff_key_scaling_level2_0x10f <= 127:
            raise ValueError(
                "generated SBNK cutoff key-scaling level 2 out of range: "
                f"{filter_cutoff_key_scaling_level2_0x10f}"
            )
        put_s8(data, 0x10F, filter_cutoff_key_scaling_level2_0x10f)
    if filter_cutoff_velocity_sensitivity_0x110 is not None:
        if not -63 <= filter_cutoff_velocity_sensitivity_0x110 <= 63:
            raise ValueError(
                "generated SBNK cutoff velocity sensitivity out of range: "
                f"{filter_cutoff_velocity_sensitivity_0x110}"
            )
        put_s8(data, 0x110, filter_cutoff_velocity_sensitivity_0x110)
    if filter_q_width_velocity_sensitivity_0x111 is not None:
        if not -63 <= filter_q_width_velocity_sensitivity_0x111 <= 63:
            raise ValueError(
                "generated SBNK Q/width velocity sensitivity out of range: "
                f"{filter_q_width_velocity_sensitivity_0x111}"
            )
        put_s8(data, 0x111, filter_q_width_velocity_sensitivity_0x111)
    if expand_detune_0x112 is not None:
        if not -7 <= expand_detune_0x112 <= 7:
            raise ValueError(f"generated SBNK expand detune out of range: {expand_detune_0x112}")
        put_s8(data, 0x112, expand_detune_0x112)
    if expand_dephase_0x113 is not None:
        if not -63 <= expand_dephase_0x113 <= 63:
            raise ValueError(f"generated SBNK expand dephase out of range: {expand_dephase_0x113}")
        put_s8(data, 0x113, expand_dephase_0x113)
    if expand_width_0x114 is not None:
        if not -63 <= expand_width_0x114 <= 63:
            raise ValueError(f"generated SBNK expand width out of range: {expand_width_0x114}")
        put_s8(data, 0x114, expand_width_0x114)
    if random_pitch_0x115 is not None:
        require_unsigned_range("generated SBNK random pitch", random_pitch_0x115, 63)
        data[0x115] = random_pitch_0x115
    if (level_scaling_break1_0x11c is None) != (level_scaling_break2_0x11d is None):
        raise ValueError("generated SBNK level-scaling breaks require both break 1 and break 2")
    if level_scaling_break1_0x11c is not None and level_scaling_break2_0x11d is not None:
        require_unsigned_range(
            "generated SBNK level-scaling break 1", level_scaling_break1_0x11c, 0x7F
        )
        require_unsigned_range(
            "generated SBNK level-scaling break 2", level_scaling_break2_0x11d, 0x7F
        )
        if level_scaling_break1_0x11c > level_scaling_break2_0x11d:
            raise ValueError(
                "generated SBNK level-scaling break 1 "
                f"{level_scaling_break1_0x11c} exceeds break 2 {level_scaling_break2_0x11d}"
            )
        data[0x11C] = level_scaling_break1_0x11c
        data[0x11D] = level_scaling_break2_0x11d
    if level_scaling_level1_0x11e is not None:
        require_unsigned_range(
            "generated SBNK level-scaling level 1", level_scaling_level1_0x11e, 0x7F
        )
        data[0x11E] = level_scaling_level1_0x11e
    if level_scaling_level2_0x11f is not None:
        require_unsigned_range(
            "generated SBNK level-scaling level 2", level_scaling_level2_0x11f, 0x7F
        )
        data[0x11F] = level_scaling_level2_0x11f
    if sample_level_0x116 is not None:
        require_unsigned_range("generated SBNK sample level", sample_level_0x116, 0x7F)
        data[sbnk_sample_parameter_offset(0x6E)] = sample_level_0x116
    if pan_0x117 is not None:
        if not -64 <= pan_0x117 <= 63:
            raise ValueError(f"generated SBNK pan out of range: {pan_0x117}")
        put_s8(data, sbnk_sample_parameter_offset(0x6F), pan_0x117)
    if velocity_low_limit_0x118 is not None:
        require_unsigned_range("generated SBNK velocity low limit", velocity_low_limit_0x118, 0x7F)
        data[sbnk_sample_parameter_offset(0x70)] = velocity_low_limit_0x118
    if velocity_offset_0x119 is not None:
        if not -127 <= velocity_offset_0x119 <= 127:
            raise ValueError(
                f"generated SBNK velocity offset out of range: {velocity_offset_0x119}"
            )
        put_s8(data, sbnk_sample_parameter_offset(0x71), velocity_offset_0x119)
    if (velocity_range_high_0x11a is None) != (velocity_range_low_0x11b is None):
        raise ValueError("generated SBNK velocity range requires both high and low values")
    if velocity_range_high_0x11a is not None and velocity_range_low_0x11b is not None:
        require_unsigned_range("generated SBNK velocity-range low", velocity_range_low_0x11b, 0x7F)
        require_unsigned_range(
            "generated SBNK velocity-range high", velocity_range_high_0x11a, 0x7F
        )
        if velocity_range_high_0x11a < velocity_range_low_0x11b:
            raise ValueError(
                "generated SBNK velocity-range high "
                f"{velocity_range_high_0x11a} is below low {velocity_range_low_0x11b}"
            )
        data[0x11A] = velocity_range_high_0x11a
        data[0x11B] = velocity_range_low_0x11b
    if velocity_sensitivity_0x120 is not None:
        if not -127 <= velocity_sensitivity_0x120 <= 127:
            raise ValueError(
                f"generated SBNK velocity sensitivity out of range: {velocity_sensitivity_0x120}"
            )
        put_s8(data, sbnk_sample_parameter_offset(0x78), velocity_sensitivity_0x120)
    if alternate_group_0x121 is not None:
        require_unsigned_range("generated SBNK alternate group", alternate_group_0x121, 16)
        data[sbnk_sample_parameter_offset(0x79)] = alternate_group_0x121
    if sample_eq_frequency_0x122 is not None:
        require_unsigned_range(
            "generated SBNK sample EQ frequency", sample_eq_frequency_0x122, 0x3A
        )
        if not 4 <= sample_eq_frequency_0x122 <= 58:
            raise ValueError(
                f"generated SBNK sample EQ frequency out of range: {sample_eq_frequency_0x122}"
            )
        data[0x122] = sample_eq_frequency_0x122
    if sample_eq_gain_0x123 is not None:
        require_unsigned_range("generated SBNK sample EQ gain", sample_eq_gain_0x123, 0x7F)
        if not 52 <= sample_eq_gain_0x123 <= 76:
            raise ValueError(f"generated SBNK sample EQ gain out of range: {sample_eq_gain_0x123}")
        data[0x123] = sample_eq_gain_0x123
    if sample_eq_width_0x124 is not None:
        require_unsigned_range("generated SBNK sample EQ width", sample_eq_width_0x124, 0x7F)
        if not 10 <= sample_eq_width_0x124 <= 120:
            raise ValueError(
                f"generated SBNK sample EQ width out of range: {sample_eq_width_0x124}"
            )
        data[0x124] = sample_eq_width_0x124
    if filter_cutoff_distance_0x125 is not None:
        if not -63 <= filter_cutoff_distance_0x125 <= 63:
            raise ValueError(
                f"generated SBNK cutoff distance out of range: {filter_cutoff_distance_0x125}"
            )
        put_s8(data, 0x125, filter_cutoff_distance_0x125)
    if feg_attack_rate_0x126 is not None:
        require_unsigned_range("generated SBNK FEG attack rate", feg_attack_rate_0x126, 0x7F)
        data[0x126] = feg_attack_rate_0x126
    if feg_decay_rate_0x127 is not None:
        require_unsigned_range("generated SBNK FEG decay rate", feg_decay_rate_0x127, 0x7F)
        data[0x127] = feg_decay_rate_0x127
    if feg_release_rate_0x128 is not None:
        require_unsigned_range("generated SBNK FEG release rate", feg_release_rate_0x128, 0x7F)
        data[0x128] = feg_release_rate_0x128
    if feg_init_level_0x129 is not None:
        if not -127 <= feg_init_level_0x129 <= 127:
            raise ValueError(f"generated SBNK FEG init level out of range: {feg_init_level_0x129}")
        put_s8(data, 0x129, feg_init_level_0x129)
    if feg_attack_level_0x12a is not None:
        if not -127 <= feg_attack_level_0x12a <= 127:
            raise ValueError(
                f"generated SBNK FEG attack level out of range: {feg_attack_level_0x12a}"
            )
        put_s8(data, 0x12A, feg_attack_level_0x12a)
    if feg_sustain_level_0x12b is not None:
        if not -127 <= feg_sustain_level_0x12b <= 127:
            raise ValueError(
                f"generated SBNK FEG sustain level out of range: {feg_sustain_level_0x12b}"
            )
        put_s8(data, 0x12B, feg_sustain_level_0x12b)
    if feg_release_level_0x12c is not None:
        if not -127 <= feg_release_level_0x12c <= 127:
            raise ValueError(
                f"generated SBNK FEG release level out of range: {feg_release_level_0x12c}"
            )
        put_s8(data, 0x12C, feg_release_level_0x12c)
    if feg_rate_key_scaling_0x12d is not None:
        if not -7 <= feg_rate_key_scaling_0x12d <= 7:
            raise ValueError(
                f"generated SBNK FEG rate key scaling out of range: {feg_rate_key_scaling_0x12d}"
            )
        put_s8(data, 0x12D, feg_rate_key_scaling_0x12d)
    if feg_rate_velocity_sensitivity_0x12e is not None:
        if not -63 <= feg_rate_velocity_sensitivity_0x12e <= 63:
            raise ValueError(
                "generated SBNK FEG rate velocity sensitivity out of range: "
                f"{feg_rate_velocity_sensitivity_0x12e}"
            )
        put_s8(data, 0x12E, feg_rate_velocity_sensitivity_0x12e)
    if feg_attack_level_velocity_sensitivity_0x12f is not None:
        if not -63 <= feg_attack_level_velocity_sensitivity_0x12f <= 63:
            raise ValueError(
                "generated SBNK FEG attack level velocity sensitivity out of range: "
                f"{feg_attack_level_velocity_sensitivity_0x12f}"
            )
        put_s8(data, 0x12F, feg_attack_level_velocity_sensitivity_0x12f)
    if feg_level_velocity_sensitivity_0x130 is not None:
        if not -63 <= feg_level_velocity_sensitivity_0x130 <= 63:
            raise ValueError(
                "generated SBNK FEG level velocity sensitivity out of range: "
                f"{feg_level_velocity_sensitivity_0x130}"
            )
        put_s8(data, 0x130, feg_level_velocity_sensitivity_0x130)
    if peg_attack_rate_0x131 is not None:
        require_unsigned_range("generated SBNK PEG attack rate", peg_attack_rate_0x131, 0x7F)
        data[0x131] = peg_attack_rate_0x131
    if peg_decay_rate_0x132 is not None:
        require_unsigned_range("generated SBNK PEG decay rate", peg_decay_rate_0x132, 0x7F)
        data[0x132] = peg_decay_rate_0x132
    if peg_release_rate_0x133 is not None:
        require_unsigned_range("generated SBNK PEG release rate", peg_release_rate_0x133, 0x7F)
        data[0x133] = peg_release_rate_0x133
    if peg_init_level_0x134 is not None:
        if not -127 <= peg_init_level_0x134 <= 127:
            raise ValueError(f"generated SBNK PEG init level out of range: {peg_init_level_0x134}")
        put_s8(data, 0x134, peg_init_level_0x134)
    if peg_attack_level_0x135 is not None:
        if not -127 <= peg_attack_level_0x135 <= 127:
            raise ValueError(
                f"generated SBNK PEG attack level out of range: {peg_attack_level_0x135}"
            )
        put_s8(data, 0x135, peg_attack_level_0x135)
    if peg_sustain_level_0x136 is not None:
        if not -127 <= peg_sustain_level_0x136 <= 127:
            raise ValueError(
                f"generated SBNK PEG sustain level out of range: {peg_sustain_level_0x136}"
            )
        put_s8(data, 0x136, peg_sustain_level_0x136)
    if peg_release_level_0x137 is not None:
        if not -127 <= peg_release_level_0x137 <= 127:
            raise ValueError(
                f"generated SBNK PEG release level out of range: {peg_release_level_0x137}"
            )
        put_s8(data, 0x137, peg_release_level_0x137)
    if peg_rate_key_scaling_0x138 is not None:
        if not -7 <= peg_rate_key_scaling_0x138 <= 7:
            raise ValueError(
                f"generated SBNK PEG rate key scaling out of range: {peg_rate_key_scaling_0x138}"
            )
        put_s8(data, 0x138, peg_rate_key_scaling_0x138)
    if peg_rate_velocity_sensitivity_0x139 is not None:
        if not -63 <= peg_rate_velocity_sensitivity_0x139 <= 63:
            raise ValueError(
                "generated SBNK PEG rate velocity sensitivity out of range: "
                f"{peg_rate_velocity_sensitivity_0x139}"
            )
        put_s8(data, 0x139, peg_rate_velocity_sensitivity_0x139)
    if peg_level_velocity_sensitivity_0x13a is not None:
        if not -63 <= peg_level_velocity_sensitivity_0x13a <= 63:
            raise ValueError(
                "generated SBNK PEG level velocity sensitivity out of range: "
                f"{peg_level_velocity_sensitivity_0x13a}"
            )
        put_s8(data, 0x13A, peg_level_velocity_sensitivity_0x13a)
    if peg_range_0x13b is not None:
        if not -63 <= peg_range_0x13b <= 63:
            raise ValueError(f"generated SBNK PEG range out of range: {peg_range_0x13b}")
        put_s8(data, 0x13B, peg_range_0x13b)
    if aeg_attack_rate_0x13c is not None:
        require_unsigned_range("generated SBNK AEG attack rate", aeg_attack_rate_0x13c, 0x7F)
        data[0x13C] = aeg_attack_rate_0x13c
    if aeg_decay_rate_0x13d is not None:
        require_unsigned_range("generated SBNK AEG decay rate", aeg_decay_rate_0x13d, 0x7F)
        data[0x13D] = aeg_decay_rate_0x13d
    if aeg_release_rate_0x13e is not None:
        require_unsigned_range("generated SBNK AEG release rate", aeg_release_rate_0x13e, 0x7F)
        data[0x13E] = aeg_release_rate_0x13e
    if aeg_sustain_level_0x141 is not None:
        require_unsigned_range("generated SBNK AEG sustain level", aeg_sustain_level_0x141, 0x7F)
        data[0x141] = aeg_sustain_level_0x141
    if aeg_attack_mode_0x143 is not None:
        require_unsigned_range("generated SBNK AEG attack mode", aeg_attack_mode_0x143, 0x02)
        data[0x143] = aeg_attack_mode_0x143
    if aeg_rate_key_scaling_0x144 is not None:
        if not -7 <= aeg_rate_key_scaling_0x144 <= 7:
            raise ValueError(
                f"generated SBNK AEG rate key scaling out of range: {aeg_rate_key_scaling_0x144}"
            )
        put_s8(data, 0x144, aeg_rate_key_scaling_0x144)
    if aeg_rate_velocity_sensitivity_0x145 is not None:
        if not -63 <= aeg_rate_velocity_sensitivity_0x145 <= 63:
            raise ValueError(
                "generated SBNK AEG rate velocity sensitivity out of range: "
                f"{aeg_rate_velocity_sensitivity_0x145}"
            )
        put_s8(data, 0x145, aeg_rate_velocity_sensitivity_0x145)
    if lfo_wave_0x146 is not None:
        require_unsigned_range("generated SBNK LFO wave", lfo_wave_0x146, 0x03)
        data[0x146] = lfo_wave_0x146
    if lfo_speed_0x147 is not None:
        require_unsigned_range("generated SBNK LFO speed", lfo_speed_0x147, 0x7F)
        data[0x147] = lfo_speed_0x147
    if lfo_delay_time_0x148 is not None:
        require_unsigned_range("generated SBNK LFO delay time", lfo_delay_time_0x148, 0x7F)
        data[0x148] = lfo_delay_time_0x148
    if lfo_flags_0x149 is not None:
        require_unsigned_range("generated SBNK LFO flags", lfo_flags_0x149, 0x07)
        data[0x149] = lfo_flags_0x149
    if lfo_cutoff_mod_depth_0x14a is not None:
        require_unsigned_range(
            "generated SBNK LFO cutoff mod depth", lfo_cutoff_mod_depth_0x14a, 0x7F
        )
        data[0x14A] = lfo_cutoff_mod_depth_0x14a
    if lfo_pitch_mod_depth_0x14b is not None:
        require_unsigned_range(
            "generated SBNK LFO pitch mod depth", lfo_pitch_mod_depth_0x14b, 0x7F
        )
        data[0x14B] = lfo_pitch_mod_depth_0x14b
    if lfo_amp_mod_depth_0x14c is not None:
        require_unsigned_range("generated SBNK LFO amp mod depth", lfo_amp_mod_depth_0x14c, 0x7F)
        data[0x14C] = lfo_amp_mod_depth_0x14c
    if start_address_velocity_sensitivity_0x108 is not None:
        if not -63 <= start_address_velocity_sensitivity_0x108 <= 63:
            raise ValueError(
                f"generated SBNK start address velocity sensitivity out of range: {start_address_velocity_sensitivity_0x108}"
            )
        put_s8(data, 0x108, start_address_velocity_sensitivity_0x108)
    if filter_gain_0x151 is not None:
        if not -31 <= filter_gain_0x151 <= 31:
            raise ValueError(f"generated SBNK filter gain out of range: {filter_gain_0x151}")
        put_s8(data, 0x151, filter_gain_0x151)
    if sample_control_records_0x164 is not None:
        write_current_sbnk_sample_control_records(data, sample_control_records_0x164)
    if velocity_xfade_high_0x17c is not None:
        require_unsigned_range(
            "generated SBNK velocity x-fade high", velocity_xfade_high_0x17c, 0x7F
        )
        data[sbnk_sample_parameter_offset(0xD4)] = velocity_xfade_high_0x17c
    if velocity_xfade_low_0x17d is not None:
        require_unsigned_range("generated SBNK velocity x-fade low", velocity_xfade_low_0x17d, 0x7F)
        data[sbnk_sample_parameter_offset(0xD5)] = velocity_xfade_low_0x17d
    if output1_0x17e is not None:
        require_unsigned_range("generated SBNK output1", output1_0x17e, 0x0C)
        data[sbnk_sample_parameter_offset(0xD6)] = output1_0x17e
    if output1_level_0x17f is not None:
        require_unsigned_range("generated SBNK output1 level", output1_level_0x17f, 0x7F)
        data[sbnk_sample_parameter_offset(0xD7)] = output1_level_0x17f
    if output2_0x180 is not None:
        require_unsigned_range("generated SBNK output2", output2_0x180, 0x0C)
        data[sbnk_sample_parameter_offset(0xD8)] = output2_0x180
    if output2_level_0x181 is not None:
        require_unsigned_range("generated SBNK output2 level", output2_level_0x181, 0x7F)
        data[sbnk_sample_parameter_offset(0xD9)] = output2_level_0x181
    if sample_portamento_type_0x182 is not None:
        require_unsigned_range(
            "generated SBNK sample portamento type", sample_portamento_type_0x182, 0x05
        )
        data[sbnk_sample_parameter_offset(0xDA)] = sample_portamento_type_0x182
    if sample_portamento_rate_0x183 is not None:
        require_unsigned_range(
            "generated SBNK sample portamento rate", sample_portamento_rate_0x183, 0x7F
        )
        data[sbnk_sample_parameter_offset(0xDB)] = sample_portamento_rate_0x183
    if sample_portamento_time_0x184 is not None:
        require_unsigned_range(
            "generated SBNK sample portamento time", sample_portamento_time_0x184, 0x7F
        )
        data[sbnk_sample_parameter_offset(0xDC)] = sample_portamento_time_0x184
    return bytes(data)


def parsed_current_sbnk_member_from_shared(
    member: object_fields.CurrentSbnkMember,
    *,
    active: bool = True,
) -> ParsedCurrentSbnkMember:
    root_key = member.root_key or 0
    sample_rate = member.sample_rate or 0
    fine_tune = member.fine_tune_cents or 0
    stored_pitch_base = member.pitch_base_word or 0
    clean_word = estimated_pitch_base_word(root_key, sample_rate, fine_tune) if active else None
    return ParsedCurrentSbnkMember(
        sample_name=member.sample_name,
        smpl_link_id=member.smpl_link_id,
        root_key=root_key,
        sample_rate=sample_rate,
        fine_tune_cents=fine_tune,
        pitch_base_word=stored_pitch_base,
        clean_pitch_base_word_for_write=clean_word,
        pitch_base_word_status=pitch_base_word_status(stored_pitch_base, clean_word, active=active),
        wave_length_frames=member.wave_length_frames or 0,
        loop_start_frame=member.loop_start_frame or 0,
        loop_length_frames=member.loop_length_frames or 0,
    )


def parse_current_sbnk_member(
    data: bytes,
    *,
    sample_name_offset: int,
    link_offset: int,
    root_key_offset: int,
    sample_rate_offset: int,
    fine_tune_offset: int,
    pitch_base_offset: int,
    wave_length_offset: int,
    loop_start_offset: int,
    loop_length_offset: int,
    active: bool = True,
) -> ParsedCurrentSbnkMember:
    lane = "left" if sample_name_offset == object_fields.SBNK_LEFT_SAMPLE_NAME_OFFSET else "right"
    return parsed_current_sbnk_member_from_shared(
        object_fields.decode_current_sbnk_member(data, lane),
        active=active,
    )


def parse_current_sbnk_contract_payload(data: bytes) -> ParsedCurrentSbnkPayload:
    if len(data) < 0x17E:
        raise ValueError(f"SBNK payload is too short: {len(data)} bytes")
    if data[0x00 : 0x00 + len(CURRENT_OBJECT_MAGIC)] != CURRENT_OBJECT_MAGIC:
        raise ValueError("payload does not start with FSFSDEV3SPLX")
    if data[0x0C:0x10] != CURRENT_SBNK_OBJECT_TYPE:
        raise ValueError("payload is not a current SBNK object")

    members = object_fields.decode_current_sbnk_members(data)
    member_window = members.member_parameters
    right_slot_present = members.right_slot_present
    left = parsed_current_sbnk_member_from_shared(members.left)
    right = (
        parsed_current_sbnk_member_from_shared(members.right) if members.right is not None else None
    )
    secondary_word = member_window.secondary_pitch_base_word_0x0e0 or 0
    secondary_clean = right.clean_pitch_base_word_for_write if right is not None else None
    sample_flags = member_window.sample_flags_0x0d0 or 0
    mapout_flags = member_window.mapout_flags_0x0d1 or 0
    return ParsedCurrentSbnkPayload(
        bank_name=members.bank_name,
        instrument_name=members.instrument_name,
        bank_topology=members.bank_topology,
        right_slot_present=right_slot_present,
        sample_parameter_base_0x0a8=CURRENT_SBNK_SAMPLE_PARAMETER_BASE,
        linked_programs_001_032_bitmap_0x0c0=be32(data, 0x0C0),
        linked_programs_033_064_bitmap_0x0c4=be32(data, 0x0C4),
        linked_programs_065_096_bitmap_0x0c8=be32(data, 0x0C8),
        linked_programs_097_128_bitmap_0x0cc=be32(data, 0x0CC),
        sample_flags_0x0d0=sample_flags,
        sample_bank_member_0x0d0_bit0=bool(sample_flags & 0x01),
        mono_sample_0x0d0_bit1=bool(sample_flags & 0x02),
        expanded_0x0d0_bit2=bool(sample_flags & 0x04),
        mapout_flags_0x0d1=mapout_flags,
        sample_eq_type_0x0d1_b7_6=(mapout_flags >> 6) & 0x03,
        sample_eq_type_ui_label=sample_eq_type_ui_label((mapout_flags >> 6) & 0x03),
        key_xfade_on_0x0d1_bit2=bool(mapout_flags & 0x04),
        fixed_pitch_on_0x0d1_bit4=bool(mapout_flags & 0x10),
        midi_receive_channel_0x0d2=member_window.midi_receive_channel_0x0d2 or 0,
        pitch_bend_type_0x0d3=member_window.pitch_bend_type_0x0d3 or 0,
        pitch_bend_type_ui_label=pitch_bend_type_ui_label(member_window.pitch_bend_type_0x0d3 or 0),
        pitch_bend_range_0x0d4=member_window.pitch_bend_range_0x0d4 or 0,
        coarse_tune_0x0d5=member_window.coarse_tune_0x0d5 or 0,
        key_range_high_0x0e2=member_window.key_range_high_0x0e2 or 0,
        key_range_low_0x0e3=member_window.key_range_low_0x0e3 or 0,
        loop_tempo_0x0e6=member_window.loop_tempo_0x0e6 or 0,
        left_wave_start_address_0x0e8=member_window.left_wave_start_address_0x0e8 or 0,
        left_wave_start_low16_0x0ea=member_window.left_wave_start_low16_0x0ea or 0,
        right_wave_start_address_0x0ec=member_window.right_wave_start_address_0x0ec or 0,
        right_wave_start_low16_0x0ee=member_window.right_wave_start_low16_0x0ee or 0,
        start_address_velocity_sensitivity_0x108=member_window.start_address_velocity_sensitivity_0x108
        or 0,
        filter_type_0x109=member_window.filter_type_0x109 or 0,
        filter_type_ui_label=filter_type_ui_label(member_window.filter_type_0x109 or 0),
        filter_cutoff_0x10a=member_window.filter_cutoff_0x10a or 0,
        filter_q_width_0x10b=member_window.filter_q_width_0x10b or 0,
        filter_cutoff_key_scaling_break1_0x10c=member_window.filter_cutoff_key_scaling_break1_0x10c
        or 0,
        filter_cutoff_key_scaling_break2_0x10d=member_window.filter_cutoff_key_scaling_break2_0x10d
        or 0,
        filter_cutoff_key_scaling_level1_0x10e=member_window.filter_cutoff_key_scaling_level1_0x10e
        or 0,
        filter_cutoff_key_scaling_level2_0x10f=member_window.filter_cutoff_key_scaling_level2_0x10f
        or 0,
        filter_cutoff_velocity_sensitivity_0x110=member_window.filter_cutoff_velocity_sensitivity_0x110
        or 0,
        filter_q_width_velocity_sensitivity_0x111=member_window.filter_q_width_velocity_sensitivity_0x111
        or 0,
        expand_detune_0x112=member_window.expand_detune_0x112 or 0,
        expand_dephase_0x113=member_window.expand_dephase_0x113 or 0,
        expand_width_0x114=member_window.expand_width_0x114 or 0,
        random_pitch_0x115=member_window.random_pitch_0x115 or 0,
        level_scaling_break1_0x11c=member_window.level_scaling_break1_0x11c or 0,
        level_scaling_break2_0x11d=member_window.level_scaling_break2_0x11d or 0,
        level_scaling_level1_0x11e=member_window.level_scaling_level1_0x11e or 0,
        level_scaling_level2_0x11f=member_window.level_scaling_level2_0x11f or 0,
        filter_scaling_bp1_0x10c=member_window.filter_cutoff_key_scaling_break1_0x10c or 0,
        filter_scaling_bp2_0x10d=member_window.filter_cutoff_key_scaling_break2_0x10d or 0,
        filter_scaling_cutoff1_0x10e=member_window.filter_cutoff_key_scaling_level1_0x10e or 0,
        filter_scaling_cutoff2_0x10f=member_window.filter_cutoff_key_scaling_level2_0x10f or 0,
        filter_velocity_to_cutoff_0x110=member_window.filter_cutoff_velocity_sensitivity_0x110 or 0,
        filter_velocity_to_q_width_0x111=member_window.filter_q_width_velocity_sensitivity_0x111
        or 0,
        sample_level_0x116=member_window.sample_level_0x116 or 0,
        pan_0x117=member_window.pan_0x117 or 0,
        velocity_low_limit_0x118=member_window.velocity_low_limit_0x118 or 0,
        velocity_offset_0x119=member_window.velocity_offset_0x119 or 0,
        velocity_range_high_0x11a=member_window.velocity_range_high_0x11a or 0,
        velocity_range_low_0x11b=member_window.velocity_range_low_0x11b or 0,
        velocity_sensitivity_0x120=member_window.velocity_sensitivity_0x120 or 0,
        alternate_group_0x121=member_window.alternate_group_0x121 or 0,
        sample_eq_frequency_0x122=member_window.sample_eq_frequency_0x122 or 0,
        sample_eq_frequency_ui_label=sample_eq_frequency_ui_label(
            member_window.sample_eq_frequency_0x122 or 0
        ),
        sample_eq_gain_0x123=member_window.sample_eq_gain_0x123 or 0,
        sample_eq_gain_db=sample_eq_gain_db(member_window.sample_eq_gain_0x123 or 0),
        sample_eq_width_0x124=member_window.sample_eq_width_0x124 or 0,
        sample_eq_width_ui_value=sample_eq_width_ui_value(member_window.sample_eq_width_0x124 or 0),
        filter_cutoff_distance_0x125=member_window.filter_cutoff_distance_0x125 or 0,
        feg_attack_rate_0x126=member_window.feg_attack_rate_0x126 or 0,
        feg_decay_rate_0x127=member_window.feg_decay_rate_0x127 or 0,
        feg_release_rate_0x128=member_window.feg_release_rate_0x128 or 0,
        feg_init_level_0x129=member_window.feg_init_level_0x129 or 0,
        feg_attack_level_0x12a=member_window.feg_attack_level_0x12a or 0,
        feg_sustain_level_0x12b=member_window.feg_sustain_level_0x12b or 0,
        feg_release_level_0x12c=member_window.feg_release_level_0x12c or 0,
        feg_rate_key_scaling_0x12d=member_window.feg_rate_key_scaling_0x12d or 0,
        feg_rate_velocity_sensitivity_0x12e=member_window.feg_rate_velocity_sensitivity_0x12e or 0,
        feg_attack_level_velocity_sensitivity_0x12f=member_window.feg_attack_level_velocity_sensitivity_0x12f
        or 0,
        feg_level_velocity_sensitivity_0x130=member_window.feg_level_velocity_sensitivity_0x130
        or 0,
        peg_attack_rate_0x131=member_window.peg_attack_rate_0x131 or 0,
        peg_decay_rate_0x132=member_window.peg_decay_rate_0x132 or 0,
        peg_release_rate_0x133=member_window.peg_release_rate_0x133 or 0,
        peg_init_level_0x134=member_window.peg_init_level_0x134 or 0,
        peg_attack_level_0x135=member_window.peg_attack_level_0x135 or 0,
        peg_sustain_level_0x136=member_window.peg_sustain_level_0x136 or 0,
        peg_release_level_0x137=member_window.peg_release_level_0x137 or 0,
        peg_rate_key_scaling_0x138=member_window.peg_rate_key_scaling_0x138 or 0,
        peg_rate_velocity_sensitivity_0x139=member_window.peg_rate_velocity_sensitivity_0x139 or 0,
        peg_level_velocity_sensitivity_0x13a=member_window.peg_level_velocity_sensitivity_0x13a
        or 0,
        peg_range_0x13b=member_window.peg_range_0x13b or 0,
        aeg_attack_rate_0x13c=member_window.aeg_attack_rate_0x13c or 0,
        aeg_decay_rate_0x13d=member_window.aeg_decay_rate_0x13d or 0,
        aeg_release_rate_0x13e=member_window.aeg_release_rate_0x13e or 0,
        aeg_sustain_level_0x141=member_window.aeg_sustain_level_0x141 or 0,
        aeg_attack_mode_0x143=member_window.aeg_attack_mode_0x143 or 0,
        aeg_rate_key_scaling_0x144=member_window.aeg_rate_key_scaling_0x144 or 0,
        aeg_rate_velocity_sensitivity_0x145=member_window.aeg_rate_velocity_sensitivity_0x145 or 0,
        lfo_wave_0x146=member_window.lfo_wave_0x146 or 0,
        lfo_speed_0x147=member_window.lfo_speed_0x147 or 0,
        lfo_speed_ui_value=lfo_speed_ui_value(member_window.lfo_speed_0x147 or 0),
        lfo_delay_time_0x148=member_window.lfo_delay_time_0x148 or 0,
        lfo_delay_ui_value=lfo_delay_ui_value(member_window.lfo_delay_time_0x148 or 0),
        lfo_flags_0x149=member_window.lfo_flags_0x149 or 0,
        lfo_key_on_sync_0x149_bit0=bool((member_window.lfo_flags_0x149 or 0) & 0x01),
        lfo_cutoff_mod_phase_invert_0x149_bit1=bool((member_window.lfo_flags_0x149 or 0) & 0x02),
        lfo_pitch_mod_phase_invert_0x149_bit2=bool((member_window.lfo_flags_0x149 or 0) & 0x04),
        lfo_cutoff_mod_depth_0x14a=member_window.lfo_cutoff_mod_depth_0x14a or 0,
        lfo_pitch_mod_depth_0x14b=member_window.lfo_pitch_mod_depth_0x14b or 0,
        lfo_amp_mod_depth_0x14c=member_window.lfo_amp_mod_depth_0x14c or 0,
        filter_gain_0x151=member_window.filter_gain_0x151 or 0,
        wave_end_address_0x15c=member_window.wave_end_address_0x15c or 0,
        expected_wave_end_address_from_start_length=u32(
            (member_window.left_wave_start_address_0x0e8 or 0) + left.wave_length_frames
        ),
        wave_end_address_delta_from_expected=u32_delta(
            member_window.wave_end_address_0x15c or 0,
            (member_window.left_wave_start_address_0x0e8 or 0) + left.wave_length_frames,
        ),
        loop_end_address_0x160=member_window.loop_end_address_0x160 or 0,
        expected_loop_end_address_from_start_length=u32(
            left.loop_start_frame + left.loop_length_frames
        ),
        loop_end_address_delta_from_expected=u32_delta(
            member_window.loop_end_address_0x160 or 0,
            left.loop_start_frame + left.loop_length_frames,
        ),
        sample_control1_device_0x164=(member_window.sample_control_records[0].device_u8 or 0),
        sample_control1_device_ui_label=sample_control_device_ui_label(
            member_window.sample_control_records[0].device_u8 or 0
        ),
        sample_control1_function_0x165=(member_window.sample_control_records[0].function_u8 or 0),
        sample_control1_function_ui_label=sample_control_function_ui_label(
            member_window.sample_control_records[0].function_u8 or 0
        ),
        sample_control1_type_0x166=(member_window.sample_control_records[0].type_u8 or 0),
        sample_control1_type_ui_label=sample_control_type_ui_label(
            member_window.sample_control_records[0].type_u8 or 0
        ),
        sample_control1_range_0x167=(member_window.sample_control_records[0].range_s8 or 0),
        sample_control2_device_0x168=(member_window.sample_control_records[1].device_u8 or 0),
        sample_control2_device_ui_label=sample_control_device_ui_label(
            member_window.sample_control_records[1].device_u8 or 0
        ),
        sample_control2_function_0x169=(member_window.sample_control_records[1].function_u8 or 0),
        sample_control2_function_ui_label=sample_control_function_ui_label(
            member_window.sample_control_records[1].function_u8 or 0
        ),
        sample_control2_type_0x16a=(member_window.sample_control_records[1].type_u8 or 0),
        sample_control2_type_ui_label=sample_control_type_ui_label(
            member_window.sample_control_records[1].type_u8 or 0
        ),
        sample_control2_range_0x16b=(member_window.sample_control_records[1].range_s8 or 0),
        sample_control3_device_0x16c=(member_window.sample_control_records[2].device_u8 or 0),
        sample_control3_device_ui_label=sample_control_device_ui_label(
            member_window.sample_control_records[2].device_u8 or 0
        ),
        sample_control3_function_0x16d=(member_window.sample_control_records[2].function_u8 or 0),
        sample_control3_function_ui_label=sample_control_function_ui_label(
            member_window.sample_control_records[2].function_u8 or 0
        ),
        sample_control3_type_0x16e=(member_window.sample_control_records[2].type_u8 or 0),
        sample_control3_type_ui_label=sample_control_type_ui_label(
            member_window.sample_control_records[2].type_u8 or 0
        ),
        sample_control3_range_0x16f=(member_window.sample_control_records[2].range_s8 or 0),
        sample_control4_device_0x170=(member_window.sample_control_records[3].device_u8 or 0),
        sample_control4_device_ui_label=sample_control_device_ui_label(
            member_window.sample_control_records[3].device_u8 or 0
        ),
        sample_control4_function_0x171=(member_window.sample_control_records[3].function_u8 or 0),
        sample_control4_function_ui_label=sample_control_function_ui_label(
            member_window.sample_control_records[3].function_u8 or 0
        ),
        sample_control4_type_0x172=(member_window.sample_control_records[3].type_u8 or 0),
        sample_control4_type_ui_label=sample_control_type_ui_label(
            member_window.sample_control_records[3].type_u8 or 0
        ),
        sample_control4_range_0x173=(member_window.sample_control_records[3].range_s8 or 0),
        sample_control5_device_0x174=(member_window.sample_control_records[4].device_u8 or 0),
        sample_control5_device_ui_label=sample_control_device_ui_label(
            member_window.sample_control_records[4].device_u8 or 0
        ),
        sample_control5_function_0x175=(member_window.sample_control_records[4].function_u8 or 0),
        sample_control5_function_ui_label=sample_control_function_ui_label(
            member_window.sample_control_records[4].function_u8 or 0
        ),
        sample_control5_type_0x176=(member_window.sample_control_records[4].type_u8 or 0),
        sample_control5_type_ui_label=sample_control_type_ui_label(
            member_window.sample_control_records[4].type_u8 or 0
        ),
        sample_control5_range_0x177=(member_window.sample_control_records[4].range_s8 or 0),
        sample_control6_device_0x178=(member_window.sample_control_records[5].device_u8 or 0),
        sample_control6_device_ui_label=sample_control_device_ui_label(
            member_window.sample_control_records[5].device_u8 or 0
        ),
        sample_control6_function_0x179=(member_window.sample_control_records[5].function_u8 or 0),
        sample_control6_function_ui_label=sample_control_function_ui_label(
            member_window.sample_control_records[5].function_u8 or 0
        ),
        sample_control6_type_0x17a=(member_window.sample_control_records[5].type_u8 or 0),
        sample_control6_type_ui_label=sample_control_type_ui_label(
            member_window.sample_control_records[5].type_u8 or 0
        ),
        sample_control6_range_0x17b=(member_window.sample_control_records[5].range_s8 or 0),
        velocity_xfade_high_0x17c=member_window.velocity_xfade_high_0x17c or 0,
        velocity_xfade_low_0x17d=member_window.velocity_xfade_low_0x17d or 0,
        output1_0x17e=member_window.output1_0x17e or 0,
        output1_ui_label=output1_destination_ui_label(member_window.output1_0x17e or 0),
        output1_level_0x17f=member_window.output1_level_0x17f or 0,
        output2_0x180=member_window.output2_0x180 or 0,
        output2_ui_label=output2_destination_ui_label(member_window.output2_0x180 or 0),
        output2_level_0x181=member_window.output2_level_0x181 or 0,
        sample_portamento_type_0x182=member_window.sample_portamento_type_0x182 or 0,
        sample_portamento_type_ui_label=sample_portamento_type_ui_label(
            member_window.sample_portamento_type_0x182 or 0
        ),
        sample_portamento_rate_0x183=member_window.sample_portamento_rate_0x183 or 0,
        sample_portamento_time_0x184=member_window.sample_portamento_time_0x184 or 0,
        left=left,
        right=right,
        secondary_pitch_base_word_0x0e0=secondary_word,
        secondary_pitch_base_word_status=pitch_base_word_status(
            secondary_word, secondary_clean, active=right_slot_present
        ),
    )
