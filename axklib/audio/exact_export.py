"""Exact current sample export placement and stereo helpers."""

from __future__ import annotations

import csv
import json
import re
import shutil
import wave
from collections.abc import Sequence
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Literal, cast

from axklib.audio import stereo as combine_sfs_stereo_waves
from axklib.containers import sfs_scan as scan_sfs_objects
from axklib.parameters import sbnk_links

OverwritePolicy = Literal["fail", "replace"]
StereoPolicy = Literal["none", "auto"]


def _replace_enabled(overwrite_policy: OverwritePolicy) -> bool:
    if overwrite_policy == "replace":
        return True
    if overwrite_policy == "fail":
        return False
    raise ValueError(f"unknown overwrite policy: {overwrite_policy}")


def _ensure_writable(path: Path, *, overwrite_policy: OverwritePolicy) -> None:
    if path.exists() and not _replace_enabled(overwrite_policy):
        raise FileExistsError(f"refusing to overwrite existing file: {path}")


def write_text_file(path: Path, text: str, *, overwrite_policy: OverwritePolicy = "fail") -> None:
    _ensure_writable(path, overwrite_policy=overwrite_policy)
    path.write_text(text, encoding="utf-8")


def copy_file(source: Path, destination: Path, *, overwrite_policy: OverwritePolicy = "fail") -> None:
    _ensure_writable(destination, overwrite_policy=overwrite_policy)
    shutil.copy2(source, destination)


def safe_name(value: str) -> str:
    value = value.strip() or "sample"
    value = re.sub(r"[^A-Za-z0-9._ -]+", "_", value)
    value = re.sub(r"\s+", "_", value)
    return value.strip("._-") or "sample"


@dataclass
class MonoExport:
    object_offset: int
    name: str
    sample_rate: int
    sample_width_bytes: int
    frames: int
    source_wav_path: str
    exported_wav_path: str
    exported_json_path: str
    partition_index: int | None
    partition_name: str
    volume_name: str
    category_name: str
    organization_source: str = ""
    organization_relationship_path: str = ""
    organization_relationship_quality: str = ""
    organization_owner_object_offset: int | None = None


@dataclass
class PairExport:
    source_image: str
    sbnk_offset: int
    sbnk_cluster: int
    bank_name: str
    instrument_name: str
    sample_name_left: str
    sample_name_right: str
    bank_topology: str
    right_slot_present: bool
    right_link_role: str
    known_member_count: int
    left_smpl_offset: int | None
    right_smpl_offset: int | None
    left_wav_path: str
    right_wav_path: str
    left_sample_rate: int | None
    right_sample_rate: int | None
    left_sample_width_bytes: int | None
    right_sample_width_bytes: int | None
    left_frames: int | None
    right_frames: int | None
    left_root_key_0x0d6: int
    right_root_key_0x0d7: int
    left_sample_rate_0x0d8: int
    right_sample_rate_0x0da: int
    left_fine_tune_cents_0x0dc: int
    right_fine_tune_cents_0x0dd: int
    pitch_base_word_0x0de: int
    secondary_pitch_base_word_0x0e0: int
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
    left_wave_length_frames_0x0f0: int
    right_wave_length_frames_0x0f4: int
    left_loop_start_frame_0x0f8: int
    right_loop_start_frame_0x0fc: int
    left_loop_length_frames_0x100: int
    right_loop_length_frames_0x104: int
    left_match_method: str
    right_match_method: str
    left_match_quality: str
    right_match_quality: str
    left_candidate_offsets: str
    right_candidate_offsets: str
    link_ids_match_smpl: bool
    names_match_smpl: bool
    sbnk_fields_match_smpl: bool
    exact_stereo_representable: bool
    stereo_wav_path: str
    export_policy: str
    partition_index: int | None
    partition_name: str
    volume_name: str
    category_name: str
    organization_source: str
    organization_relationship_path: str
    organization_relationship_quality: str
    organization_owner_object_offset: int | None
    notes: str


