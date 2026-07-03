"""Shared byte-level decoders for Yamaha current-format objects."""

from __future__ import annotations

import hashlib
import math
from dataclasses import dataclass

ROOT_KEY_SEMITONE_FRACTIONS = (
    0x000,
    0x055,
    0x0AB,
    0x100,
    0x155,
    0x1AB,
    0x200,
    0x255,
    0x2AB,
    0x300,
    0x355,
    0x3AB,
)
PITCH_REFERENCE_SAMPLE_RATE = 44100.0
PITCH_UNITS_PER_LN2 = 1477.3197
PITCH_BASE_STATUS_MATCHES_FORMULA = "matches-pitch-formula"
PITCH_BASE_STATUS_STORED_EXCEPTION = "stored-image-exception"
PITCH_BASE_STATUS_FORMULA_UNAVAILABLE = "pitch-formula-unavailable"
PITCH_BASE_STATUS_INACTIVE = "inactive-secondary-single-member"


def clean_ascii(data: bytes) -> str:
    return "".join(chr(byte) if 0x20 <= byte < 0x7F else "?" for byte in data.rstrip(b"\x00 "))


def root_key_pitch_word(root_key: int) -> int:
    if root_key == 0:
        return 0x03AB
    octave, semitone = divmod(root_key - 1, 12)
    return octave * 1024 + ROOT_KEY_SEMITONE_FRACTIONS[semitone]


def sample_rate_pitch_term(sample_rate: int) -> int | None:
    if sample_rate <= 0:
        return None
    ratio = sample_rate / PITCH_REFERENCE_SAMPLE_RATE
    return int(math.log(ratio) * PITCH_UNITS_PER_LN2)


def estimated_pitch_base_word(root_key: int, sample_rate: int, fine_tune: int) -> int | None:
    rate_term = sample_rate_pitch_term(sample_rate)
    if rate_term is None:
        return None
    return root_key_pitch_word(root_key) - fine_tune - rate_term


def pitch_base_word_status(stored_word: int, clean_word: int | None, *, active: bool = True) -> str:
    if not active:
        return PITCH_BASE_STATUS_INACTIVE
    if clean_word is None:
        return PITCH_BASE_STATUS_FORMULA_UNAVAILABLE
    if stored_word == clean_word:
        return PITCH_BASE_STATUS_MATCHES_FORMULA
    return PITCH_BASE_STATUS_STORED_EXCEPTION


SBAC_SLOT_COUNT_OFFSET = 0x144
SBAC_SLOT_START = 0x14C
SBAC_SLOT_SIZE = 0x14
SBAC_SAMPLE_PARAMETER_BLOCK_START = 0x040
SBAC_SAMPLE_PARAMETER_BLOCK_END = 0x120
SBAC_VALUE_ENABLE_BITMAP_START = 0x120
SBAC_VALUE_ENABLE_BITMAP_WORD_COUNT = 3
SBAC_VALUE_ENABLE_BITMAP_MAX_P2 = 88
SBAC_BULK_ASSIGNED_SAMPLE_COUNT_OFFSET = 0x130

PROG_ASSIGNMENT_START = 0x120
PROG_ASSIGNMENT_SIZE = 0x38
PROG_ASSIGNMENT_HANDLE_OFFSET = 0x10
PROG_ASSIGNMENT_KIND_OFFSET = 0x14
PROG_ASSIGNMENT_FLAG_OFFSET = 0x15
PROG_ASSIGNMENT_FIELD_SPANS: tuple[tuple[int, int, str], ...] = (
    (0x00, 0x10, "assignment_name"),
    (0x10, 0x14, "assignment_raw_handle_or_selector"),
    (0x14, 0x15, "assigned_object_type"),
    (0x15, 0x16, "midi_receive_channel_assign"),
    (0x16, 0x17, "level_offset"),
    (0x17, 0x18, "velocity_sensitivity"),
    (0x18, 0x19, "pan_offset"),
    (0x19, 0x1A, "velocity_xfade_high_offset"),
    (0x1A, 0x1B, "fine_tune_offset"),
    (0x1B, 0x1C, "velocity_xfade_low_offset"),
    (0x1C, 0x1D, "coarse_tune_offset"),
    (0x1D, 0x1E, "output1"),
    (0x1E, 0x1F, "key_limit_high"),
    (0x1F, 0x20, "key_limit_low"),
    (0x20, 0x21, "key_range_shift"),
    (0x21, 0x22, "velocity_limit_high"),
    (0x22, 0x23, "velocity_limit_low"),
    (0x23, 0x24, "portamento_mono_key_xfade_flags"),
    (0x24, 0x25, "alternate_group_number"),
    (0x25, 0x26, "aeg_attack_rate_offset"),
    (0x26, 0x27, "aeg_decay_rate_offset"),
    (0x27, 0x28, "aeg_release_rate_offset"),
    (0x28, 0x29, "output2"),
    (0x29, 0x2A, "filter_cutoff_offset"),
    (0x2A, 0x2B, "filter_gain_offset"),
    (0x2B, 0x2C, "filter_q_width_offset"),
    (0x2C, 0x2D, "cutoff_distance_offset"),
    (0x2D, 0x2F, "reserved_0045_0046"),
    (0x2F, 0x30, "output1_level_offset"),
    (0x30, 0x32, "reserved_0048_0049"),
    (0x32, 0x33, "output2_level_offset"),
    (0x33, 0x34, "midi_control_on"),
    (0x34, 0x35, "reserved_0052"),
    (0x35, 0x38, "reserved_0053_0055"),
)

SBNK_LEFT_SAMPLE_NAME_OFFSET = 0x078
SBNK_RIGHT_SAMPLE_NAME_OFFSET = 0x088
SBNK_LEFT_LINK_ID_OFFSET = 0x0A0
SBNK_RIGHT_LINK_ID_OFFSET = 0x0A4
SBNK_PROGRAM_LINK_BITMAP_START = 0x0C0
SBNK_PROGRAM_LINK_BITMAP_WORD_COUNT = 4
SBNK_SAMPLE_PARAMETER_BASE = 0x0A8

CURRENT_SBNK_OBJECT_TYPE = b"SBNK"
CURRENT_SBNK_CONTRACT_PAYLOAD_SIZE = 0x200


def u32(value: int) -> int:
    return value & 0xFFFFFFFF


def u32_delta(stored: int, expected: int) -> int:
    return u32(stored) - u32(expected)
SBNK_SAMPLE_CONTROL_RECORD_COUNT = 6
SBNK_SAMPLE_CONTROL_RECORD_SIZE = 4
SBNK_SAMPLE_CONTROL_RECORD_BASE = SBNK_SAMPLE_PARAMETER_BASE + 0xBC

