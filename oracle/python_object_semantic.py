"""Emit canonical current-object semantics from the Python implementation."""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict
from pathlib import Path
from typing import Any, cast

from axklib.containers import sfs_dump, sfs_extents, sfs_inventory
from axklib.objects.current import decode_current_smpl_metadata, summarize_object_header
from axklib.parameters.current import (
    CurrentSbnkMember,
    decode_current_sbac_fields,
    decode_current_sbnk_members,
    iter_prog_assignments,
)

ROOT = Path(__file__).resolve().parents[1]
FIELD_DATA = ROOT / "cpp" / "data" / "current-sbnk-fields.json"


def _member(value: CurrentSbnkMember) -> dict[str, object]:
    row = asdict(value)
    return {
        key: row[key]
        for key in (
            "sample_name",
            "smpl_link_id",
            "root_key",
            "sample_rate",
            "fine_tune_cents",
            "pitch_base_word",
            "wave_length_frames",
            "loop_start_frame",
            "loop_length_frames",
        )
    }


def _numeric_fields(payload: bytes) -> dict[str, int | None]:
    descriptors = cast(
        list[dict[str, Any]],
        json.loads(FIELD_DATA.read_text(encoding="utf-8"))["sbnk_fields"],
    )
    result: dict[str, int | None] = {}
    for descriptor in descriptors:
        offset = int(descriptor["offset"])
        width = int(descriptor["width"])
        result[str(descriptor["name"])] = (
            int.from_bytes(
                payload[offset : offset + width],
                "big",
                signed=bool(descriptor["signed"]),
            )
            if offset + width <= len(payload)
            else None
        )
    return result


def _decoded(payload: bytes, raw_type: str, header: dict[str, Any]) -> dict[str, object]:
    if raw_type == "SMPL":
        sample = decode_current_smpl_metadata(payload)
        return {
            "kind": "SMPL",
            "sample_rate": int(header["sample_rate_guess"]),
            "stored_sample_width_bytes": int(header["bytes_per_sample_guess"]),
            "source_wave_name": sample.source_wave_name_guess,
            "group_id": sample.smpl_group_id_0x06c,
            "link_id": sample.smpl_link_id_0x078,
            "duplicate_sample_rate": sample.sample_rate_duplicate_0x07c,
            "root_key": sample.root_key_midi_note_guess,
            "fine_tune_cents": sample.fine_tune_cents_guess,
            "loop_mode": sample.loop_mode_candidate_0x085,
            "loop_mode_label": sample.loop_mode_a4000_ui_label_guess,
            "wave_length_frames": sample.wave_length_frames_0x092,
            "loop_start_frame": sample.loop_start_frame_0x096,
            "loop_length_frames": sample.loop_length_frames_0x09a,
            "loop_end_frame_inclusive": sample.loop_end_frame_inclusive_guess,
            "loop_end_frame_exclusive": sample.loop_end_frame_exclusive_guess,
            "stored_pcm_offset": int(header["header_size"]),
            "stored_pcm_bytes": int(header["payload_bytes_0x1c"]),
            "compact_record_hex": sample.current_smpl_compact_record_hex,
        }
    if raw_type == "SBNK":
        bank = decode_current_sbnk_members(payload)
        controls = bank.member_parameters.sample_control_records
        return {
            "kind": "SBNK",
            "bank_name": bank.bank_name,
            "instrument_name": bank.instrument_name,
            "right_slot_present": bank.right_slot_present,
            "right_link_role": bank.right_link_role,
            "left": _member(bank.left),
            "right": _member(bank.right) if bank.right is not None else None,
            "inactive_right": _member(bank.inactive_right),
            "linked_program_bitmap_words": list(bank.linked_program_bitmap_words),
            "linked_program_numbers": list(bank.linked_program_numbers),
            "numeric_fields": _numeric_fields(payload),
            "control_records": [
                [row.device_u8, row.function_u8, row.type_u8, row.range_s8]
                for row in controls
                if row.offset + 4 <= len(payload)
            ],
            "raw_parameter_window_hex": payload[0xA8 : min(len(payload), 0x185)].hex(),
        }
    if raw_type == "SBAC":
        group = decode_current_sbac_fields(payload)
        return {
            "kind": "SBAC",
            "raw_sample_parameter_block_hex": group.sample_parameter_block_raw_0x040_0x11f.hex(),
            "value_enable_words": list(group.value_enable_words_0x120_0x12b),
            "enabled_parameter_numbers": list(group.enabled_sample_parameter_p2),
            "enabled_numbers_outside_table": list(
                group.enabled_sample_parameter_p2_over_table_range
            ),
            "bulk_assigned_sample_count": group.bulk_assigned_sample_count_0x130,
            "active_slot_count": group.active_slot_count_0x144,
            "maximum_slot_count": group.max_slot_count_from_payload,
            "slots": [
                {"name": row.name, "raw_handle": row.raw_handle_0x10, "offset": row.offset}
                for row in group.slots
            ],
        }
    if raw_type == "PROG":
        assignments = iter_prog_assignments(payload)
        return {
            "kind": "PROG",
            "raw_control_block_hex": payload[0x110:0x120].hex(),
            "raw_control_tail_copy_hex": payload[0x358:0x368].hex(),
            "effect_blocks_hex": [payload[start : start + 0x28].hex() for start in (0x98, 0xC0, 0xE8)],
            "assignments": [
                {
                    "name": row.name,
                    "raw_handle": row.raw_handle_0x10,
                    "kind": row.kind_byte_0x14,
                    "flags": row.flag_byte_0x15,
                    "level_offset": row.level_offset_0x16,
                    "velocity_sensitivity": row.velocity_sensitivity_0x17,
                    "pan_offset": row.pan_offset_0x18,
                    "key_limit_high": row.key_limit_high_0x1e,
                    "key_limit_low": row.key_limit_low_0x1f,
                    "velocity_limit_high": row.velocity_limit_high_0x21,
                    "velocity_limit_low": row.velocity_limit_low_0x22,
                    "raw_row_hex": row.raw_row.hex(),
                }
                for row in assignments
            ],
        }
    return {"kind": raw_type, "raw_payload_hex": payload.hex()}