@dataclass
class ObjectLocation:
    partition_index: int
    partition_name: str
    volume_name: str
    category_code: str
    category_name: str
    entry_name: str
    match_quality: str
    location_source: str = "inventory"
    relationship_path: str = ""
    owner_object_offset: int | None = None


@dataclass
class DerivedObjectLocation:
    object_offset: int
    object_type: str
    object_name: str
    partition_index: int
    partition_name: str
    volume_name: str
    category_code: str
    category_name: str
    inherited_from_offset: int | None
    inherited_from_type: str
    inherited_from_name: str
    relationship_path: str
    relationship_quality: str
    location_source: str
    notes: str


def read_json(path: Path) -> dict[str, object]:
    return cast(dict[str, object], json.loads(path.read_text(encoding="utf-8")))


def load_mono_metadata(mono_dir: Path) -> dict[int, dict[str, object]]:
    metadata: dict[int, dict[str, object]] = {}
    for path in sorted(mono_dir.glob("*.json")):
        item = read_json(path)
        offset = int(str(item.get("object_offset", 0)), 0)
        wav_path = Path(str(item.get("wav_path", "")))
        if not wav_path.is_absolute():
            wav_path = path.parent / wav_path.name
        item["resolved_wav_path"] = str(wav_path)
        item["source_json_path"] = str(path)
        metadata[offset] = item
    return metadata


def wav_info(path: Path) -> tuple[int, int, int, int]:
    with wave.open(str(path), "rb") as wav:
        return wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getnframes()


def mono_export_name(item: dict[str, object]) -> str:
    image_stem = Path(str(item.get("source_image", "image"))).stem
    offset = int(str(item.get("object_offset", 0)), 0)
    name = safe_name(str(item.get("name_guess", "sample")))
    return f"{image_stem}_{offset:08x}_{name}"


def direct_inventory_location_source(category_code: str) -> str:
    if category_code == "SMPL":
        return "direct-smpl-category-visibility"
    if category_code == "SBNK":
        return "direct-sbnk-category-visibility"
    if category_code == "SBAC":
        return "direct-sbac-category-visibility"
    if category_code == "PROG":
        return "direct-prog-category-visibility"
    if category_code == "SEQU":
        return "direct-sequ-category-visibility"
    return "direct-inventory-category-visibility"


def direct_inventory_relationship_path(category_code: str) -> str:
    return f"{category_code or 'object'}-category-entry"


