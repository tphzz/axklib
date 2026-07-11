#!/usr/bin/env python3
"""Report SBNK objects and their linked SMPL samples."""

from __future__ import annotations

import argparse
import csv
import json
from collections import defaultdict
from dataclasses import asdict, dataclass, field
from pathlib import Path

from axklib.containers import sfs_dump as dumper
from axklib.containers import sfs_scan as scan_sfs_objects
from axklib.parameters import current as object_fields
from axklib.parameters.sbnk_contract import (
    CURRENT_SBNK_SAMPLE_PARAMETER_BASE,
    PITCH_BASE_STATUS_FORMULA_UNAVAILABLE,
    PITCH_BASE_STATUS_STORED_EXCEPTION,
    be32,
    clean_ascii,
    estimated_pitch_base_word,
    filter_type_ui_label,
    lfo_delay_ui_value,
    lfo_speed_ui_value,
    output1_destination_ui_label,
    output2_destination_ui_label,
    pitch_base_word_status,
    pitch_bend_type_ui_label,
    s8,
    sample_control_device_ui_label,
    sample_control_function_ui_label,
    sample_control_type_ui_label,
    sample_eq_frequency_ui_label,
    sample_eq_gain_db,
    sample_eq_type_ui_label,
    sample_eq_width_ui_value,
    sample_portamento_type_ui_label,
    smpl_unaligned_u32_at_0x092,
    u32,
    u32_delta,
)


@dataclass
class SmplRef:
    name: str
    object_offset: int
    sample_rate: int
    payload_bytes: int
    frames: int
    root_key_midi_note_guess: int
    fine_tune_cents_guess: int
    link_id_0x078: int
    group_id_0x06c: int
    wave_length_frames_0x092: int
    loop_start_frame_0x096: int
    loop_length_frames_0x09a: int
    wav_path: str


@dataclass
class MatchedSmpl:
    ref: SmplRef | None
    method: str
    candidate_count: int
    candidate_refs: list[SmplRef] = field(default_factory=list)


@dataclass
class SbnkLink:
    source_image: str
    sbnk_offset: int
    sbnk_cluster: int
    bank_name: str
    instrument_name: str
    sample_name_left: str
    sample_name_right: str
    left_root_key_0x0d6: int
    right_root_key_0x0d7: int
    left_sample_rate_0x0d8: int
    right_sample_rate_0x0da: int
    left_fine_tune_cents_0x0dc: int
    right_fine_tune_cents_0x0dd: int
    pitch_base_word_0x0de: int
    secondary_pitch_base_word_0x0e0: int
    key_range_high_0x0e2: int
    key_range_low_0x0e3: int
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
    loop_tempo_0x0e6: int
    left_wave_start_address_0x0e8: int
    left_wave_start_low16_0x0ea: int
    right_wave_start_address_0x0ec: int
    right_wave_start_low16_0x0ee: int
    sample_level_0x116: int
    pan_0x117: int
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
    velocity_range_low_candidate_0x10c: int
    velocity_range_high_candidate_0x10d: int
    estimated_pitch_base_word_0x0de: int | None
    pitch_base_word_delta_from_estimate: int | None
    pitch_base_word_matches_pitch_formula: bool
    pitch_base_word_status: str
    clean_pitch_base_word_for_write_0x0de: int | None
    estimated_secondary_pitch_base_word_0x0e0: int | None
    secondary_pitch_base_word_delta_from_estimate: int | None
    secondary_pitch_base_word_matches_pitch_formula: bool | None
    secondary_pitch_base_word_status: str
    clean_secondary_pitch_base_word_for_write_0x0e0: int | None
    bank_topology: str
    right_slot_present: bool
    right_link_role: str
    known_member_count: int
    left_link_id_0x0a0: int
    right_link_id_0x0a4: int
    left_wave_length_frames_0x0f0: int
    right_wave_length_frames_0x0f4: int
    left_loop_start_frame_0x0f8: int
    right_loop_start_frame_0x0fc: int
    left_loop_length_frames_0x100: int
    right_loop_length_frames_0x104: int
    left_smpl_offset: int | None
    right_smpl_offset: int | None
    left_smpl_name: str
    right_smpl_name: str
    left_payload_bytes: int | None
    right_payload_bytes: int | None
    left_frames: int | None
    right_frames: int | None
    left_rate: int | None
    right_rate: int | None
    left_smpl_root_key: int | None
    right_smpl_root_key: int | None
    left_smpl_fine_tune_cents: int | None
    right_smpl_fine_tune_cents: int | None
    left_smpl_wave_length_frames_0x092: int | None
    right_smpl_wave_length_frames_0x092: int | None
    left_smpl_loop_start_frame_0x096: int | None
    right_smpl_loop_start_frame_0x096: int | None
    left_smpl_loop_length_frames_0x09a: int | None
    right_smpl_loop_length_frames_0x09a: int | None
    left_frames_minus_smpl_0x092: int | None
    right_frames_minus_smpl_0x092: int | None
    left_root_key_matches_smpl: bool
    right_root_key_matches_smpl: bool
    left_sample_rate_matches_smpl: bool
    right_sample_rate_matches_smpl: bool
    left_fine_tune_matches_smpl: bool
    right_fine_tune_matches_smpl: bool
    left_window_fields_match_smpl: bool
    right_window_fields_match_smpl: bool
    left_match_method: str
    right_match_method: str
    left_match_quality: str
    right_match_quality: str
    left_link_candidate_count: int
    right_link_candidate_count: int
    left_match_candidate_count: int
    right_match_candidate_count: int
    left_candidate_offsets: str
    right_candidate_offsets: str
    left_candidate_names: str
    right_candidate_names: str
    left_candidate_link_ids_0x078: str
    right_candidate_link_ids_0x078: str
    link_ids_match_smpl: bool
    names_match_smpl: bool
    sbnk_fields_match_smpl: bool
    exact_equal_length_pair: bool
    same_rate_pair: bool
    notes: str