PROG_SLOT_KIND_TARGET_CATEGORY = {
    0x10: "SBNK",
    0x11: "SBAC",
}

PROG_EFFECT_BLOCKS = ((1, 0x098), (2, 0x0C0), (3, 0x0E8))
PROG_EFFECT_BLOCK_SIZE = 0x28
PROG_EFFECT_PARAMETER_COUNT = 16
PROG_EFFECT_PARAMETER_START_IN_BLOCK = 0x08
PROG_EFFECT_PARAMETER_SIZE = 2

PROG_COMMON_FIELD_OFFSETS = {
    "program_flags_ad_source_effect_connection_lfo_sync_0x080": 0x080,
    "program_lfo_cycle_wave_initial_phase_0x081": 0x081,
    "program_lfo_reset_midi_channel_0x08f": 0x08F,
    "program_portamento_type_0x090": 0x090,
    "program_portamento_rate_0x091": 0x091,
    "program_portamento_time_0x092": 0x092,
    "sample_and_hold_speed_0x093": 0x093,
    "program_lfo_tempo_0x094": 0x094,
    "program_lfo_reset_note_0x095": 0x095,
}
PROG_COMMON_UNKNOWN_0X068_START = 0x068
PROG_COMMON_UNKNOWN_0X068_END = 0x078
PROG_COMMON_UNKNOWN_0X082_START = 0x082
PROG_COMMON_UNKNOWN_0X082_END = 0x086
PROG_COMMON_UNKNOWN_0X086_OFFSET = 0x086
PROG_COMMON_UNKNOWN_0X087_START = 0x087
PROG_COMMON_UNKNOWN_0X087_END = 0x08B
PROG_COMMON_UNKNOWN_0X096_START = 0x096
PROG_COMMON_UNKNOWN_0X096_END = 0x098
PROG_CONTROL_START = 0x110
PROG_CONTROL_RECORD_SIZE = 4
PROG_CONTROL_RECORD_COUNT = 4
PROG_CONTROL_TAIL_COPY_START = 0x358
PROG_CONTROL_TAIL_COPY_END = 0x368

PROG_LFO_CYCLE_LABELS = {
    0: "e",
    1: "q",
    2: "e x 3",
    3: "q x 2",
    4: "q x 4",
    5: "q x 8",
    6: "q x 16",
}
PROG_LFO_WAVE_LABELS = {
    0: "off",
    1: "Sine",
    2: "Saw",
    3: "Triangle",
    4: "Square",
    5: "S/H",
    6: "StepWave",
}
PROG_LFO_INITIAL_PHASE_LABELS = {
    0: "0",
    1: "90",
    2: "180",
    3: "270",
}
PROG_LFO_RESET_CHANNEL_LABELS = {
    0xFE: "off",
    0xFF: "AUD",
    0x10: "Bch",
}
PROG_PORTAMENTO_TYPE_LABELS = {
    0: "rate(fingered)",
    1: "rate(fulltime)",
    2: "time(fingered)",
    3: "time(fulltime)",
}
PROG_CONTROL_TYPE_LABELS = {
    0: "+offset",
    1: "-/+offset",
    2: "+ofst(-exp)",
    3: "+ofst(+exp)",
}


@dataclass(frozen=True)
class SbacSlot:
    """One decoded active slot inside a current SBAC payload.
    
    Use it to connect sampler-visible grouped sample-bank entries to SBNK name candidates and raw handle fields."""
    index: int
    offset: int
    raw_name: str
    name: str
    raw_handle_0x10: int


@dataclass(frozen=True)
class CurrentSbacFields:
    """Decoded current SBAC parameter and slot summary.
    
    Use it when inspecting sample-bank group metadata, enabled sample-parameter masks, active slot counts, and child slot names."""
    sample_parameter_block_raw_0x040_0x11f: bytes
    sample_parameter_block_sha1_0x040_0x11f: str
    value_enable_words_0x120_0x12b: tuple[int, int, int]
    enabled_sample_parameter_p2: tuple[int, ...]
    enabled_sample_parameter_p2_over_table_range: tuple[int, ...]
    bulk_assigned_sample_count_0x130: int | None
    active_slot_count_0x144: int
    max_slot_count_from_payload: int
    slots: tuple[SbacSlot, ...]


@dataclass(frozen=True)
class ProgAssignment:
    """One decoded current PROG assignment/Easy Edit row.
    
    Use it to inspect the sampler-visible program assignment name, target selector, and per-assignment offsets such as level, pan, key/velocity limits, pitch, output, and filter offsets."""
    index: int
    offset: int
    raw_row: bytes
    raw_name: str
    name: str
    raw_handle_0x10: int
    kind_byte_0x14: int
    flag_byte_0x15: int
    expected_category: str
    midi_receive_channel_assign_0x15: int | None
    level_offset_0x16: int | None
    velocity_sensitivity_0x17: int | None
    pan_offset_0x18: int | None
    velocity_xfade_high_offset_0x19: int | None
    fine_tune_offset_0x1a: int | None
    velocity_xfade_low_offset_0x1b: int | None
    coarse_tune_offset_0x1c: int | None
    output1_0x1d: int | None
    key_limit_high_0x1e: int | None
    key_limit_low_0x1f: int | None
    key_range_shift_0x20: int | None
    velocity_limit_high_0x21: int | None
    velocity_limit_low_0x22: int | None
    portamento_mono_key_xfade_flags_0x23: int | None
    alternate_group_number_0x24: int | None
    aeg_attack_rate_offset_0x25: int | None
    aeg_decay_rate_offset_0x26: int | None
    aeg_release_rate_offset_0x27: int | None
    output2_0x28: int | None
    filter_cutoff_offset_0x29: int | None
    filter_gain_offset_0x2a: int | None
    filter_q_width_offset_0x2b: int | None
    cutoff_distance_offset_0x2c: int | None
    reserved_0x2d_0x2e: bytes
    output1_level_offset_0x2f: int | None
    reserved_0x30_0x31: bytes
    output2_level_offset_0x32: int | None
    midi_control_on_0x33: int | None
    reserved_0x34: int | None
    reserved_0x35_0x37: bytes


@dataclass(frozen=True)
class ProgControlRecord:
    """One current PROG controller mapping record.
    
    Use it to inspect a program-level controller device, function, response type, and signed range value."""
    index: int
    offset: int
    raw_record: bytes
    device_u8: int | None
    function_u8: int | None
    type_u8: int | None
    range_s8: int | None