def semantic_value(path: Path) -> dict[str, object]:
    parsed = cast(
        dict[str, Any],
        sfs_dump.parse_image(
            path, sfs_dump.ReadOptions(max_nodes=0, include_node_payloads=False)
        ),
    )
    sector_size = int(parsed["sector_size_bytes"])
    objects: list[dict[str, object]] = []
    for partition in parsed["partitions"]:
        rows = sfs_inventory.scan_ynode_records(path, partition, [], sector_size=sector_size)
        for row in rows:
            if row.payload_kind != "object":
                continue
            record = sfs_inventory.ynode_to_index_record(row)
            with sfs_dump.ImageReader(path) as reader:
                payload = sfs_extents.read_index_record_data(
                    reader,
                    reader.read_at(record.record_offset, sfs_inventory.INDEX_RECORD_SIZE),
                    partition_start_sector=int(partition["start_sector"]),
                    sector_size=sector_size,
                    sectors_per_cluster=sfs_inventory.field_value(partition, "sectors_per_cluster"),
                    cluster_count_limit=sfs_inventory.field_value(partition, "number_of_clusters"),
                ).data
            summary = cast(dict[str, Any], summarize_object_header(payload))
            header: dict[str, Any] = {
                "raw_type": str(summary["type"]),
                "name": str(summary["name_guess"]),
                "header_size": int(summary["header_size"]),
                "unknown_0x14": int(summary["unknown_0x14"]),
                "record_size_or_header_used": int(summary["record_size_or_header_used"]),
                "payload_bytes_0x1c": int(summary["payload_bytes_0x1c"]),
                "payload_bytes_0x20": int(summary["payload_bytes_0x20"]),
                "raw_prefix_hex": payload[:64].hex(),
            }
            objects.append(
                {
                    "partition_index": int(partition["index"]),
                    "sfs_id": row.sfs_id,
                    "header": header,
                    "decoded": _decoded(payload, row.object_type, summary),
                }
            )
    return {"schema_version": "1.0", "objects": objects}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--pretty", action="store_true")
    args = parser.parse_args()
    print(json.dumps(semantic_value(args.image), indent=2 if args.pretty else None, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