def joined_offsets(refs: list[SmplRef]) -> str:
    return ";".join(f"0x{ref.object_offset:x}" for ref in refs)


def joined_names(refs: list[SmplRef]) -> str:
    return ";".join(ref.name for ref in refs)


def joined_link_ids(refs: list[SmplRef]) -> str:
    return ";".join(f"0x{ref.link_id_0x078:08x}" for ref in refs)


def match_quality_for(method: str) -> str:
    if method == "link+name":
        return "Known"
    if method == "unused-empty-name":
        return "NotApplicable"
    if method in {"link-only", "name", "name-only"}:
        return "Tentative"
    if "ambiguous" in method:
        return "Tentative"
    return "Unknown"


def load_smpl_refs(mono_dir: Path) -> tuple[dict[int, list[SmplRef]], dict[str, list[SmplRef]]]:
    refs_by_link: dict[int, list[SmplRef]] = defaultdict(list)
    refs_by_name: dict[str, list[SmplRef]] = defaultdict(list)
    for path in sorted(mono_dir.glob("*.json")):
        item = json.loads(path.read_text(encoding="utf-8"))
        source_image = str(item.get("source_container") or item["source_image"])
        object_offset = int(item["object_offset"])
        with dumper.ImageReader(Path(source_image)) as reader:
            header = reader.read_at(object_offset, 0x200)
        payload_bytes = int(item.get("decoded_pcm_size", item["stored_payload_size"]))
        sample_width = int(item["sample_width_bytes"])
        link_id = be32(header, 0x078)
        ref = SmplRef(
            name=str(item.get("name_guess", "")),
            object_offset=object_offset,
            sample_rate=int(item.get("sample_rate", 0)),
            payload_bytes=payload_bytes,
            frames=payload_bytes // sample_width if sample_width else 0,
            root_key_midi_note_guess=int(item.get("root_key_midi_note_guess", header[0x7E])),
            fine_tune_cents_guess=int(item.get("fine_tune_cents_guess", s8(header[0x7F]))),
            link_id_0x078=link_id,
            group_id_0x06c=be32(header, 0x06C),
            wave_length_frames_0x092=int(
                item.get("wave_length_frames_0x092", smpl_unaligned_u32_at_0x092(header))
            ),
            loop_start_frame_0x096=int(item.get("loop_start_frame_0x096", be32(header, 0x096))),
            loop_length_frames_0x09a=int(item.get("loop_length_frames_0x09a", be32(header, 0x09A))),
            wav_path=str(item.get("wav_path", "")),
        )
        refs_by_link[link_id].append(ref)
        refs_by_name[ref.name].append(ref)
    return dict(refs_by_link), dict(refs_by_name)


def choose_smpl_ref(
    name: str,
    link_id: int,
    smpl_by_link: dict[int, list[SmplRef]],
    smpl_by_name: dict[str, list[SmplRef]],
) -> MatchedSmpl:
    link_candidates = smpl_by_link.get(link_id, [])
    exact_link_name = [ref for ref in link_candidates if ref.name == name]
    if len(exact_link_name) == 1:
        return MatchedSmpl(exact_link_name[0], "link+name", len(link_candidates), exact_link_name)
    if len(exact_link_name) > 1:
        return MatchedSmpl(None, "link+name-ambiguous", len(link_candidates), exact_link_name)

    name_candidates = smpl_by_name.get(name, [])
    if len(name_candidates) == 1:
        method = "name-only" if link_candidates else "name"
        return MatchedSmpl(name_candidates[0], method, len(link_candidates), name_candidates)
    if len(link_candidates) == 1:
        return MatchedSmpl(link_candidates[0], "link-only", len(link_candidates), link_candidates)
    if len(name_candidates) > 1:
        return MatchedSmpl(None, "name-ambiguous", len(link_candidates), name_candidates)
    if link_candidates:
        return MatchedSmpl(None, "link-ambiguous", len(link_candidates), link_candidates)
    return MatchedSmpl(None, "unmatched", 0, [])