@dataclass(frozen=True)
class ProgCommonFields:
    """Decoded current PROG common/controller fields outside assignment rows.
    
    Use it for program-level settings such as LFO, portamento, reset controls, controller records, and raw reserved neighborhoods that still need quality."""
    raw_values: dict[str, int | None]
    raw_0x068_0x077: bytes
    raw_0x082_0x085: bytes
    raw_0x086_u8: int | None
    raw_0x087_0x08a: bytes
    raw_0x096_0x097: bytes
    control_records: tuple[ProgControlRecord, ...]
    control_raw_0x110_0x11f: bytes
    control_tail_raw_0x358_0x367: bytes

    def value(self, name: str) -> int | None:
        return self.raw_values.get(name)


@dataclass(frozen=True)
class ProgEffectCommonBlock:
    """Decoded common header and parameter slots for one current PROG effect block.
    
    Use it to inspect effect active/bypass state, routing/common levels, type byte, width display transform, and raw parameter words."""
    effect_number: int
    block_start: int
    block_end: int
    raw_block: bytes
    active_or_bypass_u8: int | None
    input_level_u8: int | None
    output_level_u8: int | None
    pan_s8: int | None
    output_u8: int | None
    width_raw_s8: int | None
    width_display: int | None
    type_u8: int | None
    type_mirror_or_reserved_u8: int | None
    parameter_values_be16: tuple[int | None, ...]

    def parameter_value(self, parameter_number: int) -> int | None:
        if not 1 <= parameter_number <= len(self.parameter_values_be16):
            return None
        return self.parameter_values_be16[parameter_number - 1]


@dataclass(frozen=True)
class CurrentSbnkMember:
    """Decoded left, right, or inactive member lane in a current SBNK.
    
    Use it to inspect the linked sample name, SMPL link ID, pitch metadata, waveform length, and loop window for one member lane."""
    lane: str
    sample_name_offset: int
    link_offset: int
    sample_name: str
    smpl_link_id: int
    root_key: int | None
    sample_rate: int | None
    fine_tune_cents: int | None
    pitch_base_word: int | None
    wave_length_frames: int | None
    loop_start_frame: int | None
    loop_length_frames: int | None


@dataclass(frozen=True)
class CurrentSbnkMembers:
    """Decoded current SBNK topology and member summary.
    
    Use it to distinguish single-member and two-member banks, linked program bitmaps, active/inactive right lanes, and member parameter windows."""
    bank_name: str
    instrument_name: str
    bank_topology: str
    right_slot_present: bool
    right_link_role: str
    left: CurrentSbnkMember
    right: CurrentSbnkMember | None
    inactive_right: CurrentSbnkMember
    linked_program_numbers: tuple[int, ...]
    linked_program_bitmap_words: tuple[int, int, int, int]
    member_parameters: SbnkMemberParameterWindow


@dataclass(frozen=True)
class SbnkSampleControlRecord:
    """One decoded sample-controller record in a current SBNK member parameter window.
    
    Use it to inspect SmpCtrlA/B device, function, response type, and signed range values."""
    index: int
    offset: int
    device_u8: int | None
    function_u8: int | None
    type_u8: int | None
    range_s8: int | None


@dataclass(frozen=True)
class SbnkMemberParameterWindow:
    """Decoded current SBNK sample-parameter window.
    
    Use it for sampler-visible sample/member parameters such as map/output flags, pitch, key/velocity ranges, loop caches, levels, pan, filter, envelopes, LFO, EQ, output routing, portamento, and sample controllers."""
    sample_parameter_base_0x0a8: int
    sample_flags_0x0d0: int | None
    mapout_flags_0x0d1: int | None
    midi_receive_channel_0x0d2: int | None
    pitch_bend_type_0x0d3: int | None
    pitch_bend_range_0x0d4: int | None
    coarse_tune_0x0d5: int | None
    left_root_key_0x0d6: int | None
    right_root_key_0x0d7: int | None
    left_sample_rate_0x0d8: int | None
    right_sample_rate_0x0da: int | None
    left_fine_tune_cents_0x0dc: int | None
    right_fine_tune_cents_0x0dd: int | None
    pitch_base_word_0x0de: int | None
    secondary_pitch_base_word_0x0e0: int | None
    key_range_high_0x0e2: int | None
    key_range_low_0x0e3: int | None
    loop_tempo_0x0e6: int | None
    left_wave_start_address_0x0e8: int | None
    left_wave_start_low16_0x0ea: int | None
    right_wave_start_address_0x0ec: int | None
    right_wave_start_low16_0x0ee: int | None
    left_wave_length_frames_0x0f0: int | None
    right_wave_length_frames_0x0f4: int | None
    left_loop_start_frame_0x0f8: int | None
    right_loop_start_frame_0x0fc: int | None
    left_loop_length_frames_0x100: int | None
    right_loop_length_frames_0x104: int | None
    start_address_velocity_sensitivity_0x108: int | None
    filter_type_0x109: int | None
    filter_cutoff_0x10a: int | None
    filter_q_width_0x10b: int | None
    filter_cutoff_key_scaling_break1_0x10c: int | None
    filter_cutoff_key_scaling_break2_0x10d: int | None
    filter_cutoff_key_scaling_level1_0x10e: int | None
    filter_cutoff_key_scaling_level2_0x10f: int | None
    filter_cutoff_velocity_sensitivity_0x110: int | None
    filter_q_width_velocity_sensitivity_0x111: int | None
    expand_detune_0x112: int | None
    expand_dephase_0x113: int | None
    expand_width_0x114: int | None
    random_pitch_0x115: int | None
    sample_level_0x116: int | None
    pan_0x117: int | None
    velocity_low_limit_0x118: int | None
    velocity_offset_0x119: int | None
    velocity_range_high_0x11a: int | None
    velocity_range_low_0x11b: int | None
    level_scaling_break1_0x11c: int | None
    level_scaling_break2_0x11d: int | None
    level_scaling_level1_0x11e: int | None
    level_scaling_level2_0x11f: int | None
    velocity_sensitivity_0x120: int | None
    alternate_group_0x121: int | None
    sample_eq_frequency_0x122: int | None
    sample_eq_gain_0x123: int | None
    sample_eq_width_0x124: int | None
    filter_cutoff_distance_0x125: int | None
    feg_attack_rate_0x126: int | None
    feg_decay_rate_0x127: int | None
    feg_release_rate_0x128: int | None
    feg_init_level_0x129: int | None
    feg_attack_level_0x12a: int | None
    feg_sustain_level_0x12b: int | None
    feg_release_level_0x12c: int | None
    feg_rate_key_scaling_0x12d: int | None
    feg_rate_velocity_sensitivity_0x12e: int | None
    feg_attack_level_velocity_sensitivity_0x12f: int | None
    feg_level_velocity_sensitivity_0x130: int | None
    peg_attack_rate_0x131: int | None
    peg_decay_rate_0x132: int | None
    peg_release_rate_0x133: int | None
    peg_init_level_0x134: int | None
    peg_attack_level_0x135: int | None
    peg_sustain_level_0x136: int | None
    peg_release_level_0x137: int | None
    peg_rate_key_scaling_0x138: int | None
    peg_rate_velocity_sensitivity_0x139: int | None
    peg_level_velocity_sensitivity_0x13a: int | None
    peg_range_0x13b: int | None
    aeg_attack_rate_0x13c: int | None
    aeg_decay_rate_0x13d: int | None
    aeg_release_rate_0x13e: int | None
    aeg_sustain_level_0x141: int | None
    aeg_attack_mode_0x143: int | None
    aeg_rate_key_scaling_0x144: int | None
    aeg_rate_velocity_sensitivity_0x145: int | None
    lfo_wave_0x146: int | None
    lfo_speed_0x147: int | None
    lfo_delay_time_0x148: int | None
    lfo_flags_0x149: int | None
    lfo_cutoff_mod_depth_0x14a: int | None
    lfo_pitch_mod_depth_0x14b: int | None
    lfo_amp_mod_depth_0x14c: int | None
    filter_gain_0x151: int | None
    wave_end_address_0x15c: int | None
    loop_end_address_0x160: int | None
    sample_control_records: tuple[SbnkSampleControlRecord, ...]
    velocity_xfade_high_0x17c: int | None
    velocity_xfade_low_0x17d: int | None
    output1_0x17e: int | None
    output1_level_0x17f: int | None
    output2_0x180: int | None
    output2_level_0x181: int | None
    sample_portamento_type_0x182: int | None
    sample_portamento_rate_0x183: int | None
    sample_portamento_time_0x184: int | None