def load_object_locations(inventory_dir: Path | None) -> dict[int, ObjectLocation]:
    if inventory_dir is None:
        return {}
    path = inventory_dir / "volume_objects.csv"
    if not path.exists():
        return {}
    locations: dict[int, ObjectLocation] = {}
    with path.open(newline="", encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            value = row.get("object_offset", "")
            if not value:
                continue
            quality = row.get("match_quality", "Known")
            if quality != "Known":
                continue
            category_code = row.get("category_code", "")
            offset = int(value)
            locations[offset] = ObjectLocation(
                partition_index=int(row.get("partition_index", 0)),
                partition_name=row.get("partition_name", ""),
                volume_name=row.get("volume_name", ""),
                category_code=category_code,
                category_name=row.get("category_name", category_code),
                entry_name=row.get("entry_name", ""),
                match_quality=quality,
                location_source=direct_inventory_location_source(category_code),
                relationship_path=direct_inventory_relationship_path(category_code),
            )
    return locations



def parse_optional_int(value: object) -> int | None:
    text = str(value or "").strip()
    if not text:
        return None
    return int(text, 0)


def clone_inherited_location(
    owner: ObjectLocation,
    *,
    source: str,
    relationship_path: str,
    owner_object_offset: int | None,
    relationship_quality: str,
) -> ObjectLocation:
    return ObjectLocation(
        partition_index=owner.partition_index,
        partition_name=owner.partition_name,
        volume_name=owner.volume_name,
        category_code=owner.category_code,
        category_name=owner.category_name,
        entry_name=owner.entry_name,
        match_quality=relationship_quality,
        location_source=source,
        relationship_path=relationship_path,
        owner_object_offset=owner_object_offset,
    )


def maybe_add_derived_location(
    locations: dict[int, ObjectLocation],
    derived_rows: list[DerivedObjectLocation],
    *,
    target_offset: int | None,
    target_type: str,
    target_name: str,
    inherited_from_offset: int | None,
    inherited_from_type: str,
    inherited_from_name: str,
    inherited_location: ObjectLocation | None,
    relationship_path: str,
    relationship_quality: str,
    location_source: str,
) -> bool:
    if target_offset is None or inherited_location is None or target_offset in locations:
        return False
    locations[target_offset] = clone_inherited_location(
        inherited_location,
        source=location_source,
        relationship_path=relationship_path,
        owner_object_offset=inherited_from_offset,
        relationship_quality=relationship_quality,
    )
    derived_rows.append(
        DerivedObjectLocation(
            object_offset=target_offset,
            object_type=target_type,
            object_name=target_name,
            partition_index=inherited_location.partition_index,
            partition_name=inherited_location.partition_name,
            volume_name=inherited_location.volume_name,
            category_code=inherited_location.category_code,
            category_name=inherited_location.category_name,
            inherited_from_offset=inherited_from_offset,
            inherited_from_type=inherited_from_type,
            inherited_from_name=inherited_from_name,
            relationship_path=relationship_path,
            relationship_quality=relationship_quality,
            location_source=location_source,
            notes="filled_previous_unmapped_location_only",
        )
    )
    return True


def read_csv_rows(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open(newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def inventory_object_key(row: dict[str, str], *, id_field: str) -> str:
    partition = parse_optional_int(row.get("partition_index"))
    sfs_id = parse_optional_int(row.get(id_field))
    if partition is None or sfs_id is None:
        return ""
    return f"p{partition}:sfs{sfs_id}"


def load_inventory_key_offsets(inventory_dir: Path | None) -> dict[str, int]:
    if inventory_dir is None:
        return {}
    result: dict[str, int] = {}
    for row in read_csv_rows(inventory_dir / "ynode_records.csv"):
        key = inventory_object_key(row, id_field="sfs_id")
        offset = parse_optional_int(row.get("payload_offset"))
        if key and offset is not None:
            result[key] = offset
    return result


def apply_volume_disambiguated_sbac_locations(
    locations: dict[int, ObjectLocation],
    derived_rows: list[DerivedObjectLocation],
    *,
    sbac_volume_disambiguation_dir: Path | None,
    inventory_dir: Path | None,
) -> bool:
    if sbac_volume_disambiguation_dir is None:
        return False
    key_offsets = load_inventory_key_offsets(inventory_dir)
    changed = False
    for row in read_csv_rows(sbac_volume_disambiguation_dir / "current_sbac_volume_disambiguation.csv"):
        quality = row.get("volume_match_quality", "")
        selected_key = row.get("volume_selected_sbnk_object_key", "")
        sbac_key = row.get("sbac_object_key", "")
        if quality != "Likely" or not selected_key or not sbac_key:
            continue
        sbac_offset = key_offsets.get(sbac_key)
        target_offset = key_offsets.get(selected_key)
        method = row.get("volume_match_method", "")
        source = "current-sbac-volume-disambiguated-relationship"
        if "hidden-candidate+volume-handle-sfs-sequence" in method:
            source = "current-sbac-volume-sequence-relationship"
        changed = maybe_add_derived_location(
            locations,
            derived_rows,
            target_offset=target_offset,
            target_type="SBNK",
            target_name=row.get("slot_sbnk_name", ""),
            inherited_from_offset=sbac_offset,
            inherited_from_type="SBAC",
            inherited_from_name=row.get("sbac_name", ""),
            inherited_location=locations.get(sbac_offset or -1),
            relationship_path="SBAC->SBNK",
            relationship_quality=quality,
            location_source=source,
        ) or changed
    return changed


def apply_bank_relationship_locations(
    locations: dict[int, ObjectLocation],
    bank_relationships_dir: Path | None,
    inventory_dir: Path | None = None,
    sbac_volume_disambiguation_dir: Path | None = None,
) -> list[DerivedObjectLocation]:
    if bank_relationships_dir is None:
        return []
    prog_rows = read_csv_rows(bank_relationships_dir / "current_prog_bank_links.csv")
    sbac_rows = read_csv_rows(bank_relationships_dir / "current_sbac_sbnk_links.csv")
    derived_rows: list[DerivedObjectLocation] = []
    changed = True
    while changed:
        changed = False
        for row in prog_rows:
            quality = row.get("match_quality", "")
            target_type = row.get("matched_target_type", "")
            if quality not in {"Known", "Likely"} or target_type not in {"SBNK", "SBAC"}:
                continue
            prog_offset = parse_optional_int(row.get("prog_payload_offset"))
            target_offset = parse_optional_int(row.get("matched_target_payload_offset"))
            changed = maybe_add_derived_location(
                locations,
                derived_rows,
                target_offset=target_offset,
                target_type=target_type,
                target_name=row.get("matched_target_name", ""),
                inherited_from_offset=prog_offset,
                inherited_from_type="PROG",
                inherited_from_name=row.get("prog_name", ""),
                inherited_location=locations.get(prog_offset or -1),
                relationship_path=f"PROG->{target_type}",
                relationship_quality=quality,
                location_source="current-prog-bank-relationship",
            ) or changed
        for row in sbac_rows:
            if row.get("match_quality", "") != "Known":
                continue
            sbac_offset = parse_optional_int(row.get("sbac_payload_offset"))
            target_offset = parse_optional_int(row.get("matched_sbnk_payload_offset"))
            changed = maybe_add_derived_location(
                locations,
                derived_rows,
                target_offset=target_offset,
                target_type="SBNK",
                target_name=row.get("matched_sbnk_name", ""),
                inherited_from_offset=sbac_offset,
                inherited_from_type="SBAC",
                inherited_from_name=row.get("sbac_name", ""),
                inherited_location=locations.get(sbac_offset or -1),
                relationship_path="SBAC->SBNK",
                relationship_quality="Known",
                location_source="current-sbac-sbnk-relationship",
            ) or changed
        changed = apply_volume_disambiguated_sbac_locations(
            locations,
            derived_rows,
            sbac_volume_disambiguation_dir=sbac_volume_disambiguation_dir,
            inventory_dir=inventory_dir,
        ) or changed
    return derived_rows


def apply_sbnk_member_locations(
    locations: dict[int, ObjectLocation],
    sbnk_rows: Sequence[Any],
) -> list[DerivedObjectLocation]:
    derived_rows: list[DerivedObjectLocation] = []
    for row in sbnk_rows:
        sbnk_location = locations.get(row.sbnk_offset)
        if sbnk_location is None:
            continue
        maybe_add_derived_location(
            locations,
            derived_rows,
            target_offset=row.left_smpl_offset,
            target_type="SMPL",
            target_name=row.left_smpl_name,
            inherited_from_offset=row.sbnk_offset,
            inherited_from_type="SBNK",
            inherited_from_name=row.bank_name,
            inherited_location=sbnk_location if row.left_match_quality == "Known" else None,
            relationship_path="SBNK->SMPL:left",
            relationship_quality=row.left_match_quality,
            location_source="current-sbnk-smpl-relationship",
        )
        maybe_add_derived_location(
            locations,
            derived_rows,
            target_offset=row.right_smpl_offset,
            target_type="SMPL",
            target_name=row.right_smpl_name,
            inherited_from_offset=row.sbnk_offset,
            inherited_from_type="SBNK",
            inherited_from_name=row.bank_name,
            inherited_location=sbnk_location if row.right_match_quality == "Known" else None,
            relationship_path="SBNK->SMPL:right",
            relationship_quality=row.right_match_quality,
            location_source="current-sbnk-smpl-relationship",
        )
    return derived_rows

def load_sbnk_rows(image: Path, mono_dir: Path) -> list[sbnk_links.SbnkLink]:
    smpl_by_link, smpl_by_name = sbnk_links.load_smpl_refs(mono_dir)
    object_rows = scan_sfs_objects.scan_image(image, max_nodes=256)
    return [
        sbnk_links.parse_sbnk(image, row, smpl_by_link, smpl_by_name)
        for row in object_rows
        if row.get("type") == "SBNK"
    ]


def export_pairs(
    image: Path,
    mono_dir: Path,
    output_dir: Path,
    *,
    inventory_dir: Path | None = None,
    bank_relationships_dir: Path | None = None,
    sbac_volume_disambiguation_dir: Path | None = None,
    overwrite_policy: OverwritePolicy = "fail",
    stereo_policy: StereoPolicy = "auto",
) -> list[PairExport]:
    if stereo_policy not in {"none", "auto"}:
        raise ValueError(f"unknown stereo policy: {stereo_policy}")
    mono_metadata = load_mono_metadata(mono_dir)
    sbnk_rows = load_sbnk_rows(image, mono_dir)
    object_locations = load_object_locations(inventory_dir)
    derived_locations = apply_bank_relationship_locations(
        object_locations,
        bank_relationships_dir,
        inventory_dir,
        sbac_volume_disambiguation_dir,
    )
    derived_locations.extend(apply_sbnk_member_locations(object_locations, sbnk_rows))

    pair_index_dir = output_dir / "_indexes" / "pairs"
    stereo_index_dir = output_dir / "_indexes" / "stereo"
    pair_index_dir.mkdir(parents=True, exist_ok=True)
    stereo_index_dir.mkdir(parents=True, exist_ok=True)

    mono_exports: dict[int, MonoExport] = {
        offset: export_mono(
            item,
            output_dir,
            object_locations.get(offset),
            overwrite_policy=overwrite_policy,
        )
        for offset, item in sorted(mono_metadata.items())
    }
    pair_exports: list[PairExport] = []
    for row in sbnk_rows:
        left = mono_exports.get(row.left_smpl_offset or -1)
        right = mono_exports.get(row.right_smpl_offset or -1)
        exact_stereo = is_known_sbnk_pair(row) and is_exact_stereo_representable(left, right)
        stem = f"{Path(image).stem}_{row.sbnk_offset:08x}_{safe_name(row.bank_name)}"
        pair_location = object_locations.get(row.sbnk_offset)
        pair_output_dir = category_output_dir(output_dir, pair_location, "Sample Banks")
        pair_output_dir.mkdir(parents=True, exist_ok=True)
        stereo_wav_path = ""
        if stereo_policy == "auto" and exact_stereo and left is not None and right is not None:
            stereo_path = pair_output_dir / f"{stem}.wav"
            write_exact_stereo(left, right, stereo_path, overwrite_policy=overwrite_policy)
            stereo_wav_path = str(stereo_path)
            export_policy = "exact-interleaved-stereo"
        elif row.bank_topology == "single-member" and left is not None:
            export_policy = "single-mono-member"
        elif left is not None and right is not None:
            export_policy = "linked-dual-mono"
        else:
            export_policy = "unmatched"

        pair = PairExport(
            source_image=str(image),
            sbnk_offset=row.sbnk_offset,
            sbnk_cluster=row.sbnk_cluster,
            bank_name=row.bank_name,
            instrument_name=row.instrument_name,
            sample_name_left=row.sample_name_left,
            sample_name_right=row.sample_name_right,
            bank_topology=row.bank_topology,
            right_slot_present=row.right_slot_present,
            right_link_role=row.right_link_role,
            known_member_count=row.known_member_count,
            left_smpl_offset=row.left_smpl_offset,
            right_smpl_offset=row.right_smpl_offset,
            left_wav_path=left.exported_wav_path if left else "",
            right_wav_path=right.exported_wav_path if right else "",
            left_sample_rate=left.sample_rate if left else None,
            right_sample_rate=right.sample_rate if right else None,
            left_sample_width_bytes=left.sample_width_bytes if left else None,
            right_sample_width_bytes=right.sample_width_bytes if right else None,
            left_frames=left.frames if left else None,
            right_frames=right.frames if right else None,
            left_root_key_0x0d6=row.left_root_key_0x0d6,
            right_root_key_0x0d7=row.right_root_key_0x0d7,
            left_sample_rate_0x0d8=row.left_sample_rate_0x0d8,
            right_sample_rate_0x0da=row.right_sample_rate_0x0da,
            left_fine_tune_cents_0x0dc=row.left_fine_tune_cents_0x0dc,
            right_fine_tune_cents_0x0dd=row.right_fine_tune_cents_0x0dd,
            pitch_base_word_0x0de=row.pitch_base_word_0x0de,
            secondary_pitch_base_word_0x0e0=row.secondary_pitch_base_word_0x0e0,
            estimated_pitch_base_word_0x0de=row.estimated_pitch_base_word_0x0de,
            pitch_base_word_delta_from_estimate=row.pitch_base_word_delta_from_estimate,
            pitch_base_word_matches_pitch_formula=row.pitch_base_word_matches_pitch_formula,
            pitch_base_word_status=row.pitch_base_word_status,
            clean_pitch_base_word_for_write_0x0de=row.clean_pitch_base_word_for_write_0x0de,
            estimated_secondary_pitch_base_word_0x0e0=row.estimated_secondary_pitch_base_word_0x0e0,
            secondary_pitch_base_word_delta_from_estimate=row.secondary_pitch_base_word_delta_from_estimate,
            secondary_pitch_base_word_matches_pitch_formula=row.secondary_pitch_base_word_matches_pitch_formula,
            secondary_pitch_base_word_status=row.secondary_pitch_base_word_status,
            clean_secondary_pitch_base_word_for_write_0x0e0=row.clean_secondary_pitch_base_word_for_write_0x0e0,
            left_wave_length_frames_0x0f0=row.left_wave_length_frames_0x0f0,
            right_wave_length_frames_0x0f4=row.right_wave_length_frames_0x0f4,
            left_loop_start_frame_0x0f8=row.left_loop_start_frame_0x0f8,
            right_loop_start_frame_0x0fc=row.right_loop_start_frame_0x0fc,
            left_loop_length_frames_0x100=row.left_loop_length_frames_0x100,
            right_loop_length_frames_0x104=row.right_loop_length_frames_0x104,
            left_match_method=row.left_match_method,
            right_match_method=row.right_match_method,
            left_match_quality=row.left_match_quality,
            right_match_quality=row.right_match_quality,
            left_candidate_offsets=row.left_candidate_offsets,
            right_candidate_offsets=row.right_candidate_offsets,
            link_ids_match_smpl=row.link_ids_match_smpl,
            names_match_smpl=row.names_match_smpl,
            sbnk_fields_match_smpl=row.sbnk_fields_match_smpl,
            exact_stereo_representable=exact_stereo,
            stereo_wav_path=stereo_wav_path,
            export_policy=export_policy,
            partition_index=pair_location.partition_index if pair_location else None,
            partition_name=pair_location.partition_name if pair_location else "",
            volume_name=pair_location.volume_name if pair_location else "",
            category_name=pair_location.category_name if pair_location else "",
            organization_source=pair_location.location_source if pair_location else "",
            organization_relationship_path=pair_location.relationship_path if pair_location else "",
            organization_relationship_quality=pair_location.match_quality if pair_location else "",
            organization_owner_object_offset=pair_location.owner_object_offset if pair_location else None,
            notes=row.notes,
        )
        pair_sidecar_path = pair_output_dir / f"{stem}.json"
        write_text_file(
            pair_sidecar_path,
            json.dumps(asdict(pair), indent=2) + "\n",
            overwrite_policy=overwrite_policy,
        )
        write_text_file(
            pair_index_dir / f"{stem}.json",
            json.dumps(asdict(pair), indent=2) + "\n",
            overwrite_policy=overwrite_policy,
        )
        if stereo_wav_path:
            stereo_index = stereo_index_dir / Path(stereo_wav_path).name
            copy_file(Path(stereo_wav_path), stereo_index, overwrite_policy=overwrite_policy)
        pair_exports.append(pair)

    write_csv(output_dir / "sbnk_exact_pairs.csv", pair_exports, overwrite_policy=overwrite_policy)
    write_json(output_dir / "sbnk_exact_pairs.json", pair_exports, overwrite_policy=overwrite_policy)
    write_csv(output_dir / "mono_exports.csv", list(mono_exports.values()), overwrite_policy=overwrite_policy)
    write_json(output_dir / "mono_exports.json", list(mono_exports.values()), overwrite_policy=overwrite_policy)
    write_csv(output_dir / "derived_object_locations.csv", derived_locations, overwrite_policy=overwrite_policy)
    write_json(output_dir / "derived_object_locations.json", derived_locations, overwrite_policy=overwrite_policy)
    write_summary(
        output_dir / "summary.json",
        mono_exports,
        pair_exports,
        derived_locations,
        overwrite_policy=overwrite_policy,
    )
    return pair_exports
def partition_dir_name(location: ObjectLocation) -> str:
    name = safe_name(location.partition_name or f"partition_{location.partition_index:02d}")
    return f"partition_{location.partition_index:02d}_{name}"


def category_output_dir(output_dir: Path, location: ObjectLocation | None, fallback: str) -> Path:
    if location is None:
        return output_dir / "_unmapped" / safe_name(fallback)
    return (
        output_dir
        / partition_dir_name(location)
        / safe_name(location.volume_name)
        / safe_name(location.category_name)
    )


def export_mono(
    item: dict[str, object],
    output_dir: Path,
    location: ObjectLocation | None,
    *,
    overwrite_policy: OverwritePolicy = "fail",
) -> MonoExport:
    source_wav = Path(str(item["resolved_wav_path"]))
    channels, width, rate, frames = wav_info(source_wav)
    if channels != 1:
        raise ValueError(f"expected mono WAV, got {channels} channels: {source_wav}")

    stem = mono_export_name(item)
    mono_output_dir = category_output_dir(output_dir, location, "Samples")
    mono_output_dir.mkdir(parents=True, exist_ok=True)
    wav_path = mono_output_dir / f"{stem}.wav"
    json_path = mono_output_dir / f"{stem}.json"
    copy_file(source_wav, wav_path, overwrite_policy=overwrite_policy)

    updated = dict(item)
    updated.pop("resolved_wav_path", None)
    updated["original_wav_path"] = str(source_wav)
    updated["original_json_path"] = str(item.get("source_json_path", ""))
    updated["wav_path"] = str(wav_path)
    updated["export_mode"] = "sbnk-exact-mono"
    if location is not None:
        updated["partition_index"] = location.partition_index
        updated["partition_name"] = location.partition_name
        updated["volume_name"] = location.volume_name
        updated["category_name"] = location.category_name
        updated["organization_source"] = location.location_source
        updated["organization_relationship_path"] = location.relationship_path
        updated["organization_relationship_quality"] = location.match_quality
        updated["organization_owner_object_offset"] = location.owner_object_offset
    write_text_file(json_path, json.dumps(updated, indent=2) + "\n", overwrite_policy=overwrite_policy)

    return MonoExport(
        object_offset=int(str(item.get("object_offset", 0)), 0),
        name=str(item.get("name_guess", "")),
        sample_rate=rate,
        sample_width_bytes=width,
        frames=frames,
        source_wav_path=str(source_wav),
        exported_wav_path=str(wav_path),
        exported_json_path=str(json_path),
        partition_index=location.partition_index if location else None,
        partition_name=location.partition_name if location else "",
        volume_name=location.volume_name if location else "",
        category_name=location.category_name if location else "",
        organization_source=location.location_source if location else "",
        organization_relationship_path=location.relationship_path if location else "",
        organization_relationship_quality=location.match_quality if location else "",
        organization_owner_object_offset=location.owner_object_offset if location else None,
    )


def is_exact_stereo_representable(left: MonoExport | None, right: MonoExport | None) -> bool:
    return (
        left is not None
        and right is not None
        and left.sample_rate == right.sample_rate
        and left.sample_width_bytes == right.sample_width_bytes
        and left.frames == right.frames
    )


def write_exact_stereo(
    left: MonoExport,
    right: MonoExport,
    output_path: Path,
    *,
    overwrite_policy: OverwritePolicy = "fail",
) -> None:
    left_params, left_pcm = combine_sfs_stereo_waves.read_wav_pcm(Path(left.exported_wav_path))
    right_params, right_pcm = combine_sfs_stereo_waves.read_wav_pcm(Path(right.exported_wav_path))
    if left_params.nchannels != 1 or right_params.nchannels != 1:
        raise ValueError("input WAVs must be mono")
    if left_params.sampwidth != right_params.sampwidth:
        raise ValueError("sample widths differ")
    if left_params.framerate != right_params.framerate:
        raise ValueError("sample rates differ")
    if left_params.nframes != right_params.nframes:
        raise ValueError("frame counts differ")

    stereo_pcm = combine_sfs_stereo_waves.interleave(left_pcm, right_pcm, left_params.sampwidth)
    _ensure_writable(output_path, overwrite_policy=overwrite_policy)
    with wave.open(str(output_path), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(left_params.sampwidth)
        wav.setframerate(left_params.framerate)
        wav.writeframes(stereo_pcm)


def is_known_sbnk_pair(row: Any) -> bool:
    return bool(
        row.bank_topology == "two-member"
        and row.left_match_quality == "Known"
        and row.right_match_quality == "Known"
    )


ExactExportRow = MonoExport | PairExport | DerivedObjectLocation


def write_csv(
    path: Path,
    rows: Sequence[ExactExportRow],
    *,
    overwrite_policy: OverwritePolicy = "fail",
) -> None:
    _ensure_writable(path, overwrite_policy=overwrite_policy)
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fields = list(asdict(rows[0]).keys())
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow(asdict(row))


def write_json(
    path: Path,
    rows: Sequence[ExactExportRow],
    *,
    overwrite_policy: OverwritePolicy = "fail",
) -> None:
    write_text_file(
        path,
        json.dumps([asdict(row) for row in rows], indent=2) + "\n",
        overwrite_policy=overwrite_policy,
    )


def write_summary(
    path: Path,
    mono_exports: dict[int, MonoExport],
    pair_exports: list[PairExport],
    derived_locations: list[DerivedObjectLocation],
    *,
    overwrite_policy: OverwritePolicy = "fail",
) -> None:
    summary = {
        "unique_mono_wavs_exported": len(mono_exports),
        "mono_wavs_with_volume_category": sum(
            1 for row in mono_exports.values() if row.partition_index is not None
        ),
        "mono_wavs_unmapped": sum(1 for row in mono_exports.values() if row.partition_index is None),
        "derived_object_locations": len(derived_locations),
        "sbnk_pair_sidecars": len(pair_exports),
        "sbnk_pair_sidecars_with_volume_category": sum(
            1 for row in pair_exports if row.partition_index is not None
        ),
        "sbnk_pair_sidecars_unmapped": sum(1 for row in pair_exports if row.partition_index is None),
        "single_member_banks": sum(1 for row in pair_exports if row.bank_topology == "single-member"),
        "two_member_banks": sum(1 for row in pair_exports if row.bank_topology == "two-member"),
        "exact_stereo_pairs_representable": sum(1 for row in pair_exports if row.exact_stereo_representable),
        "exact_stereo_wavs_exported": sum(1 for row in pair_exports if row.stereo_wav_path),
        "single_mono_member_banks": sum(1 for row in pair_exports if row.export_policy == "single-mono-member"),
        "linked_dual_mono_only": sum(1 for row in pair_exports if row.export_policy == "linked-dual-mono"),
        "unmatched_sbnk_pairs": sum(
            1
            for row in pair_exports
            if row.bank_topology == "two-member"
            and (row.left_smpl_offset is None or row.right_smpl_offset is None)
        ),
        "different_rate_pairs": sum(
            1
            for row in pair_exports
            if row.left_sample_rate is not None
            and row.right_sample_rate is not None
            and row.left_sample_rate != row.right_sample_rate
        ),
        "same_rate_different_length_pairs": sum(
            1
            for row in pair_exports
            if row.left_sample_rate == row.right_sample_rate
            and row.left_frames is not None
            and row.right_frames is not None
            and row.left_frames != row.right_frames
        ),
    }
    write_text_file(path, json.dumps(summary, indent=2) + "\n", overwrite_policy=overwrite_policy)