def parse_sbnk(
    source_image: Path,
    row: dict[str, object],
    smpl_by_link: dict[int, list[SmplRef]],
    smpl_by_name: dict[str, list[SmplRef]],
) -> SbnkLink:
    offset = int(str(row["offset"]))
    with dumper.ImageReader(source_image) as reader:
        header = reader.read_at(offset, 0x200)

    members = object_fields.decode_current_sbnk_members(header)
    member_window = members.member_parameters
    left_link = members.left.smpl_link_id
    right_link = members.inactive_right.smpl_link_id
    bank_name = members.bank_name
    sample_name_left = members.left.sample_name
    sample_name_right = members.inactive_right.sample_name
    left_match = choose_smpl_ref(sample_name_left, left_link, smpl_by_link, smpl_by_name)
    right_slot_present = bool(sample_name_right)
    if right_slot_present:
        right_match = choose_smpl_ref(sample_name_right, right_link, smpl_by_link, smpl_by_name)
    else:
        right_candidates = smpl_by_link.get(right_link, []) if right_link else []
        right_match = MatchedSmpl(
            None, "unused-empty-name", len(right_candidates), right_candidates
        )
    left = left_match.ref
    right = right_match.ref
    left_root_key = member_window.left_root_key_0x0d6 or 0
    right_root_key = member_window.right_root_key_0x0d7 or 0
    left_sample_rate = member_window.left_sample_rate_0x0d8 or 0
    right_sample_rate = member_window.right_sample_rate_0x0da or 0
    left_fine_tune = member_window.left_fine_tune_cents_0x0dc or 0
    right_fine_tune = member_window.right_fine_tune_cents_0x0dd or 0
    pitch_base_word = member_window.pitch_base_word_0x0de or 0
    secondary_pitch_base_word = member_window.secondary_pitch_base_word_0x0e0 or 0
    key_range_high = member_window.key_range_high_0x0e2 or 0
    key_range_low = member_window.key_range_low_0x0e3 or 0
    left_0x0f0 = member_window.left_wave_length_frames_0x0f0 or 0
    right_0x0f4 = member_window.right_wave_length_frames_0x0f4 or 0
    left_0x0f8 = member_window.left_loop_start_frame_0x0f8 or 0
    right_0x0fc = member_window.right_loop_start_frame_0x0fc or 0
    left_0x100 = member_window.left_loop_length_frames_0x100 or 0
    right_0x104 = member_window.right_loop_length_frames_0x104 or 0
    sample_flags = member_window.sample_flags_0x0d0 or 0
    mapout_flags = member_window.mapout_flags_0x0d1 or 0
    start_address_velocity_sensitivity = member_window.start_address_velocity_sensitivity_0x108 or 0
    filter_type = member_window.filter_type_0x109 or 0
    filter_cutoff = member_window.filter_cutoff_0x10a or 0
    filter_q_width = member_window.filter_q_width_0x10b or 0
    filter_cutoff_key_scaling_break1 = member_window.filter_cutoff_key_scaling_break1_0x10c or 0
    filter_cutoff_key_scaling_break2 = member_window.filter_cutoff_key_scaling_break2_0x10d or 0
    filter_cutoff_key_scaling_level1 = member_window.filter_cutoff_key_scaling_level1_0x10e or 0
    filter_cutoff_key_scaling_level2 = member_window.filter_cutoff_key_scaling_level2_0x10f or 0
    filter_cutoff_velocity_sensitivity = member_window.filter_cutoff_velocity_sensitivity_0x110 or 0
    filter_q_width_velocity_sensitivity = (
        member_window.filter_q_width_velocity_sensitivity_0x111 or 0
    )
    expand_detune = member_window.expand_detune_0x112 or 0
    expand_dephase = member_window.expand_dephase_0x113 or 0
    expand_width = member_window.expand_width_0x114 or 0
    random_pitch = member_window.random_pitch_0x115 or 0
    level_scaling_break1 = member_window.level_scaling_break1_0x11c or 0
    level_scaling_break2 = member_window.level_scaling_break2_0x11d or 0
    level_scaling_level1 = member_window.level_scaling_level1_0x11e or 0
    level_scaling_level2 = member_window.level_scaling_level2_0x11f or 0
    sample_level = member_window.sample_level_0x116 or 0
    pan = member_window.pan_0x117 or 0
    velocity_low_limit = member_window.velocity_low_limit_0x118 or 0
    velocity_offset = member_window.velocity_offset_0x119 or 0
    velocity_range_high = member_window.velocity_range_high_0x11a or 0
    velocity_range_low = member_window.velocity_range_low_0x11b or 0
    velocity_sensitivity = member_window.velocity_sensitivity_0x120 or 0
    sample_eq_frequency = member_window.sample_eq_frequency_0x122 or 0
    sample_eq_gain = member_window.sample_eq_gain_0x123 or 0
    sample_eq_width = member_window.sample_eq_width_0x124 or 0
    filter_cutoff_distance = member_window.filter_cutoff_distance_0x125 or 0
    feg_attack_rate = member_window.feg_attack_rate_0x126 or 0
    feg_decay_rate = member_window.feg_decay_rate_0x127 or 0
    feg_release_rate = member_window.feg_release_rate_0x128 or 0
    feg_init_level = member_window.feg_init_level_0x129 or 0
    feg_attack_level = member_window.feg_attack_level_0x12a or 0
    feg_sustain_level = member_window.feg_sustain_level_0x12b or 0
    feg_release_level = member_window.feg_release_level_0x12c or 0
    feg_rate_key_scaling = member_window.feg_rate_key_scaling_0x12d or 0
    feg_rate_velocity_sensitivity = member_window.feg_rate_velocity_sensitivity_0x12e or 0
    feg_attack_level_velocity_sensitivity = (
        member_window.feg_attack_level_velocity_sensitivity_0x12f or 0
    )
    feg_level_velocity_sensitivity = member_window.feg_level_velocity_sensitivity_0x130 or 0
    peg_attack_rate = member_window.peg_attack_rate_0x131 or 0
    peg_decay_rate = member_window.peg_decay_rate_0x132 or 0
    peg_release_rate = member_window.peg_release_rate_0x133 or 0
    peg_init_level = member_window.peg_init_level_0x134 or 0
    peg_attack_level = member_window.peg_attack_level_0x135 or 0
    peg_sustain_level = member_window.peg_sustain_level_0x136 or 0
    peg_release_level = member_window.peg_release_level_0x137 or 0
    peg_rate_key_scaling = member_window.peg_rate_key_scaling_0x138 or 0
    peg_rate_velocity_sensitivity = member_window.peg_rate_velocity_sensitivity_0x139 or 0
    peg_level_velocity_sensitivity = member_window.peg_level_velocity_sensitivity_0x13a or 0
    peg_range = member_window.peg_range_0x13b or 0
    aeg_attack_rate = member_window.aeg_attack_rate_0x13c or 0
    aeg_decay_rate = member_window.aeg_decay_rate_0x13d or 0
    aeg_release_rate = member_window.aeg_release_rate_0x13e or 0
    aeg_sustain_level = member_window.aeg_sustain_level_0x141 or 0
    aeg_attack_mode = member_window.aeg_attack_mode_0x143 or 0
    aeg_rate_key_scaling = member_window.aeg_rate_key_scaling_0x144 or 0
    aeg_rate_velocity_sensitivity = member_window.aeg_rate_velocity_sensitivity_0x145 or 0
    lfo_wave = member_window.lfo_wave_0x146 or 0
    lfo_speed = member_window.lfo_speed_0x147 or 0
    lfo_delay_time = member_window.lfo_delay_time_0x148 or 0
    lfo_flags = member_window.lfo_flags_0x149 or 0
    lfo_cutoff_mod_depth = member_window.lfo_cutoff_mod_depth_0x14a or 0
    lfo_pitch_mod_depth = member_window.lfo_pitch_mod_depth_0x14b or 0
    lfo_amp_mod_depth = member_window.lfo_amp_mod_depth_0x14c or 0
    filter_gain = member_window.filter_gain_0x151 or 0
    wave_end_address = member_window.wave_end_address_0x15c or 0
    expected_wave_end_address = u32((member_window.left_wave_start_address_0x0e8 or 0) + left_0x0f0)
    loop_end_address = member_window.loop_end_address_0x160 or 0
    expected_loop_end_address = u32(left_0x0f8 + left_0x100)
    velocity_xfade_high = member_window.velocity_xfade_high_0x17c or 0
    velocity_xfade_low = member_window.velocity_xfade_low_0x17d or 0
    output1 = member_window.output1_0x17e or 0
    output1_level = member_window.output1_level_0x17f or 0
    output2 = member_window.output2_0x180 or 0
    output2_level = member_window.output2_level_0x181 or 0
    velocity_range_low_candidate = member_window.filter_cutoff_key_scaling_break1_0x10c or 0
    velocity_range_high_candidate = member_window.filter_cutoff_key_scaling_break2_0x10d or 0
    expected_pitch_base = estimated_pitch_base_word(left_root_key, left_sample_rate, left_fine_tune)
    pitch_base_delta = (
        pitch_base_word - expected_pitch_base if expected_pitch_base is not None else None
    )
    pitch_base_matches_formula = pitch_base_delta == 0
    pitch_base_status = pitch_base_word_status(pitch_base_word, expected_pitch_base)
    expected_secondary_pitch_base = estimated_pitch_base_word(
        right_root_key, right_sample_rate, right_fine_tune
    )

    matched_pair = left is not None and right is not None
    link_ids_match = (
        matched_pair
        and left is not None
        and right is not None
        and left.link_id_0x078 == left_link
        and right.link_id_0x078 == right_link
    )
    names_match = (
        matched_pair
        and left is not None
        and right is not None
        and sample_name_left == left.name
        and sample_name_right == right.name
    )
    left_root_matches = bool(left and left_root_key == left.root_key_midi_note_guess)
    right_root_matches = bool(right and right_root_key == right.root_key_midi_note_guess)
    left_rate_matches = bool(left and left_sample_rate == left.sample_rate)
    right_rate_matches = bool(right and right_sample_rate == right.sample_rate)
    left_fine_matches = bool(left and left_fine_tune == left.fine_tune_cents_guess)
    right_fine_matches = bool(right and right_fine_tune == right.fine_tune_cents_guess)
    left_window_matches = bool(
        left
        and left_0x0f0 == left.wave_length_frames_0x092
        and left_0x0f8 == left.loop_start_frame_0x096
        and left_0x100 == left.loop_length_frames_0x09a
    )
    right_window_matches = bool(
        right
        and right_0x0f4 == right.wave_length_frames_0x092
        and right_0x0fc == right.loop_start_frame_0x096
        and right_0x104 == right.loop_length_frames_0x09a
    )
    sbnk_fields_match = bool(
        left_window_matches and (right_window_matches if right_slot_present else True)
    )
    exact_equal = bool(matched_pair and left and right and left.frames == right.frames)
    same_rate = bool(matched_pair and left and right and left.sample_rate == right.sample_rate)
    bank_topology = members.bank_topology
    right_link_role = members.right_link_role
    secondary_pitch_base_delta = (
        secondary_pitch_base_word - expected_secondary_pitch_base
        if right_slot_present and expected_secondary_pitch_base is not None
        else None
    )
    secondary_pitch_base_matches_formula = (
        secondary_pitch_base_delta == 0 if secondary_pitch_base_delta is not None else None
    )
    secondary_pitch_base_status = pitch_base_word_status(
        secondary_pitch_base_word,
        expected_secondary_pitch_base if right_slot_present else None,
        active=right_slot_present,
    )
    known_member_count = int(left_match.method == "link+name") + int(
        right_slot_present and right_match.method == "link+name"
    )
    notes = []
    if right_slot_present:
        notes.append("two_sample_names")
    else:
        notes.append("single_member_empty_right_name")
        notes.append(right_link_role)
    if exact_equal:
        notes.append("equal_length")
    elif same_rate:
        notes.append("same_rate_different_lengths")
    if not same_rate and matched_pair:
        notes.append("different_rates")
    if not left_rate_matches or (right_slot_present and not right_rate_matches):
        notes.append("sample_rate_field_differs_from_smpl")
    if not left_root_matches or (right_slot_present and not right_root_matches):
        notes.append("root_key_override_or_differs_from_smpl")
    if not left_fine_matches or (right_slot_present and not right_fine_matches):
        notes.append("fine_tune_override_or_differs_from_smpl")
    if not left_window_matches or (right_slot_present and not right_window_matches):
        notes.append("window_override_or_differs_from_smpl")
    if pitch_base_status == PITCH_BASE_STATUS_STORED_EXCEPTION:
        notes.append("stored_pitch_base_word_exception")
    elif pitch_base_status == PITCH_BASE_STATUS_FORMULA_UNAVAILABLE:
        notes.append("pitch_base_pitch_formula_unavailable")
    if secondary_pitch_base_status == PITCH_BASE_STATUS_STORED_EXCEPTION:
        notes.append("stored_secondary_pitch_base_word_exception")
    elif secondary_pitch_base_status == PITCH_BASE_STATUS_FORMULA_UNAVAILABLE:
        notes.append("secondary_pitch_base_pitch_formula_unavailable")
    if left_match.method != "link+name" or right_match.method != "link+name":
        notes.append(f"match_methods={left_match.method}/{right_match.method}")

    return SbnkLink(
        source_image=str(source_image),
        sbnk_offset=offset,
        sbnk_cluster=int(str(row.get("cluster_offset", 0))),
        bank_name=bank_name,
        instrument_name=clean_ascii(header[0x50:0x68]),
        sample_name_left=sample_name_left,
        sample_name_right=sample_name_right,
        left_root_key_0x0d6=left_root_key,
        right_root_key_0x0d7=right_root_key,
        left_sample_rate_0x0d8=left_sample_rate,
        right_sample_rate_0x0da=right_sample_rate,
        left_fine_tune_cents_0x0dc=left_fine_tune,
        right_fine_tune_cents_0x0dd=right_fine_tune,
        pitch_base_word_0x0de=pitch_base_word,
        secondary_pitch_base_word_0x0e0=secondary_pitch_base_word,
        key_range_high_0x0e2=key_range_high,
        key_range_low_0x0e3=key_range_low,
        sample_parameter_base_0x0a8=CURRENT_SBNK_SAMPLE_PARAMETER_BASE,
        linked_programs_001_032_bitmap_0x0c0=be32(header, 0x0C0),
        linked_programs_033_064_bitmap_0x0c4=be32(header, 0x0C4),
        linked_programs_065_096_bitmap_0x0c8=be32(header, 0x0C8),
        linked_programs_097_128_bitmap_0x0cc=be32(header, 0x0CC),
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
        loop_tempo_0x0e6=member_window.loop_tempo_0x0e6 or 0,
        left_wave_start_address_0x0e8=member_window.left_wave_start_address_0x0e8 or 0,
        left_wave_start_low16_0x0ea=member_window.left_wave_start_low16_0x0ea or 0,
        right_wave_start_address_0x0ec=member_window.right_wave_start_address_0x0ec or 0,
        right_wave_start_low16_0x0ee=member_window.right_wave_start_low16_0x0ee or 0,
        sample_level_0x116=sample_level,
        pan_0x117=pan,
        start_address_velocity_sensitivity_0x108=start_address_velocity_sensitivity,
        filter_type_0x109=filter_type,
        filter_type_ui_label=filter_type_ui_label(filter_type),
        filter_cutoff_0x10a=filter_cutoff,
        filter_q_width_0x10b=filter_q_width,
        filter_cutoff_key_scaling_break1_0x10c=filter_cutoff_key_scaling_break1,
        filter_cutoff_key_scaling_break2_0x10d=filter_cutoff_key_scaling_break2,
        filter_cutoff_key_scaling_level1_0x10e=filter_cutoff_key_scaling_level1,
        filter_cutoff_key_scaling_level2_0x10f=filter_cutoff_key_scaling_level2,
        filter_cutoff_velocity_sensitivity_0x110=filter_cutoff_velocity_sensitivity,
        filter_q_width_velocity_sensitivity_0x111=filter_q_width_velocity_sensitivity,
        expand_detune_0x112=expand_detune,
        expand_dephase_0x113=expand_dephase,
        expand_width_0x114=expand_width,
        random_pitch_0x115=random_pitch,
        level_scaling_break1_0x11c=level_scaling_break1,
        level_scaling_break2_0x11d=level_scaling_break2,
        level_scaling_level1_0x11e=level_scaling_level1,
        level_scaling_level2_0x11f=level_scaling_level2,
        filter_scaling_bp1_0x10c=filter_cutoff_key_scaling_break1,
        filter_scaling_bp2_0x10d=filter_cutoff_key_scaling_break2,
        filter_scaling_cutoff1_0x10e=filter_cutoff_key_scaling_level1,
        filter_scaling_cutoff2_0x10f=filter_cutoff_key_scaling_level2,
        filter_velocity_to_cutoff_0x110=filter_cutoff_velocity_sensitivity,
        filter_velocity_to_q_width_0x111=filter_q_width_velocity_sensitivity,
        velocity_low_limit_0x118=velocity_low_limit,
        velocity_offset_0x119=velocity_offset,
        velocity_range_high_0x11a=velocity_range_high,
        velocity_range_low_0x11b=velocity_range_low,
        velocity_sensitivity_0x120=velocity_sensitivity,
        alternate_group_0x121=member_window.alternate_group_0x121 or 0,
        sample_eq_frequency_0x122=sample_eq_frequency,
        sample_eq_frequency_ui_label=sample_eq_frequency_ui_label(sample_eq_frequency),
        sample_eq_gain_0x123=sample_eq_gain,
        sample_eq_gain_db=sample_eq_gain_db(sample_eq_gain),
        sample_eq_width_0x124=sample_eq_width,
        sample_eq_width_ui_value=sample_eq_width_ui_value(sample_eq_width),
        filter_cutoff_distance_0x125=filter_cutoff_distance,
        feg_attack_rate_0x126=feg_attack_rate,
        feg_decay_rate_0x127=feg_decay_rate,
        feg_release_rate_0x128=feg_release_rate,
        feg_init_level_0x129=feg_init_level,
        feg_attack_level_0x12a=feg_attack_level,
        feg_sustain_level_0x12b=feg_sustain_level,
        feg_release_level_0x12c=feg_release_level,
        feg_rate_key_scaling_0x12d=feg_rate_key_scaling,
        feg_rate_velocity_sensitivity_0x12e=feg_rate_velocity_sensitivity,
        feg_attack_level_velocity_sensitivity_0x12f=feg_attack_level_velocity_sensitivity,
        feg_level_velocity_sensitivity_0x130=feg_level_velocity_sensitivity,
        peg_attack_rate_0x131=peg_attack_rate,
        peg_decay_rate_0x132=peg_decay_rate,
        peg_release_rate_0x133=peg_release_rate,
        peg_init_level_0x134=peg_init_level,
        peg_attack_level_0x135=peg_attack_level,
        peg_sustain_level_0x136=peg_sustain_level,
        peg_release_level_0x137=peg_release_level,
        peg_rate_key_scaling_0x138=peg_rate_key_scaling,
        peg_rate_velocity_sensitivity_0x139=peg_rate_velocity_sensitivity,
        peg_level_velocity_sensitivity_0x13a=peg_level_velocity_sensitivity,
        peg_range_0x13b=peg_range,
        aeg_attack_rate_0x13c=aeg_attack_rate,
        aeg_decay_rate_0x13d=aeg_decay_rate,
        aeg_release_rate_0x13e=aeg_release_rate,
        aeg_sustain_level_0x141=aeg_sustain_level,
        aeg_attack_mode_0x143=aeg_attack_mode,
        aeg_rate_key_scaling_0x144=aeg_rate_key_scaling,
        aeg_rate_velocity_sensitivity_0x145=aeg_rate_velocity_sensitivity,
        lfo_wave_0x146=lfo_wave,
        lfo_speed_0x147=lfo_speed,
        lfo_speed_ui_value=lfo_speed_ui_value(lfo_speed),
        lfo_delay_time_0x148=lfo_delay_time,
        lfo_delay_ui_value=lfo_delay_ui_value(lfo_delay_time),
        lfo_flags_0x149=lfo_flags,
        lfo_key_on_sync_0x149_bit0=bool(lfo_flags & 0x01),
        lfo_cutoff_mod_phase_invert_0x149_bit1=bool(lfo_flags & 0x02),
        lfo_pitch_mod_phase_invert_0x149_bit2=bool(lfo_flags & 0x04),
        lfo_cutoff_mod_depth_0x14a=lfo_cutoff_mod_depth,
        lfo_pitch_mod_depth_0x14b=lfo_pitch_mod_depth,
        lfo_amp_mod_depth_0x14c=lfo_amp_mod_depth,
        filter_gain_0x151=filter_gain,
        wave_end_address_0x15c=wave_end_address,
        expected_wave_end_address_from_start_length=expected_wave_end_address,
        wave_end_address_delta_from_expected=u32_delta(wave_end_address, expected_wave_end_address),
        loop_end_address_0x160=loop_end_address,
        expected_loop_end_address_from_start_length=expected_loop_end_address,
        loop_end_address_delta_from_expected=u32_delta(loop_end_address, expected_loop_end_address),
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
        velocity_xfade_high_0x17c=velocity_xfade_high,
        velocity_xfade_low_0x17d=velocity_xfade_low,
        output1_0x17e=output1,
        output1_ui_label=output1_destination_ui_label(output1),
        output1_level_0x17f=output1_level,
        output2_0x180=output2,
        output2_ui_label=output2_destination_ui_label(output2),
        output2_level_0x181=output2_level,
        sample_portamento_type_0x182=member_window.sample_portamento_type_0x182 or 0,
        sample_portamento_type_ui_label=sample_portamento_type_ui_label(
            member_window.sample_portamento_type_0x182 or 0
        ),
        sample_portamento_rate_0x183=member_window.sample_portamento_rate_0x183 or 0,
        sample_portamento_time_0x184=member_window.sample_portamento_time_0x184 or 0,
        velocity_range_low_candidate_0x10c=velocity_range_low_candidate,
        velocity_range_high_candidate_0x10d=velocity_range_high_candidate,
        estimated_pitch_base_word_0x0de=expected_pitch_base,
        pitch_base_word_delta_from_estimate=pitch_base_delta,
        pitch_base_word_matches_pitch_formula=pitch_base_matches_formula,
        pitch_base_word_status=pitch_base_status,
        clean_pitch_base_word_for_write_0x0de=expected_pitch_base,
        estimated_secondary_pitch_base_word_0x0e0=(
            expected_secondary_pitch_base if right_slot_present else None
        ),
        secondary_pitch_base_word_delta_from_estimate=secondary_pitch_base_delta,
        secondary_pitch_base_word_matches_pitch_formula=secondary_pitch_base_matches_formula,
        secondary_pitch_base_word_status=secondary_pitch_base_status,
        clean_secondary_pitch_base_word_for_write_0x0e0=(
            expected_secondary_pitch_base if right_slot_present else None
        ),
        bank_topology=bank_topology,
        right_slot_present=right_slot_present,
        right_link_role=right_link_role,
        known_member_count=known_member_count,
        left_link_id_0x0a0=left_link,
        right_link_id_0x0a4=right_link,
        left_wave_length_frames_0x0f0=left_0x0f0,
        right_wave_length_frames_0x0f4=right_0x0f4,
        left_loop_start_frame_0x0f8=left_0x0f8,
        right_loop_start_frame_0x0fc=right_0x0fc,
        left_loop_length_frames_0x100=left_0x100,
        right_loop_length_frames_0x104=right_0x104,
        left_smpl_offset=left.object_offset if left else None,
        right_smpl_offset=right.object_offset if right else None,
        left_smpl_name=left.name if left else "",
        right_smpl_name=right.name if right else "",
        left_payload_bytes=left.payload_bytes if left else None,
        right_payload_bytes=right.payload_bytes if right else None,
        left_frames=left.frames if left else None,
        right_frames=right.frames if right else None,
        left_rate=left.sample_rate if left else None,
        right_rate=right.sample_rate if right else None,
        left_smpl_root_key=left.root_key_midi_note_guess if left else None,
        right_smpl_root_key=right.root_key_midi_note_guess if right else None,
        left_smpl_fine_tune_cents=left.fine_tune_cents_guess if left else None,
        right_smpl_fine_tune_cents=right.fine_tune_cents_guess if right else None,
        left_smpl_wave_length_frames_0x092=left.wave_length_frames_0x092 if left else None,
        right_smpl_wave_length_frames_0x092=right.wave_length_frames_0x092 if right else None,
        left_smpl_loop_start_frame_0x096=left.loop_start_frame_0x096 if left else None,
        right_smpl_loop_start_frame_0x096=right.loop_start_frame_0x096 if right else None,
        left_smpl_loop_length_frames_0x09a=left.loop_length_frames_0x09a if left else None,
        right_smpl_loop_length_frames_0x09a=right.loop_length_frames_0x09a if right else None,
        left_frames_minus_smpl_0x092=left.frames - left.wave_length_frames_0x092 if left else None,
        right_frames_minus_smpl_0x092=right.frames - right.wave_length_frames_0x092
        if right
        else None,
        left_root_key_matches_smpl=left_root_matches,
        right_root_key_matches_smpl=right_root_matches,
        left_sample_rate_matches_smpl=left_rate_matches,
        right_sample_rate_matches_smpl=right_rate_matches,
        left_fine_tune_matches_smpl=left_fine_matches,
        right_fine_tune_matches_smpl=right_fine_matches,
        left_window_fields_match_smpl=left_window_matches,
        right_window_fields_match_smpl=right_window_matches,
        left_match_method=left_match.method,
        right_match_method=right_match.method,
        left_match_quality=match_quality_for(left_match.method),
        right_match_quality=match_quality_for(right_match.method),
        left_link_candidate_count=left_match.candidate_count,
        right_link_candidate_count=right_match.candidate_count,
        left_match_candidate_count=len(left_match.candidate_refs),
        right_match_candidate_count=len(right_match.candidate_refs),
        left_candidate_offsets=joined_offsets(left_match.candidate_refs),
        right_candidate_offsets=joined_offsets(right_match.candidate_refs),
        left_candidate_names=joined_names(left_match.candidate_refs),
        right_candidate_names=joined_names(right_match.candidate_refs),
        left_candidate_link_ids_0x078=joined_link_ids(left_match.candidate_refs),
        right_candidate_link_ids_0x078=joined_link_ids(right_match.candidate_refs),
        link_ids_match_smpl=link_ids_match,
        names_match_smpl=names_match,
        sbnk_fields_match_smpl=sbnk_fields_match,
        exact_equal_length_pair=exact_equal,
        same_rate_pair=same_rate,
        notes=";".join(notes),
    )