def parse_program_number(name: str) -> int | None:
    try:
        number = int(name.strip())
    except ValueError:
        return None
    return number if 1 <= number <= 128 else None


def raw_u8(payload: bytes, offset: int) -> int | None:
    return payload[offset] if offset < len(payload) else None


def raw_hex(value: int | None) -> str:
    return "" if value is None else f"0x{value:02x}"


def s8(value: int | None) -> int | None:
    if value is None:
        return None
    return value - 0x100 if value >= 0x80 else value


def be16(payload: bytes, offset: int) -> int | None:
    if offset + 1 >= len(payload):
        return None
    return (payload[offset] << 8) | payload[offset + 1]


def be32(payload: bytes, offset: int) -> int | None:
    if offset + 3 >= len(payload):
        return None
    return (
        (payload[offset] << 24)
        | (payload[offset + 1] << 16)
        | (payload[offset + 2] << 8)
        | payload[offset + 3]
    )


def raw_slice(payload: bytes, start: int, end: int) -> bytes:
    if start >= len(payload):
        return b""
    return payload[start : min(end, len(payload))]


def raw_slice_hex(payload: bytes, start: int, end: int) -> str:
    data = raw_slice(payload, start, end)
    return data.hex() if data else ""


def raw_slice_sha1(payload: bytes, start: int, end: int) -> str:
    data = raw_slice(payload, start, end)
    return hashlib.sha1(data).hexdigest() if data else ""


def effect_width_display(raw_signed: int | None) -> int | None:
    if raw_signed is None:
        return None
    decoded = raw_signed + 63
    return decoded if -63 <= decoded <= 63 else None


def prog_lfo_sync_raw(flags_0x080: int | None) -> int | None:
    return None if flags_0x080 is None else (flags_0x080 >> 6) & 0x03


def prog_effect_connection_raw(flags_0x080: int | None) -> int | None:
    return None if flags_0x080 is None else (flags_0x080 >> 3) & 0x07


def prog_lfo_cycle_raw(value_0x081: int | None) -> int | None:
    return None if value_0x081 is None else value_0x081 & 0x07


def prog_lfo_wave_raw(value_0x081: int | None) -> int | None:
    return None if value_0x081 is None else (value_0x081 >> 3) & 0x07


def prog_lfo_initial_phase_raw(value_0x081: int | None) -> int | None:
    return None if value_0x081 is None else (value_0x081 >> 6) & 0x03


def label_from_map(value: int | None, labels: dict[int, str]) -> str:
    if value is None:
        return ""
    return labels.get(value, "")


def prog_reset_channel_label(raw_channel: int | None) -> str:
    if raw_channel is None:
        return ""
    if 0 <= raw_channel <= 0x0F:
        return f"{raw_channel + 1:02d}"
    return PROG_LFO_RESET_CHANNEL_LABELS.get(raw_channel, "")


def prog_reset_note_display(
    raw_note: int | None, *, raw_reset_channel: int | None, lfo_sync_raw: int | None
) -> str:
    if raw_note is None:
        return ""
    if lfo_sync_raw == 1 or raw_reset_channel in {0xFE, 0xFF, None}:
        return "(-)"
    if raw_note == 0xFF:
        return "all"
    return str(raw_note)


def prog_portamento_effective_rate_time(
    raw_type: int | None,
    rate_value: int | None,
    time_value: int | None,
) -> tuple[int | None, str]:
    if raw_type in {0, 1}:
        return rate_value, "rate_0x091"
    if raw_type in {2, 3}:
        return time_value, "time_0x092"
    return None, ""


def prog_sample_and_hold_display(raw_speed: int | None) -> int | None:
    return None if raw_speed is None else raw_speed + 1


def prog_channel_bitmap_labels(raw_word: int | None, *, prefix: str = "A") -> tuple[str, ...]:
    if raw_word is None:
        return ()
    return tuple(f"{prefix}{bit + 1:02d}" for bit in range(16) if raw_word & (1 << bit))


def prog_big_endian_u16(data: bytes) -> int | None:
    if len(data) < 2:
        return None
    return int.from_bytes(data[:2], "big")


def prog_control_record_summary(record: ProgControlRecord) -> str:
    if len(record.raw_record) < PROG_CONTROL_RECORD_SIZE:
        return f"{record.index + 1}:short"
    type_label = label_from_map(record.type_u8, PROG_CONTROL_TYPE_LABELS)
    range_text = "" if record.range_s8 is None else f"{record.range_s8:+d}"
    function_label = type_label or raw_hex(record.type_u8)
    return (
        f"{record.index + 1}:dev={raw_hex(record.device_u8)};"
        f"fn={raw_hex(record.function_u8)};"
        f"type={function_label};range={range_text}"
    )


def prog_control_records_summary(records: tuple[ProgControlRecord, ...]) -> str:
    return "|".join(prog_control_record_summary(record) for record in records)


def sbnk_sample_parameter_offset(sysex_sample_parameter_offset: int) -> int:
    return SBNK_SAMPLE_PARAMETER_BASE + sysex_sample_parameter_offset


def sbnk_sample_control_record_offset(record_index: int, field_offset: int = 0) -> int:
    if not 1 <= record_index <= SBNK_SAMPLE_CONTROL_RECORD_COUNT:
        raise ValueError(f"sample control record index out of range: {record_index}")
    if not 0 <= field_offset < SBNK_SAMPLE_CONTROL_RECORD_SIZE:
        raise ValueError(f"sample control field offset out of range: {field_offset}")
    return (
        SBNK_SAMPLE_CONTROL_RECORD_BASE
        + (record_index - 1) * SBNK_SAMPLE_CONTROL_RECORD_SIZE
        + field_offset
    )


def decode_sbac_value_enable_bitmaps(
    payload: bytes,
) -> tuple[tuple[int, int, int], tuple[int, ...], tuple[int, ...]]:
    words: list[int] = []
    enabled: list[int] = []
    over_table_range: list[int] = []
    for word_index in range(SBAC_VALUE_ENABLE_BITMAP_WORD_COUNT):
        offset = SBAC_VALUE_ENABLE_BITMAP_START + word_index * 4
        word = be32(payload, offset) or 0
        words.append(word)
        base_p2 = word_index * 32
        for bit in range(32):
            if word & (1 << bit):
                p2 = base_p2 + bit
                if p2 <= SBAC_VALUE_ENABLE_BITMAP_MAX_P2:
                    enabled.append(p2)
                else:
                    over_table_range.append(p2)
    return (words[0], words[1], words[2]), tuple(enabled), tuple(over_table_range)