def write_csv(path: Path, rows: list[SbnkLink]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fields = list(asdict(rows[0]).keys())
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow(asdict(row))


def write_json(path: Path, rows: list[SbnkLink]) -> None:
    path.write_text(json.dumps([asdict(row) for row in rows], indent=2) + "\n", encoding="utf-8")


def write_summary(path: Path, rows: list[SbnkLink]) -> None:
    linked_member_count = sum(1 for row in rows if row.left_smpl_offset is not None) + sum(
        1 for row in rows if row.right_smpl_offset is not None
    )
    summary = {
        "sbnk_count": len(rows),
        "single_member_bank_count": sum(1 for row in rows if row.bank_topology == "single-member"),
        "two_member_bank_count": sum(1 for row in rows if row.bank_topology == "two-member"),
        "single_member_unused_zero_right_link_count": sum(
            1 for row in rows if row.right_link_role == "unused-zero"
        ),
        "single_member_unused_mirrors_left_link_count": sum(
            1 for row in rows if row.right_link_role == "unused-mirrors-left-link"
        ),
        "single_member_unused_nonzero_right_link_count": sum(
            1 for row in rows if row.right_link_role == "unused-nonzero"
        ),
        "known_single_member_count": sum(
            1
            for row in rows
            if row.bank_topology == "single-member" and row.known_member_count == 1
        ),
        "known_two_member_count": sum(
            1 for row in rows if row.bank_topology == "two-member" and row.known_member_count == 2
        ),
        "matched_pair_count": sum(
            1
            for row in rows
            if row.left_smpl_offset is not None and row.right_smpl_offset is not None
        ),
        "linked_by_ids_count": sum(1 for row in rows if row.link_ids_match_smpl),
        "names_match_count": sum(1 for row in rows if row.names_match_smpl),
        "linked_member_count": linked_member_count,
        "sbnk_window_fields_match_count": sum(1 for row in rows if row.sbnk_fields_match_smpl),
        "member_sample_rate_matches_smpl_count": sum(
            int(row.left_sample_rate_matches_smpl) + int(row.right_sample_rate_matches_smpl)
            for row in rows
        ),
        "member_root_key_matches_smpl_count": sum(
            int(row.left_root_key_matches_smpl) + int(row.right_root_key_matches_smpl)
            for row in rows
        ),
        "member_fine_tune_matches_smpl_count": sum(
            int(row.left_fine_tune_matches_smpl) + int(row.right_fine_tune_matches_smpl)
            for row in rows
        ),
        "member_window_fields_match_smpl_count": sum(
            int(row.left_window_fields_match_smpl) + int(row.right_window_fields_match_smpl)
            for row in rows
        ),
        "pitch_base_word_pitch_formula_match_count": sum(
            1 for row in rows if row.pitch_base_word_matches_pitch_formula
        ),
        "pitch_base_word_pitch_formula_mismatch_count": sum(
            1 for row in rows if not row.pitch_base_word_matches_pitch_formula
        ),
        "pitch_base_word_stored_image_exception_count": sum(
            1 for row in rows if row.pitch_base_word_status == PITCH_BASE_STATUS_STORED_EXCEPTION
        ),
        "pitch_base_word_formula_unavailable_count": sum(
            1 for row in rows if row.pitch_base_word_status == PITCH_BASE_STATUS_FORMULA_UNAVAILABLE
        ),
        "secondary_pitch_base_word_two_member_pitch_formula_match_count": sum(
            1
            for row in rows
            if row.right_slot_present and row.secondary_pitch_base_word_matches_pitch_formula
        ),
        "secondary_pitch_base_word_two_member_pitch_formula_mismatch_count": sum(
            1
            for row in rows
            if row.right_slot_present
            and row.secondary_pitch_base_word_matches_pitch_formula is False
        ),
        "secondary_pitch_base_word_two_member_stored_image_exception_count": sum(
            1
            for row in rows
            if row.right_slot_present
            and row.secondary_pitch_base_word_status == PITCH_BASE_STATUS_STORED_EXCEPTION
        ),
        "clean_pitch_base_word_for_write_available_count": sum(
            1 for row in rows if row.clean_pitch_base_word_for_write_0x0de is not None
        ),
        "clean_secondary_pitch_base_word_for_write_available_count": sum(
            1 for row in rows if row.clean_secondary_pitch_base_word_for_write_0x0e0 is not None
        ),
        "secondary_pitch_base_word_single_member_noncanonical_count": sum(
            1 for row in rows if not row.right_slot_present
        ),
        "equal_length_pair_count": sum(1 for row in rows if row.exact_equal_length_pair),
        "same_rate_different_length_count": sum(
            1 for row in rows if row.same_rate_pair and not row.exact_equal_length_pair
        ),
        "different_rate_pair_count": sum(
            1
            for row in rows
            if row.left_smpl_offset is not None
            and row.right_smpl_offset is not None
            and not row.same_rate_pair
        ),
        "ambiguous_link_id_pair_count": sum(
            1
            for row in rows
            if "ambiguous" in row.left_match_method or "ambiguous" in row.right_match_method
        ),
        "known_pair_match_count": sum(
            1
            for row in rows
            if row.left_match_quality == "Known" and row.right_match_quality == "Known"
        ),
        "name_disambiguated_pair_count": sum(
            1
            for row in rows
            if row.left_match_method == "link+name" and row.right_match_method == "link+name"
        ),
        "two_member_name_only_match_count": sum(
            1
            for row in rows
            if row.bank_topology == "two-member"
            and (row.left_match_method == "name-only" or row.right_match_method == "name-only")
        ),
    }
    path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


def build_sbnk_links(image: Path, mono_dir: Path) -> list[SbnkLink]:
    """Build detailed current SBNK-to-SMPL rows for one SFS image."""
    smpl_by_link, smpl_by_name = load_smpl_refs(mono_dir)
    object_rows = scan_sfs_objects.scan_image(image, max_nodes=256)
    return [
        parse_sbnk(image, row, smpl_by_link, smpl_by_name)
        for row in object_rows
        if row.get("type") == "SBNK"
    ]


def write_report(output_dir: Path, rows: list[SbnkLink]) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    write_csv(output_dir / "sbnk_links.csv", rows)
    write_json(output_dir / "sbnk_links.json", rows)
    write_summary(output_dir / "summary.json", rows)


def build_report(image: Path, mono_dir: Path, output_dir: Path) -> list[SbnkLink]:
    rows = build_sbnk_links(image, mono_dir)
    write_report(output_dir, rows)
    return rows


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--mono-dir", type=Path, required=True)
    parser.add_argument("--output-dir", "-o", type=Path, required=True)
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    rows = build_report(args.image, args.mono_dir, args.output_dir)

    print(f"SBNK objects: {len(rows)}")
    print(f"single-member banks: {sum(1 for row in rows if row.bank_topology == 'single-member')}")
    print(f"two-member banks: {sum(1 for row in rows if row.bank_topology == 'two-member')}")
    print(f"linked by SMPL+0x078 IDs: {sum(1 for row in rows if row.link_ids_match_smpl)}")
    print(f"names match linked SMPLs: {sum(1 for row in rows if row.names_match_smpl)}")
    print(
        f"SBNK window fields mirror linked SMPLs: {sum(1 for row in rows if row.sbnk_fields_match_smpl)}"
    )
    print(
        "member sample-rate fields match linked SMPLs: "
        f"{sum(int(row.left_sample_rate_matches_smpl) + int(row.right_sample_rate_matches_smpl) for row in rows)}"
    )
    print(
        "pitch base words match pitch formula: "
        f"{sum(1 for row in rows if row.pitch_base_word_matches_pitch_formula)}"
    )
    print(
        "stored pitch base exceptions: "
        f"{sum(1 for row in rows if row.pitch_base_word_status == PITCH_BASE_STATUS_STORED_EXCEPTION)}"
    )
    print(
        "secondary pitch base words match pitch formula for two-member banks: "
        f"{sum(1 for row in rows if row.right_slot_present and row.secondary_pitch_base_word_matches_pitch_formula)}"
    )
    print(f"reports written to {args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