def iter_sbac_slots(payload: bytes) -> tuple[int, int, list[SbacSlot]]:
    if len(payload) <= SBAC_SLOT_COUNT_OFFSET:
        return 0, 0, []
    slot_count = payload[SBAC_SLOT_COUNT_OFFSET]
    max_slots = max(0, (len(payload) - SBAC_SLOT_START) // SBAC_SLOT_SIZE)
    slots: list[SbacSlot] = []
    for index in range(min(slot_count, max_slots)):
        offset = SBAC_SLOT_START + index * SBAC_SLOT_SIZE
        raw_name_bytes = payload[offset : offset + 16]
        slots.append(
            SbacSlot(
                index=index,
                offset=offset,
                raw_name=raw_name_bytes.decode("ascii", errors="replace"),
                name=clean_ascii(raw_name_bytes),
                raw_handle_0x10=be32(payload, offset + 16) or 0,
            )
        )
    return slot_count, max_slots, slots


def decode_current_sbac_fields(payload: bytes) -> CurrentSbacFields:
    slot_count, max_slots, slots = iter_sbac_slots(payload)
    words, enabled, over_table_range = decode_sbac_value_enable_bitmaps(payload)
    sample_parameter_block = raw_slice(
        payload,
        SBAC_SAMPLE_PARAMETER_BLOCK_START,
        SBAC_SAMPLE_PARAMETER_BLOCK_END,
    )
    return CurrentSbacFields(
        sample_parameter_block_raw_0x040_0x11f=sample_parameter_block,
        sample_parameter_block_sha1_0x040_0x11f=hashlib.sha1(sample_parameter_block).hexdigest()
        if sample_parameter_block
        else "",
        value_enable_words_0x120_0x12b=words,
        enabled_sample_parameter_p2=enabled,
        enabled_sample_parameter_p2_over_table_range=over_table_range,
        bulk_assigned_sample_count_0x130=raw_u8(payload, SBAC_BULK_ASSIGNED_SAMPLE_COUNT_OFFSET),
        active_slot_count_0x144=slot_count,
        max_slot_count_from_payload=max_slots,
        slots=tuple(slots),
    )


def iter_prog_control_records(
    payload: bytes, start: int = PROG_CONTROL_START
) -> tuple[ProgControlRecord, ...]:
    records: list[ProgControlRecord] = []
    for index in range(PROG_CONTROL_RECORD_COUNT):
        offset = start + index * PROG_CONTROL_RECORD_SIZE
        raw_record = raw_slice(payload, offset, offset + PROG_CONTROL_RECORD_SIZE)
        records.append(
            ProgControlRecord(
                index=index,
                offset=offset,
                raw_record=raw_record,
                device_u8=raw_u8(payload, offset),
                function_u8=raw_u8(payload, offset + 1),
                type_u8=raw_u8(payload, offset + 2),
                range_s8=s8(raw_u8(payload, offset + 3)),
            )
        )
    return tuple(records)


def decode_prog_common_fields(payload: bytes) -> ProgCommonFields:
    return ProgCommonFields(
        raw_values={
            name: raw_u8(payload, offset) for name, offset in PROG_COMMON_FIELD_OFFSETS.items()
        },
        raw_0x068_0x077=raw_slice(
            payload, PROG_COMMON_UNKNOWN_0X068_START, PROG_COMMON_UNKNOWN_0X068_END
        ),
        raw_0x082_0x085=raw_slice(
            payload, PROG_COMMON_UNKNOWN_0X082_START, PROG_COMMON_UNKNOWN_0X082_END
        ),
        raw_0x086_u8=raw_u8(payload, PROG_COMMON_UNKNOWN_0X086_OFFSET),
        raw_0x087_0x08a=raw_slice(
            payload, PROG_COMMON_UNKNOWN_0X087_START, PROG_COMMON_UNKNOWN_0X087_END
        ),
        raw_0x096_0x097=raw_slice(
            payload, PROG_COMMON_UNKNOWN_0X096_START, PROG_COMMON_UNKNOWN_0X096_END
        ),
        control_records=iter_prog_control_records(payload),
        control_raw_0x110_0x11f=raw_slice(
            payload,
            PROG_CONTROL_START,
            PROG_CONTROL_START + PROG_CONTROL_RECORD_SIZE * PROG_CONTROL_RECORD_COUNT,
        ),
        control_tail_raw_0x358_0x367=raw_slice(
            payload, PROG_CONTROL_TAIL_COPY_START, PROG_CONTROL_TAIL_COPY_END
        ),
    )


def decode_sbnk_program_link_bitmaps(payload: bytes) -> tuple[list[int], tuple[int, int, int, int]]:
    end = SBNK_PROGRAM_LINK_BITMAP_START + SBNK_PROGRAM_LINK_BITMAP_WORD_COUNT * 4
    if len(payload) < end:
        return [], (0, 0, 0, 0)
    words = tuple(
        be32(payload, SBNK_PROGRAM_LINK_BITMAP_START + index * 4) or 0
        for index in range(SBNK_PROGRAM_LINK_BITMAP_WORD_COUNT)
    )
    programs: list[int] = []
    for word_index, word in enumerate(words):
        base_program = word_index * 32 + 1
        for bit_index in range(32):
            if word & (1 << bit_index):
                programs.append(base_program + bit_index)
    return programs, (words[0], words[1], words[2], words[3])


def iter_sbnk_sample_control_records(payload: bytes) -> tuple[SbnkSampleControlRecord, ...]:
    records: list[SbnkSampleControlRecord] = []
    for index in range(1, SBNK_SAMPLE_CONTROL_RECORD_COUNT + 1):
        offset = sbnk_sample_control_record_offset(index)
        records.append(
            SbnkSampleControlRecord(
                index=index,
                offset=offset,
                device_u8=raw_u8(payload, offset),
                function_u8=raw_u8(payload, offset + 1),
                type_u8=raw_u8(payload, offset + 2),
                range_s8=s8(raw_u8(payload, offset + 3)),
            )
        )
    return tuple(records)


def decode_current_sbnk_member(
    payload: bytes, lane: str, member_parameters: SbnkMemberParameterWindow | None = None
) -> CurrentSbnkMember:
    params = member_parameters or decode_sbnk_member_parameter_window(payload)
    if lane == "left":
        return CurrentSbnkMember(
            lane="left",
            sample_name_offset=SBNK_LEFT_SAMPLE_NAME_OFFSET,
            link_offset=SBNK_LEFT_LINK_ID_OFFSET,
            sample_name=clean_ascii(
                raw_slice(payload, SBNK_LEFT_SAMPLE_NAME_OFFSET, SBNK_LEFT_SAMPLE_NAME_OFFSET + 16)
            ),
            smpl_link_id=be32(payload, SBNK_LEFT_LINK_ID_OFFSET) or 0,
            root_key=params.left_root_key_0x0d6,
            sample_rate=params.left_sample_rate_0x0d8,
            fine_tune_cents=params.left_fine_tune_cents_0x0dc,
            pitch_base_word=params.pitch_base_word_0x0de,
            wave_length_frames=params.left_wave_length_frames_0x0f0,
            loop_start_frame=params.left_loop_start_frame_0x0f8,
            loop_length_frames=params.left_loop_length_frames_0x100,
        )
    if lane == "right":
        return CurrentSbnkMember(
            lane="right",
            sample_name_offset=SBNK_RIGHT_SAMPLE_NAME_OFFSET,
            link_offset=SBNK_RIGHT_LINK_ID_OFFSET,
            sample_name=clean_ascii(
                raw_slice(
                    payload, SBNK_RIGHT_SAMPLE_NAME_OFFSET, SBNK_RIGHT_SAMPLE_NAME_OFFSET + 16
                )
            ),
            smpl_link_id=be32(payload, SBNK_RIGHT_LINK_ID_OFFSET) or 0,
            root_key=params.right_root_key_0x0d7,
            sample_rate=params.right_sample_rate_0x0da,
            fine_tune_cents=params.right_fine_tune_cents_0x0dd,
            pitch_base_word=params.secondary_pitch_base_word_0x0e0,
            wave_length_frames=params.right_wave_length_frames_0x0f4,
            loop_start_frame=params.right_loop_start_frame_0x0fc,
            loop_length_frames=params.right_loop_length_frames_0x104,
        )
    raise ValueError(f"unknown SBNK member lane: {lane}")


def decode_current_sbnk_members(payload: bytes) -> CurrentSbnkMembers:
    params = decode_sbnk_member_parameter_window(payload)
    left = decode_current_sbnk_member(payload, "left", params)
    inactive_right = decode_current_sbnk_member(payload, "right", params)
    right_slot_present = bool(inactive_right.sample_name)
    if right_slot_present:
        right_link_role = "sample-reference"
        right = inactive_right
    else:
        right = None
        if inactive_right.smpl_link_id == 0:
            right_link_role = "unused-zero"
        elif inactive_right.smpl_link_id == left.smpl_link_id:
            right_link_role = "unused-mirrors-left-link"
        else:
            right_link_role = "unused-nonzero"
    linked_program_numbers, bitmap_words = decode_sbnk_program_link_bitmaps(payload)
    return CurrentSbnkMembers(
        bank_name=clean_ascii(raw_slice(payload, 0x32, 0x42)),
        instrument_name=clean_ascii(raw_slice(payload, 0x50, 0x68)),
        bank_topology="two-member" if right_slot_present else "single-member",
        right_slot_present=right_slot_present,
        right_link_role=right_link_role,
        left=left,
        right=right,
        inactive_right=inactive_right,
        linked_program_numbers=tuple(linked_program_numbers),
        linked_program_bitmap_words=bitmap_words,
        member_parameters=params,
    )


def decode_sbnk_member_parameter_window(payload: bytes) -> SbnkMemberParameterWindow:
    return SbnkMemberParameterWindow(
        sample_parameter_base_0x0a8=SBNK_SAMPLE_PARAMETER_BASE,
        sample_flags_0x0d0=raw_u8(payload, 0x0D0),
        mapout_flags_0x0d1=raw_u8(payload, 0x0D1),
        midi_receive_channel_0x0d2=raw_u8(payload, 0x0D2),
        pitch_bend_type_0x0d3=raw_u8(payload, 0x0D3),
        pitch_bend_range_0x0d4=raw_u8(payload, 0x0D4),
        coarse_tune_0x0d5=s8(raw_u8(payload, 0x0D5)),
        left_root_key_0x0d6=raw_u8(payload, 0x0D6),
        right_root_key_0x0d7=raw_u8(payload, 0x0D7),
        left_sample_rate_0x0d8=be16(payload, 0x0D8),
        right_sample_rate_0x0da=be16(payload, 0x0DA),
        left_fine_tune_cents_0x0dc=s8(raw_u8(payload, 0x0DC)),
        right_fine_tune_cents_0x0dd=s8(raw_u8(payload, 0x0DD)),
        pitch_base_word_0x0de=be16(payload, 0x0DE),
        secondary_pitch_base_word_0x0e0=be16(payload, 0x0E0),
        key_range_high_0x0e2=raw_u8(payload, 0x0E2),
        key_range_low_0x0e3=raw_u8(payload, 0x0E3),
        loop_tempo_0x0e6=be16(payload, 0x0E6),
        left_wave_start_address_0x0e8=be32(payload, 0x0E8),
        left_wave_start_low16_0x0ea=be16(payload, 0x0EA),
        right_wave_start_address_0x0ec=be32(payload, 0x0EC),
        right_wave_start_low16_0x0ee=be16(payload, 0x0EE),
        left_wave_length_frames_0x0f0=be32(payload, 0x0F0),
        right_wave_length_frames_0x0f4=be32(payload, 0x0F4),
        left_loop_start_frame_0x0f8=be32(payload, 0x0F8),
        right_loop_start_frame_0x0fc=be32(payload, 0x0FC),
        left_loop_length_frames_0x100=be32(payload, 0x100),
        right_loop_length_frames_0x104=be32(payload, 0x104),
        start_address_velocity_sensitivity_0x108=s8(raw_u8(payload, 0x108)),
        filter_type_0x109=raw_u8(payload, 0x109),
        filter_cutoff_0x10a=raw_u8(payload, 0x10A),
        filter_q_width_0x10b=raw_u8(payload, 0x10B),
        filter_cutoff_key_scaling_break1_0x10c=raw_u8(payload, 0x10C),
        filter_cutoff_key_scaling_break2_0x10d=raw_u8(payload, 0x10D),
        filter_cutoff_key_scaling_level1_0x10e=s8(raw_u8(payload, 0x10E)),
        filter_cutoff_key_scaling_level2_0x10f=s8(raw_u8(payload, 0x10F)),
        filter_cutoff_velocity_sensitivity_0x110=s8(raw_u8(payload, 0x110)),
        filter_q_width_velocity_sensitivity_0x111=s8(raw_u8(payload, 0x111)),
        expand_detune_0x112=s8(raw_u8(payload, 0x112)),
        expand_dephase_0x113=s8(raw_u8(payload, 0x113)),
        expand_width_0x114=s8(raw_u8(payload, 0x114)),
        random_pitch_0x115=raw_u8(payload, 0x115),
        sample_level_0x116=raw_u8(payload, sbnk_sample_parameter_offset(0x6E)),
        pan_0x117=s8(raw_u8(payload, sbnk_sample_parameter_offset(0x6F))),
        velocity_low_limit_0x118=raw_u8(payload, sbnk_sample_parameter_offset(0x70)),
        velocity_offset_0x119=s8(raw_u8(payload, sbnk_sample_parameter_offset(0x71))),
        velocity_range_high_0x11a=raw_u8(payload, 0x11A),
        velocity_range_low_0x11b=raw_u8(payload, 0x11B),
        level_scaling_break1_0x11c=raw_u8(payload, 0x11C),
        level_scaling_break2_0x11d=raw_u8(payload, 0x11D),
        level_scaling_level1_0x11e=raw_u8(payload, 0x11E),
        level_scaling_level2_0x11f=raw_u8(payload, 0x11F),
        velocity_sensitivity_0x120=s8(raw_u8(payload, sbnk_sample_parameter_offset(0x78))),
        alternate_group_0x121=raw_u8(payload, 0x121),
        sample_eq_frequency_0x122=raw_u8(payload, 0x122),
        sample_eq_gain_0x123=raw_u8(payload, 0x123),
        sample_eq_width_0x124=raw_u8(payload, 0x124),
        filter_cutoff_distance_0x125=s8(raw_u8(payload, 0x125)),
        feg_attack_rate_0x126=raw_u8(payload, 0x126),
        feg_decay_rate_0x127=raw_u8(payload, 0x127),
        feg_release_rate_0x128=raw_u8(payload, 0x128),
        feg_init_level_0x129=s8(raw_u8(payload, 0x129)),
        feg_attack_level_0x12a=s8(raw_u8(payload, 0x12A)),
        feg_sustain_level_0x12b=s8(raw_u8(payload, 0x12B)),
        feg_release_level_0x12c=s8(raw_u8(payload, 0x12C)),
        feg_rate_key_scaling_0x12d=s8(raw_u8(payload, 0x12D)),
        feg_rate_velocity_sensitivity_0x12e=s8(raw_u8(payload, 0x12E)),
        feg_attack_level_velocity_sensitivity_0x12f=s8(raw_u8(payload, 0x12F)),
        feg_level_velocity_sensitivity_0x130=s8(raw_u8(payload, 0x130)),
        peg_attack_rate_0x131=raw_u8(payload, 0x131),
        peg_decay_rate_0x132=raw_u8(payload, 0x132),
        peg_release_rate_0x133=raw_u8(payload, 0x133),
        peg_init_level_0x134=s8(raw_u8(payload, 0x134)),
        peg_attack_level_0x135=s8(raw_u8(payload, 0x135)),
        peg_sustain_level_0x136=s8(raw_u8(payload, 0x136)),
        peg_release_level_0x137=s8(raw_u8(payload, 0x137)),
        peg_rate_key_scaling_0x138=s8(raw_u8(payload, 0x138)),
        peg_rate_velocity_sensitivity_0x139=s8(raw_u8(payload, 0x139)),
        peg_level_velocity_sensitivity_0x13a=s8(raw_u8(payload, 0x13A)),
        peg_range_0x13b=s8(raw_u8(payload, 0x13B)),
        aeg_attack_rate_0x13c=raw_u8(payload, 0x13C),
        aeg_decay_rate_0x13d=raw_u8(payload, 0x13D),
        aeg_release_rate_0x13e=raw_u8(payload, 0x13E),
        aeg_sustain_level_0x141=raw_u8(payload, 0x141),
        aeg_attack_mode_0x143=raw_u8(payload, 0x143),
        aeg_rate_key_scaling_0x144=s8(raw_u8(payload, 0x144)),
        aeg_rate_velocity_sensitivity_0x145=s8(raw_u8(payload, 0x145)),
        lfo_wave_0x146=raw_u8(payload, 0x146),
        lfo_speed_0x147=raw_u8(payload, 0x147),
        lfo_delay_time_0x148=raw_u8(payload, 0x148),
        lfo_flags_0x149=raw_u8(payload, 0x149),
        lfo_cutoff_mod_depth_0x14a=raw_u8(payload, 0x14A),
        lfo_pitch_mod_depth_0x14b=raw_u8(payload, 0x14B),
        lfo_amp_mod_depth_0x14c=raw_u8(payload, 0x14C),
        filter_gain_0x151=s8(raw_u8(payload, 0x151)),
        wave_end_address_0x15c=be32(payload, 0x15C),
        loop_end_address_0x160=be32(payload, 0x160),
        sample_control_records=iter_sbnk_sample_control_records(payload),
        velocity_xfade_high_0x17c=raw_u8(payload, sbnk_sample_parameter_offset(0xD4)),
        velocity_xfade_low_0x17d=raw_u8(payload, sbnk_sample_parameter_offset(0xD5)),
        output1_0x17e=raw_u8(payload, sbnk_sample_parameter_offset(0xD6)),
        output1_level_0x17f=raw_u8(payload, sbnk_sample_parameter_offset(0xD7)),
        output2_0x180=raw_u8(payload, sbnk_sample_parameter_offset(0xD8)),
        output2_level_0x181=raw_u8(payload, sbnk_sample_parameter_offset(0xD9)),
        sample_portamento_type_0x182=raw_u8(payload, sbnk_sample_parameter_offset(0xDA)),
        sample_portamento_rate_0x183=raw_u8(payload, sbnk_sample_parameter_offset(0xDB)),
        sample_portamento_time_0x184=raw_u8(payload, sbnk_sample_parameter_offset(0xDC)),
    )


def prog_effect_parameter_offset(block_start: int, parameter_number: int) -> int:
    if not 1 <= parameter_number <= PROG_EFFECT_PARAMETER_COUNT:
        raise ValueError(f"effect parameter number out of range: {parameter_number}")
    return (
        block_start
        + PROG_EFFECT_PARAMETER_START_IN_BLOCK
        + (parameter_number - 1) * PROG_EFFECT_PARAMETER_SIZE
    )


def decode_prog_effect_common_block(
    payload: bytes, effect_number: int, block_start: int
) -> ProgEffectCommonBlock:
    width_raw = s8(raw_u8(payload, block_start + 0x05))
    parameter_values = tuple(
        be16(payload, prog_effect_parameter_offset(block_start, parameter_number))
        for parameter_number in range(1, PROG_EFFECT_PARAMETER_COUNT + 1)
    )
    return ProgEffectCommonBlock(
        effect_number=effect_number,
        block_start=block_start,
        block_end=block_start + PROG_EFFECT_BLOCK_SIZE,
        raw_block=raw_slice(payload, block_start, block_start + PROG_EFFECT_BLOCK_SIZE),
        active_or_bypass_u8=raw_u8(payload, block_start + 0x00),
        input_level_u8=raw_u8(payload, block_start + 0x01),
        output_level_u8=raw_u8(payload, block_start + 0x02),
        pan_s8=s8(raw_u8(payload, block_start + 0x03)),
        output_u8=raw_u8(payload, block_start + 0x04),
        width_raw_s8=width_raw,
        width_display=effect_width_display(width_raw),
        type_u8=raw_u8(payload, block_start + 0x06),
        type_mirror_or_reserved_u8=raw_u8(payload, block_start + 0x07),
        parameter_values_be16=parameter_values,
    )


def iter_prog_effect_common_blocks(payload: bytes) -> list[ProgEffectCommonBlock]:
    return [
        decode_prog_effect_common_block(payload, effect_number, block_start)
        for effect_number, block_start in PROG_EFFECT_BLOCKS
    ]


def prog_assignment_offset(index: int) -> int:
    if index < 0:
        raise ValueError(f"assignment index out of range: {index}")
    return PROG_ASSIGNMENT_START + index * PROG_ASSIGNMENT_SIZE


def prog_assignment_row(payload: bytes, index: int) -> bytes:
    start = prog_assignment_offset(index)
    end = start + PROG_ASSIGNMENT_SIZE
    if len(payload) < end:
        raise ValueError(f"PROG payload too short for assignment row {index}: {len(payload)} bytes")
    return payload[start:end]


def prog_assignment_field_labels_for_offset(offset: int) -> list[str]:
    return [label for start, end, label in PROG_ASSIGNMENT_FIELD_SPANS if start <= offset < end]


def prog_assignment_field_labels_for_span(start: int, end_exclusive: int) -> str:
    labels = sorted(
        {
            label
            for offset in range(start, end_exclusive)
            for label in prog_assignment_field_labels_for_offset(offset)
        }
    )
    return ";".join(labels)


def _row_u8(row: bytes, offset: int) -> int | None:
    return row[offset] if offset < len(row) else None


def _row_s8(row: bytes, offset: int) -> int | None:
    return s8(_row_u8(row, offset))


def iter_prog_assignments(payload: bytes) -> list[ProgAssignment]:
    max_slots = max(0, (len(payload) - PROG_ASSIGNMENT_START) // PROG_ASSIGNMENT_SIZE)
    rows: list[ProgAssignment] = []
    for index in range(max_slots):
        offset = prog_assignment_offset(index)
        row = payload[offset : offset + PROG_ASSIGNMENT_SIZE]
        raw_name = row[:16].decode("ascii", errors="replace")
        name = raw_name.rstrip(" \x00")
        raw_handle = int.from_bytes(
            row[PROG_ASSIGNMENT_HANDLE_OFFSET : PROG_ASSIGNMENT_HANDLE_OFFSET + 4], "big"
        )
        kind_byte = row[PROG_ASSIGNMENT_KIND_OFFSET]
        flag_byte = row[PROG_ASSIGNMENT_FLAG_OFFSET]
        rows.append(
            ProgAssignment(
                index=index,
                offset=offset,
                raw_row=bytes(row),
                raw_name=raw_name,
                name=name,
                raw_handle_0x10=raw_handle,
                kind_byte_0x14=kind_byte,
                flag_byte_0x15=flag_byte,
                expected_category=PROG_SLOT_KIND_TARGET_CATEGORY.get(kind_byte, ""),
                midi_receive_channel_assign_0x15=_row_u8(row, 0x15),
                level_offset_0x16=_row_s8(row, 0x16),
                velocity_sensitivity_0x17=_row_s8(row, 0x17),
                pan_offset_0x18=_row_s8(row, 0x18),
                velocity_xfade_high_offset_0x19=_row_s8(row, 0x19),
                fine_tune_offset_0x1a=_row_s8(row, 0x1A),
                velocity_xfade_low_offset_0x1b=_row_s8(row, 0x1B),
                coarse_tune_offset_0x1c=_row_s8(row, 0x1C),
                output1_0x1d=_row_u8(row, 0x1D),
                key_limit_high_0x1e=_row_u8(row, 0x1E),
                key_limit_low_0x1f=_row_u8(row, 0x1F),
                key_range_shift_0x20=_row_s8(row, 0x20),
                velocity_limit_high_0x21=_row_u8(row, 0x21),
                velocity_limit_low_0x22=_row_u8(row, 0x22),
                portamento_mono_key_xfade_flags_0x23=_row_u8(row, 0x23),
                alternate_group_number_0x24=_row_u8(row, 0x24),
                aeg_attack_rate_offset_0x25=_row_s8(row, 0x25),
                aeg_decay_rate_offset_0x26=_row_s8(row, 0x26),
                aeg_release_rate_offset_0x27=_row_s8(row, 0x27),
                output2_0x28=_row_u8(row, 0x28),
                filter_cutoff_offset_0x29=_row_s8(row, 0x29),
                filter_gain_offset_0x2a=_row_s8(row, 0x2A),
                filter_q_width_offset_0x2b=_row_s8(row, 0x2B),
                cutoff_distance_offset_0x2c=_row_s8(row, 0x2C),
                reserved_0x2d_0x2e=bytes(row[0x2D:0x2F]),
                output1_level_offset_0x2f=_row_s8(row, 0x2F),
                reserved_0x30_0x31=bytes(row[0x30:0x32]),
                output2_level_offset_0x32=_row_s8(row, 0x32),
                midi_control_on_0x33=_row_u8(row, 0x33),
                reserved_0x34=_row_u8(row, 0x34),
                reserved_0x35_0x37=bytes(row[0x35:0x38]),
            )
        )
    return rows
