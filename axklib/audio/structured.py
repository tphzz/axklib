"""Structure-preserving WAV export planning and sidecar helpers."""

from __future__ import annotations

import csv
import hashlib
import json
import re
import wave
from collections import Counter, defaultdict, deque
from collections.abc import Callable, Iterable
from dataclasses import asdict, dataclass, replace
from io import BytesIO
from pathlib import Path
from typing import cast

from axklib.audio import (
    OverwritePolicy,
    StereoPolicy,
    Waveform,
    WaveformPlacement,
    WaveformRelationship,
    WavExportProgress,
    _atomic_write_bytes,
    _atomic_write_text,
    _safe_name,
    _wav_bytes,
)
from axklib.audio.stereo import interleave, pad_to_match
from axklib.model import DataQuality
from axklib.reports import to_plain


@dataclass(frozen=True)
class SampleExportTarget:
    stable_id: str
    waveform: Waveform
    relative_wav_path: Path
    relative_json_path: Path
    placement: WaveformPlacement | None
    placement_quality: DataQuality
    placement_source: str
    placement_notes: str = ""
    sampler_sample_key: str = ""
    sampler_sample_name: str = ""
    sample_bank_key: str = ""
    sample_bank_name: str = ""
    export_category: str = "Waveforms"
    source_scope: str = ""


@dataclass(frozen=True)
class WaveExportPlan:
    output_dir: Path
    targets: tuple[SampleExportTarget, ...]
    relationships: tuple[WaveformRelationship, ...] = ()
    issues: tuple[str, ...] = ()


@dataclass(frozen=True)
class StereoExportDecision:
    source_image: str
    scoped_sbnk_key: str
    sbnk_object_key: str
    relationship_quality: str
    basis: str
    partition_name: str
    volume_name: str
    category_name: str
    sample_bank_name: str
    sample_name: str
    left_target_id: str
    right_target_id: str
    left_waveform_name: str
    right_waveform_name: str
    left_waveform_object_key: str
    right_waveform_object_key: str
    decision: str
    reason_code: str
    reason: str
    stereo_wav_path: str = ""
    stereo_json_path: str = ""
    left_mono_wav_path: str = ""
    right_mono_wav_path: str = ""
    left_sample_rate: int | None = None
    right_sample_rate: int | None = None
    left_sample_width_bytes: int | None = None
    right_sample_width_bytes: int | None = None
    left_frame_count: int | None = None
    right_frame_count: int | None = None
    output_frame_count: int | None = None
    left_padding_frames: int = 0
    right_padding_frames: int = 0


_SCOPE_SEPARATOR = "␟"

_STEREO_DECISION_SUCCESS = "interleaved_stereo_written"
_STEREO_DECISION_NOT_EXACT = "kept_physical_only_not_exact_representable"
_STEREO_DECISION_MISSING_SIDE = "kept_physical_only_missing_side"
_STEREO_DECISION_PARENT_CONFLICT = "kept_physical_only_placement_conflict"
_STEREO_DECISION_UNKNOWN = "kept_physical_only_unknown_relationship"

_STEREO_REASON_EXACT = "STEREO_EXACT_INTERLEAVED"
_STEREO_REASON_PADDED = "STEREO_PADDED_SHORTER"
_STEREO_REASON_FRAME_MISMATCH = "STEREO_FRAME_COUNT_MISMATCH"
_STEREO_REASON_RATE_MISMATCH = "STEREO_SAMPLE_RATE_MISMATCH"
_STEREO_REASON_WIDTH_MISMATCH = "STEREO_SAMPLE_WIDTH_MISMATCH"
_STEREO_REASON_MISSING_SIDE = "STEREO_MISSING_LEFT_OR_RIGHT"
_STEREO_REASON_PARENT_CONFLICT = "STEREO_EXPORT_PARENT_CONFLICT"
_STEREO_REASON_UNKNOWN = "STEREO_RELATIONSHIP_NOT_KNOWN"

_USER_CATEGORY_BY_RAW = {
    "SMPL": "Waveforms",
    "Samples": "Waveforms",
}


_INVALID_PATH_CHARS = re.compile(r'[<>:"/\\|?*\x00-\x1f]+')


def _safe_display_path_name(value: str, fallback: str = "sample") -> str:
    text = value.strip() or fallback
    # A-series duplicate-name markers are displayed as trailing asterisks. They
    # help disambiguate objects in the UI, but Windows cannot store `*` in path
    # names. Preserve that user-facing distinction as numeric suffixes: one
    # trailing star becomes ` (2)`, two become ` (3)`, and so on.
    duplicate_match = re.search(r"\s*(\*+)$", text)
    duplicate_suffix = ""
    if duplicate_match:
        duplicate_suffix = f" ({len(duplicate_match.group(1)) + 1})"
        text = text[: duplicate_match.start()].rstrip() or fallback
    text = _INVALID_PATH_CHARS.sub("_", text)
    text = re.sub(r"\s+", " ", text)
    text = re.sub(r"_+", "_", text)
    return (text.strip(" ._") + duplicate_suffix) or fallback


def user_category_name(raw_category: str | None, *, object_type: str = "") -> str:
    if object_type == "SMPL":
        return "Waveforms"
    return _USER_CATEGORY_BY_RAW.get(raw_category or "", raw_category or "Waveforms")


def scoped_object_key(source_image: str, object_key: str) -> str:
    return f"{source_image}{_SCOPE_SEPARATOR}{object_key}"


def _relationship_source_matches(row: WaveformRelationship, source_image: str) -> bool:
    return not row.source_image or row.source_image == source_image


def note_name(midi_note: int | None) -> str | None:
    if midi_note is None:
        return None
    names = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
    return f"{names[midi_note % 12]}{midi_note // 12 - 1}"


def source_scopes(waveforms: tuple[Waveform, ...]) -> dict[str, str]:
    sources = sorted({waveform.source_image for waveform in waveforms}, key=str.lower)
    if len(sources) == 1:
        return {sources[0]: ""}
    return {
        source: f"src{index:04d}_{_safe_name(Path(source).stem or 'source')}"
        for index, source in enumerate(sources, start=1)
    }


def stable_id(
    waveform: Waveform, source_scope: str, used: set[str], *, logical_key: str = ""
) -> str:
    object_key = _safe_name((logical_key or waveform.object_key).replace(":", "_"))
    base = f"{source_scope}__{object_key}" if source_scope else object_key
    candidate = base
    if candidate in used:
        digest = hashlib.sha1(
            f"{waveform.source_image}|{waveform.object_key}|{logical_key}".encode()
        ).hexdigest()[:10]
        candidate = f"{base}__{digest}"
    if candidate in used:
        raise ValueError(f"stable ID collision after hash disambiguation: {candidate}")
    used.add(candidate)
    return candidate


def _partition_dir(placement: WaveformPlacement) -> str:
    if placement.partition_index is None:
        return _safe_display_path_name(
            placement.partition_name or "partition_unknown", "partition_unknown"
        )
    name = _safe_name(placement.partition_name or f"partition_{placement.partition_index:02d}")
    return f"partition_{placement.partition_index:02d}_{name}"


def _placement_for(
    placement_map: dict[str, WaveformPlacement], source_image: str, object_key: str
) -> WaveformPlacement | None:
    return placement_map.get(scoped_object_key(source_image, object_key)) or placement_map.get(
        object_key
    )


def _raw_volume_suffix(raw_volume_path: str) -> str:
    parts = [part for part in raw_volume_path.replace("\\", "/").split("/") if part]
    return parts[-1] if parts else ""


def _export_volume_names_by_raw_path(
    placements: Iterable[WaveformPlacement],
) -> dict[str, str]:
    entries = {
        (placement.partition_name, placement.volume_name, placement.raw_volume_path)
        for placement in placements
        if placement.raw_volume_path
    }
    duplicate_names: Counter[tuple[str, str]] = Counter()
    for partition_name, volume_name, _raw_volume_path in entries:
        duplicate_names[(partition_name, volume_name)] += 1
    names: dict[str, str] = {}
    for partition_name, volume_name, raw_volume_path in entries:
        if duplicate_names[(partition_name, volume_name)] <= 1:
            continue
        suffix = _raw_volume_suffix(raw_volume_path)
        if suffix:
            names[raw_volume_path] = f"{volume_name} ({suffix})"
    return names


def _placement_for_export(
    placement: WaveformPlacement | None, volume_names_by_raw_path: dict[str, str]
) -> WaveformPlacement | None:
    if placement is None or not placement.raw_volume_path:
        return placement
    volume_name = volume_names_by_raw_path.get(placement.raw_volume_path)
    if not volume_name or volume_name == placement.volume_name:
        return placement
    return replace(placement, volume_name=volume_name)


def _single_target_key(row: WaveformRelationship) -> str | None:
    targets = _targets(row.target_key)
    if len(targets) != 1:
        return None
    return targets[0]


def _sample_parent_rows(
    waveform: Waveform, relationships: tuple[WaveformRelationship, ...]
) -> tuple[WaveformRelationship, ...]:
    rows = [
        row
        for row in relationships
        if row.quality == "Known"
        and row.relationship_type in {"SBNK_LEFT_MEMBER_TO_SMPL", "SBNK_RIGHT_MEMBER_TO_SMPL"}
        and _relationship_source_matches(row, waveform.source_image)
        and _single_target_key(row) == waveform.object_key
    ]
    return tuple(
        sorted(rows, key=lambda row: (row.source_image, row.source_key, row.relationship_type))
    )


def _sample_bank_parent_row(
    source_image: str, sample_key: str, relationships: tuple[WaveformRelationship, ...]
) -> WaveformRelationship | None:
    rows = [
        row
        for row in relationships
        if row.quality == "Known"
        and row.relationship_type == "SBAC_SLOT_TO_SBNK"
        and _relationship_source_matches(row, source_image)
        and _single_target_key(row) == sample_key
    ]
    return sorted(rows, key=lambda row: (row.source_image, row.source_key))[0] if rows else None


def _base_parts(source_scope: str, placement: WaveformPlacement | None) -> list[str]:
    parts = [source_scope] if source_scope else []
    if placement is not None and placement.quality == DataQuality.KNOWN:
        if placement.partition_index is not None or placement.partition_name:
            parts.append(_partition_dir(placement))
        parts.append(
            _safe_display_path_name(placement.volume_name or "unknown_volume", "unknown_volume")
        )
    else:
        parts.append("_unplaced")
    return parts


def _dedupe_pair(wav_rel: Path, json_rel: Path, used_paths: set[str]) -> tuple[Path, Path]:
    candidate_wav = wav_rel
    candidate_json = json_rel
    index = 2
    while str(candidate_wav).lower() in used_paths or str(candidate_json).lower() in used_paths:
        candidate_wav = wav_rel.with_name(f"{wav_rel.stem} ({index}){wav_rel.suffix}")
        candidate_json = json_rel.with_name(f"{json_rel.stem} ({index}){json_rel.suffix}")
        index += 1
    used_paths.add(str(candidate_wav).lower())
    used_paths.add(str(candidate_json).lower())
    return candidate_wav, candidate_json


def target_paths(
    waveform: Waveform,
    source_scope: str,
    *,
    layout: str,
    placement: WaveformPlacement | None,
    display_name: str,
    export_category: str,
    sample_bank_name: str = "",
) -> tuple[Path, Path]:
    name = _safe_display_path_name(display_name or waveform.sample_name or "sample")
    if layout == "flat":
        return Path(f"{name}.wav"), Path(f"{name}.json")
    parts = _base_parts(source_scope, placement)
    if export_category == "Sample Banks" and sample_bank_name:
        parts.append("Sample Banks")
        parts.append(_safe_display_path_name(sample_bank_name, "sample bank"))
    else:
        parts.append(export_category)
    base = Path(*parts)
    return base / f"{name}.wav", base / f"{name}.json"


def _volume_root(source_scope: str, placement: WaveformPlacement | None) -> Path:
    return Path(*_base_parts(source_scope, placement))


def _volume_key(source_scope: str, placement: WaveformPlacement | None) -> str:
    return _volume_root(source_scope, placement).as_posix()


def _graph_object_id(object_type: str, source_image: str, object_key: str) -> str:
    digest = hashlib.sha1(f"{source_image}|{object_key}".encode()).hexdigest()[:10]
    return f"{object_type}:{_safe_name(object_key.replace(':', '_'))}:{digest}"


def _graph_source_ref(source_image: str, object_key: str) -> str:
    return scoped_object_key(source_image, object_key)


def _graph_relationship_id(row: WaveformRelationship) -> str:
    digest = hashlib.sha1(
        "|".join(
            (
                row.source_image,
                row.scope_key,
                row.source_key,
                row.relationship_type,
                row.target_key,
                row.basis,
                "" if row.assignment_index is None else str(row.assignment_index),
                row.assignment_row_state,
                row.active_assignment_state,
                row.assignment_rch_assign_display,
            )
        ).encode()
    ).hexdigest()[:12]
    return f"REL:{digest}"


def build_export_plan(
    output_dir: Path,
    waveforms: tuple[Waveform, ...],
    *,
    layout: str,
    placements: dict[str, WaveformPlacement] | None = None,
    relationships: tuple[WaveformRelationship, ...] = (),
) -> WaveExportPlan:
    scopes = source_scopes(waveforms)
    placement_map = placements or {}
    volume_names_by_raw_path = _export_volume_names_by_raw_path(placement_map.values())
    used_ids: set[str] = set()
    used_paths: set[str] = set()
    targets: list[SampleExportTarget] = []
    for waveform in waveforms:
        scope = scopes[waveform.source_image]
        waveform_placement = _placement_for_export(
            _placement_for(placement_map, waveform.source_image, waveform.object_key),
            volume_names_by_raw_path,
        )
        parent_rows = _sample_parent_rows(waveform, relationships)
        if not parent_rows:
            item_id = stable_id(waveform, scope, used_ids)
            placement = waveform_placement
            quality = DataQuality.UNKNOWN if placement is None else placement.quality
            source = (
                "no volume/category placement quality supplied"
                if placement is None
                else placement.source
            )
            notes = (
                "Structured export used source-scoped unplaced waveform directory."
                if placement is None
                else placement.relationship_path
            )
            wav_rel, json_rel = target_paths(
                waveform,
                scope,
                layout=layout,
                placement=placement,
                display_name=waveform.sample_name,
                export_category="Waveforms",
            )
            wav_rel, json_rel = _dedupe_pair(wav_rel, json_rel, used_paths)
            targets.append(
                SampleExportTarget(
                    stable_id=item_id,
                    waveform=waveform,
                    relative_wav_path=wav_rel,
                    relative_json_path=json_rel,
                    placement=placement,
                    placement_quality=quality,
                    placement_source=source,
                    placement_notes=notes,
                    export_category="Waveforms",
                    source_scope=scope,
                )
            )
            continue

        for row in parent_rows:
            sample_key = row.source_key
            sample_placement = _placement_for_export(
                _placement_for(placement_map, waveform.source_image, sample_key),
                volume_names_by_raw_path,
            )
            bank_row = _sample_bank_parent_row(waveform.source_image, sample_key, relationships)
            bank_key = bank_row.source_key if bank_row else ""
            bank_placement = _placement_for_export(
                _placement_for(placement_map, waveform.source_image, bank_key)
                if bank_key
                else None,
                volume_names_by_raw_path,
            )
            placement = sample_placement or bank_placement or waveform_placement
            sample_name = (sample_placement.display_name if sample_placement else "") or sample_key
            bank_name = (
                (bank_placement.display_name if bank_placement else "") if bank_placement else ""
            )
            item_id = stable_id(waveform, scope, used_ids, logical_key=sample_key)
            export_category = "Sample Banks" if bank_name else "Samples"
            wav_rel, json_rel = target_paths(
                waveform,
                scope,
                layout=layout,
                placement=placement,
                display_name=sample_name,
                export_category=export_category,
                sample_bank_name=bank_name,
            )
            wav_rel, json_rel = _dedupe_pair(wav_rel, json_rel, used_paths)
            targets.append(
                SampleExportTarget(
                    stable_id=item_id,
                    waveform=waveform,
                    relative_wav_path=wav_rel,
                    relative_json_path=json_rel,
                    placement=placement,
                    placement_quality=DataQuality.KNOWN if placement else DataQuality.UNKNOWN,
                    placement_source=row.basis,
                    placement_notes=f"{row.relationship_type}; {bank_row.relationship_type if bank_row else 'no SBAC parent'}",
                    sampler_sample_key=sample_key,
                    sampler_sample_name=sample_name,
                    sample_bank_key=bank_key,
                    sample_bank_name=bank_name,
                    export_category=export_category,
                    source_scope=scope,
                )
            )

    return WaveExportPlan(
        output_dir=output_dir,
        targets=tuple(targets),
        relationships=tuple(
            sorted(
                relationships,
                key=lambda row: (
                    row.source_image,
                    row.scope_key,
                    row.source_key,
                    row.relationship_type,
                    row.target_key,
                ),
            )
        ),
        issues=(),
    )


def legacy_sidecar_fields(waveform: Waveform, wav_path: Path) -> dict[str, object]:
    return {
        "source_container": waveform.source_image,
        "container_kind": waveform.container_kind,
        "object_key": waveform.object_key,
        "object_offset": waveform.object_offset,
        "partition_index": waveform.partition_index,
        "object_type": waveform.object_type,
        "name_guess": waveform.sample_name,
        "wav_path": str(wav_path),
        "sample_rate": waveform.sample_rate,
        "channels": waveform.channel_count,
        "sample_width_bytes": waveform.sample_width_bytes,
        "stored_sample_width_bytes": waveform.stored_sample_width_bytes,
        "frames": waveform.frame_count,
        "stored_payload_size": waveform.stored_payload_size,
        "decoded_pcm_size": len(waveform.pcm),
        "stored_payload_transform": waveform.stored_payload_transform,
        "alternating_byte_payload_detected": waveform.alternating_byte_payload_detected,
        "extraction_quality": waveform.quality.quality.value,
        "extraction_basis": waveform.quality.source,
        "extraction_notes": waveform.quality.notes,
        "exactness_status": waveform.exactness_status,
        "export_mode": "sbnk-exact-mono",
        "original_wav_path": str(wav_path),
        "root_key_midi_note_guess": waveform.root_key,
        "fine_tune_cents_guess": waveform.fine_tune,
        "loop_mode_candidate_0x085": waveform.loop_mode,
        "loop_mode_a4000_ui_label_guess": waveform.loop_mode_label,
        "loop_start_frame_0x096": waveform.loop_start,
        "loop_length_frames_0x09a": waveform.loop_length,
        "loop_end_frame_a4000_ui_guess": waveform.loop_end_a4000_ui,
        "field_quality": waveform.field_quality,
        **waveform.metadata,
    }


def _targets(value: str) -> tuple[str, ...]:
    return tuple(part for part in value.split("|") if part)


def _relationship_row(row: WaveformRelationship) -> dict[str, object]:
    return asdict(row)


def _relationship_sidecar_row(row: WaveformRelationship) -> dict[str, object]:
    payload = asdict(row)
    payload.pop("ambiguity_notes", None)
    return payload


def _parents(
    plan: WaveExportPlan, source_image: str, object_key: str
) -> tuple[WaveformRelationship, ...]:
    return tuple(
        row
        for row in plan.relationships
        if _relationship_source_matches(row, source_image)
        and object_key in _targets(row.target_key)
    )


def _children(
    plan: WaveExportPlan, source_image: str, object_key: str
) -> tuple[WaveformRelationship, ...]:
    return tuple(
        row
        for row in plan.relationships
        if _relationship_source_matches(row, source_image) and row.source_key == object_key
    )


def _ancestors(
    plan: WaveExportPlan, source_image: str, object_key: str
) -> tuple[WaveformRelationship, ...]:
    seen_keys: set[str] = set()
    queued: deque[tuple[str, str]] = deque([(source_image, object_key)])
    rows: list[WaveformRelationship] = []
    while queued:
        current_source, current_key = queued.popleft()
        for row in _parents(plan, current_source, current_key):
            parent_source = row.source_image or current_source
            parent_ref = scoped_object_key(parent_source, row.source_key)
            if parent_ref in seen_keys:
                continue
            seen_keys.add(parent_ref)
            rows.append(row)
            queued.append((parent_source, row.source_key))
    return tuple(rows)


def _parent_entries(rows: tuple[WaveformRelationship, ...], prefix: str) -> list[dict[str, object]]:
    return [
        {
            "object_key": row.source_key,
            "relationship_type": row.relationship_type,
            "source_image": row.source_image,
        }
        for row in rows
        if row.relationship_type.startswith(prefix)
    ]


def _target_parent_rows(
    target: SampleExportTarget, parents: tuple[WaveformRelationship, ...]
) -> tuple[WaveformRelationship, ...]:
    if not target.sampler_sample_key:
        return parents
    rows = tuple(row for row in parents if row.source_key == target.sampler_sample_key)
    return rows or parents


def _target_ancestor_rows(
    target: SampleExportTarget, ancestors: tuple[WaveformRelationship, ...]
) -> tuple[WaveformRelationship, ...]:
    if not target.sampler_sample_key:
        return ancestors
    rows = [row for row in ancestors if row.source_key == target.sampler_sample_key]
    queue = [target.sampler_sample_key]
    seen = set(queue)
    while queue:
        child_key = queue.pop(0)
        for row in ancestors:
            if child_key not in _targets(row.target_key):
                continue
            scoped = f"{row.source_image}|{row.source_key}|{row.relationship_type}|{row.target_key}"
            if scoped in seen:
                continue
            seen.add(scoped)
            rows.append(row)
            queue.append(row.source_key)
    return tuple(rows)


def sidecar_v2(
    target: SampleExportTarget,
    wav_path: Path,
    plan: WaveExportPlan,
    stereo_context: tuple[StereoExportDecision, str] | None = None,
) -> dict[str, object]:
    waveform = target.waveform
    relative_wav = target.relative_wav_path.as_posix()
    duration = waveform.frame_count / waveform.sample_rate if waveform.sample_rate else 0.0
    placement = target.placement
    parents = _target_parent_rows(
        target, _parents(plan, waveform.source_image, waveform.object_key)
    )
    ancestors = _target_ancestor_rows(
        target, _ancestors(plan, waveform.source_image, waveform.object_key)
    )
    parent_banks = _parent_entries(parents, "SBNK_")
    parent_sbac = _parent_entries(ancestors, "SBAC_")
    parent_programs = _parent_entries(ancestors, "PROG_")
    sampler_sample_name = target.sampler_sample_name or waveform.sample_name
    sampler_sample_key = target.sampler_sample_key or ""
    playback = {
        "root_key_midi": waveform.root_key,
        "root_key_name": note_name(waveform.root_key),
        "fine_tune_cents": waveform.fine_tune,
        "coarse_tune_semitones": None,
        "loop_mode_raw": waveform.loop_mode,
        "loop_mode_label": waveform.loop_mode_label,
        "loop_start_frame": waveform.loop_start,
        "loop_end_frame": waveform.loop_end_a4000_ui,
        "loop_length_frames": waveform.loop_length,
        "one_shot": waveform.loop_mode_label.startswith("One")
        if waveform.loop_mode_label
        else None,
        "key_low": None,
        "key_high": None,
        "velocity_low": None,
        "velocity_high": None,
    }
    stereo_relationships: dict[str, object] = {
        "stereo_role": "mono",
        "linked_left_sample": None,
        "linked_right_sample": None,
    }
    stereo_warnings: list[str] = []
    if stereo_context is not None:
        decision, role = stereo_context
        counterpart = (
            decision.right_mono_wav_path if role == "left" else decision.left_mono_wav_path
        )
        stereo_relationships.update(
            {
                "stereo_role": role,
                "linked_left_sample": decision.left_waveform_object_key,
                "linked_right_sample": decision.right_waveform_object_key,
                "counterpart_wav_path": counterpart,
                "stereo_decision": decision.decision,
                "stereo_reason_code": decision.reason_code,
                "stereo_reason": decision.reason,
                "stereo_relationship_quality": decision.relationship_quality,
                "stereo_basis": decision.basis,
                "stereo_output_wav_path": decision.stereo_wav_path,
                "stereo_output_frame_count": decision.output_frame_count,
                "left_padding_frames": decision.left_padding_frames,
                "right_padding_frames": decision.right_padding_frames,
            }
        )
        if decision.decision != _STEREO_DECISION_SUCCESS:
            stereo_warnings.append(f"Stereo pair kept as mono: {decision.reason}")
    conversion_warnings = (
        []
        if target.placement_quality == DataQuality.KNOWN
        else ["No known sampler-facing placement was supplied."]
    )
    conversion_warnings.extend(stereo_warnings)
    return {
        "schema": "axklib.wave_sidecar.v2",
        "identity": {
            "sample_name": sampler_sample_name,
            "display_name": sampler_sample_name,
            "object_type": "SBNK" if sampler_sample_key else waveform.object_type,
            "object_key": sampler_sample_key or waveform.object_key,
            "waveform_name": waveform.sample_name,
            "waveform_object_key": waveform.object_key,
            "sample_bank_name": target.sample_bank_name,
            "sample_bank_object_key": target.sample_bank_key,
            "container_kind": waveform.container_kind,
            "partition_index": placement.partition_index if placement else waveform.partition_index,
            "partition_name": placement.partition_name if placement else "",
            "volume_name": placement.volume_name if placement else "",
            "category_name": target.export_category,
            "source_category": placement.category_name if placement else "",
            "stable_id": target.stable_id,
        },
        "audio": {
            "wav_path": relative_wav,
            "channels": waveform.channel_count,
            "sample_rate": waveform.sample_rate,
            "sample_width_bytes": waveform.sample_width_bytes,
            "bit_depth": waveform.sample_width_bytes * 8,
            "frames": waveform.frame_count,
            "duration_seconds": duration,
            "exactness_status": waveform.exactness_status,
            "stored_payload_size": waveform.stored_payload_size,
            "decoded_pcm_size": len(waveform.pcm),
            "stored_payload_transform": waveform.stored_payload_transform,
        },
        "playback": playback,
        "relationships": {
            "canonical_sample_path": relative_wav,
            "parent_sample_banks": parent_banks,
            "parent_sample_bank_accessories": parent_sbac,
            "parent_programs": parent_programs,
            **stereo_relationships,
        },
        "parameters": {
            "sample_playback": playback,
            "sample_bank_membership": parent_banks,
            "program_membership": parent_programs,
            "decoded_current_sbnk_member_parameters": waveform.metadata.get(
                "decoded_current_sbnk_member_parameters", {}
            ),
            "decoded_current_prog_parameters": waveform.metadata.get(
                "decoded_current_prog_parameters", {}
            ),
        },
        "conversion": {
            "recommended_import_name": sampler_sample_name,
            "sample_bank_membership": parent_banks,
            "program_membership": parent_programs,
            "root_key_midi": waveform.root_key,
            "sample_rate": waveform.sample_rate,
            "loop": {
                "mode": waveform.loop_mode_label,
                "start_frame": waveform.loop_start,
                "end_frame": waveform.loop_end_a4000_ui,
                "length_frames": waveform.loop_length,
            },
            "tuning": {
                "fine_tune_cents": waveform.fine_tune,
                "coarse_tune_semitones": None,
            },
            "zones": [],
            "warnings": conversion_warnings,
        },
        "origin": {
            "source_container": waveform.source_image,
            "source_object_ref": waveform.object_key,
            "object_offset": waveform.object_offset,
            "payload_offset": waveform.object_offset,
            "sfs_id": waveform.object_key if waveform.object_key.startswith("p") else "",
            "fat_file": "",
            "iso_raw_group": waveform.metadata.get("iso_raw_group", ""),
            "iso_raw_volume": waveform.metadata.get("iso_raw_volume", ""),
            "iso_group_label": waveform.metadata.get("iso_group_label", ""),
            "iso_volume_label": waveform.metadata.get("iso_volume_label", ""),
            "iso_group_label_source": waveform.metadata.get("iso_group_label_source", ""),
            "iso_volume_label_source": waveform.metadata.get("iso_volume_label_source", ""),
            "iso_recovery_quality": waveform.metadata.get("iso_recovery_quality", ""),
            "source_recovered_metadata": {
                key: value
                for key, value in waveform.metadata.items()
                if key.startswith("source_recovered_")
            },
        },
    }


def _relationship_quality_summary(rows: tuple[WaveformRelationship, ...]) -> str:
    if not rows:
        return "no decoded parent relationship"
    qualities = sorted({row.quality for row in rows})
    return "+".join(qualities)


def _stereo_pcm_and_padding(left: Waveform, right: Waveform) -> tuple[bytes, int, int, int]:
    left_pcm, right_pcm, left_padding_bytes, right_padding_bytes = pad_to_match(
        left.pcm,
        right.pcm,
        left.sample_width_bytes,
    )
    pcm = interleave(left_pcm, right_pcm, left.sample_width_bytes)
    output_frames = len(left_pcm) // left.sample_width_bytes if left.sample_width_bytes else 0
    return (
        pcm,
        output_frames,
        left_padding_bytes // left.sample_width_bytes,
        right_padding_bytes // right.sample_width_bytes,
    )


def _stereo_wav_bytes(left: Waveform, right: Waveform) -> tuple[bytes, int, int, int]:
    pcm, output_frames, left_padding_frames, right_padding_frames = _stereo_pcm_and_padding(
        left, right
    )
    handle = BytesIO()
    with wave.open(handle, "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(left.sample_width_bytes)
        wav.setframerate(left.sample_rate)
        wav.writeframes(pcm)
    return handle.getvalue(), output_frames, left_padding_frames, right_padding_frames


def _target_source_matches(target: SampleExportTarget, source_image: str) -> bool:
    return target.waveform.source_image == source_image or not source_image


def _target_for_relationship_member(
    targets: tuple[SampleExportTarget, ...], row: WaveformRelationship, waveform_key: str
) -> SampleExportTarget | None:
    matches = _targets_for_relationship_member(targets, row, waveform_key)
    return matches[0] if matches else None


def _export_category_priority(target: SampleExportTarget) -> int:
    return {"Sample Banks": 0, "Samples": 1, "Waveforms": 2}.get(target.export_category, 99)


def _targets_for_relationship_member(
    targets: tuple[SampleExportTarget, ...], row: WaveformRelationship, waveform_key: str
) -> tuple[SampleExportTarget, ...]:
    matches = [
        target
        for target in targets
        if target.waveform.object_key == waveform_key
        and _target_source_matches(target, row.source_image)
        and (not target.sampler_sample_key or target.sampler_sample_key == row.source_key)
    ]
    return tuple(
        sorted(
            matches,
            key=lambda target: (
                _export_category_priority(target),
                target.sample_bank_name.lower(),
                target.sampler_sample_name.lower(),
                target.relative_wav_path.as_posix().lower(),
                target.stable_id,
            ),
        )
    )


def _scoped_row_source(row: WaveformRelationship) -> str:
    return row.source_image or row.scope_key


def _trailing_duplicate_base(value: str) -> str | None:
    match = re.search(r"\s*\*+$", value.strip())
    if match is None:
        return None
    base = value.strip()[: match.start()].rstrip()
    return base or None


def _terminal_lr_name(value: str) -> tuple[str, str] | None:
    match = re.match(r"^(.+?\s*)-([LR])(\*+)?$", value.strip())
    if match is None:
        return None
    base = match.group(1).rstrip()
    suffix = match.group(3) or ""
    side = "left" if match.group(2) == "L" else "right"
    if not base:
        return None
    return f"{base}{suffix}", side


def _stereo_member_rows(plan: WaveExportPlan) -> dict[str, dict[str, WaveformRelationship]]:
    members: dict[str, dict[str, WaveformRelationship]] = defaultdict(dict)
    for row in plan.relationships:
        if row.quality not in {"Known", "Likely", "Tentative", "Unknown"}:
            continue
        if row.relationship_type not in {"SBNK_LEFT_MEMBER_TO_SMPL", "SBNK_RIGHT_MEMBER_TO_SMPL"}:
            continue
        targets = _targets(row.target_key)
        if len(targets) != 1:
            continue
        member_key = scoped_object_key(_scoped_row_source(row), row.source_key)
        if row.relationship_type == "SBNK_LEFT_MEMBER_TO_SMPL":
            members[member_key]["left_row"] = row
        elif row.relationship_type == "SBNK_RIGHT_MEMBER_TO_SMPL":
            members[member_key]["right_row"] = row
    return members


def _target_volume_path(target: SampleExportTarget | None) -> tuple[str, str, str]:
    if target is None or target.placement is None:
        return "", "", ""
    return target.placement.partition_name, target.placement.volume_name, target.export_category


def _decision_context(
    left_target: SampleExportTarget | None, right_target: SampleExportTarget | None
) -> tuple[str, str, str, str, str]:
    target = left_target or right_target
    partition_name, volume_name, category_name = _target_volume_path(target)
    sample_bank_name = ""
    sample_name = ""
    for candidate in (left_target, right_target):
        if candidate is None:
            continue
        if not sample_bank_name and candidate.sample_bank_name:
            sample_bank_name = candidate.sample_bank_name
        if not sample_name:
            sample_name = _stereo_display_name(candidate, candidate)
    return partition_name, volume_name, category_name, sample_bank_name, sample_name


def _stereo_interleave_reason(left: Waveform, right: Waveform) -> tuple[str, str]:
    if left.sample_rate != right.sample_rate:
        return (
            _STEREO_REASON_RATE_MISMATCH,
            f"L/R sample rates differ (left={left.sample_rate}, right={right.sample_rate})",
        )
    if left.sample_width_bytes != right.sample_width_bytes:
        return (
            _STEREO_REASON_WIDTH_MISMATCH,
            "L/R sample widths differ "
            f"(left={left.sample_width_bytes} bytes, right={right.sample_width_bytes} bytes)",
        )
    if left.frame_count != right.frame_count:
        left_padding = max(0, right.frame_count - left.frame_count)
        right_padding = max(0, left.frame_count - right.frame_count)
        return (
            _STEREO_REASON_PADDED,
            "L/R frame counts differ; shorter side is padded with trailing zero frames "
            f"(left={left.frame_count}, right={right.frame_count}, "
            f"left_padding_frames={left_padding}, right_padding_frames={right_padding})",
        )
    return _STEREO_REASON_EXACT, "L/R sample rate, width, and frame count match"


def _stereo_output_frame_count(left: Waveform, right: Waveform, reason_code: str) -> int | None:
    if reason_code in {_STEREO_REASON_EXACT, _STEREO_REASON_PADDED}:
        return max(left.frame_count, right.frame_count)
    return None


def _stereo_padding_frames(left: Waveform, right: Waveform, reason_code: str) -> tuple[int, int]:
    if reason_code != _STEREO_REASON_PADDED:
        return 0, 0
    return max(0, right.frame_count - left.frame_count), max(
        0, left.frame_count - right.frame_count
    )


def _decision_row_targets(
    plan: WaveExportPlan, row: WaveformRelationship | None
) -> tuple[SampleExportTarget, ...]:
    if row is None:
        return ()
    target_key = _single_target_key(row)
    if target_key is None:
        return ()
    return _targets_for_relationship_member(plan.targets, row, target_key)


def _same_sbac_paired_sbnk_decisions(
    plan: WaveExportPlan, member_rows: dict[str, dict[str, WaveformRelationship]]
) -> tuple[StereoExportDecision, ...]:
    by_sbnk: dict[str, tuple[WaveformRelationship, SampleExportTarget, tuple[str, str]]] = {}
    for scoped_sbnk_key, rows in member_rows.items():
        if "right_row" in rows:
            continue
        left_row = rows.get("left_row")
        if left_row is None or left_row.quality != "Known":
            continue
        targets = _decision_row_targets(plan, left_row)
        if len(targets) != 1:
            continue
        target = targets[0]
        lr_name = _terminal_lr_name(target.sampler_sample_name)
        if lr_name is None:
            continue
        by_sbnk[scoped_sbnk_key] = (left_row, target, lr_name)

    grouped: dict[
        tuple[str, str, str], dict[str, tuple[WaveformRelationship, SampleExportTarget]]
    ] = defaultdict(dict)
    bases: dict[tuple[str, str, str], str] = {}
    sbac_basis: dict[tuple[str, str, str], str] = {}
    for row in plan.relationships:
        if row.quality != "Known" or row.relationship_type != "SBAC_SLOT_TO_SBNK":
            continue
        target_keys = _targets(row.target_key)
        if len(target_keys) != 1:
            continue
        source_image = _scoped_row_source(row)
        scoped_sbnk_key = scoped_object_key(source_image, target_keys[0])
        entry = by_sbnk.get(scoped_sbnk_key)
        if entry is None:
            continue
        _left_row, target, (base, side) = entry
        key = (source_image, row.source_key, base)
        if side in grouped[key]:
            grouped[key]["ambiguous"] = (row, target)
            continue
        grouped[key][side] = (row, target)
        bases[key] = base
        sbac_basis[key] = row.basis

    decisions: list[StereoExportDecision] = []
    for key, sides in sorted(grouped.items()):
        source_image, _sbac_key, base = key
        if "ambiguous" in sides or set(sides) != {"left", "right"}:
            continue
        _left_sbac_row, left_target = sides["left"]
        _right_sbac_row, right_target = sides["right"]
        if left_target.waveform.object_key == right_target.waveform.object_key:
            continue
        left_member = by_sbnk[scoped_object_key(source_image, left_target.sampler_sample_key)][0]
        right_member = by_sbnk[scoped_object_key(source_image, right_target.sampler_sample_key)][0]
        partition_name, volume_name, category_name, sample_bank_name, _sample_name = (
            _decision_context(left_target, right_target)
        )
        reason_code, reason = _stereo_interleave_reason(left_target.waveform, right_target.waveform)
        interleavable = reason_code in {_STEREO_REASON_EXACT, _STEREO_REASON_PADDED}
        output_frame_count: int | None = None
        left_padding_frames = 0
        right_padding_frames = 0
        if (
            interleavable
            and left_target.relative_wav_path.parent != right_target.relative_wav_path.parent
        ):
            decision = _STEREO_DECISION_PARENT_CONFLICT
            reason_code = _STEREO_REASON_PARENT_CONFLICT
            reason = "L/R interleavable targets have different sampler-facing export parents"
        else:
            decision = _STEREO_DECISION_SUCCESS if interleavable else _STEREO_DECISION_NOT_EXACT
            output_frame_count = _stereo_output_frame_count(
                left_target.waveform, right_target.waveform, reason_code
            )
            left_padding_frames, right_padding_frames = _stereo_padding_frames(
                left_target.waveform,
                right_target.waveform,
                reason_code,
            )
        decisions.append(
            StereoExportDecision(
                source_image=source_image,
                scoped_sbnk_key=scoped_object_key(source_image, left_target.sampler_sample_key),
                sbnk_object_key=left_target.sampler_sample_key,
                relationship_quality="Known",
                basis="+".join(
                    sorted(
                        {
                            sbac_basis[key],
                            left_member.basis,
                            right_member.basis,
                            "same-sbac-sbnk-name-lr-pair",
                        }
                    )
                ),
                partition_name=partition_name,
                volume_name=volume_name,
                category_name=category_name,
                sample_bank_name=sample_bank_name,
                sample_name=_stereo_display_name(left_target, right_target),
                left_target_id=left_target.stable_id,
                right_target_id=right_target.stable_id,
                left_waveform_name=left_target.waveform.sample_name,
                right_waveform_name=right_target.waveform.sample_name,
                left_waveform_object_key=left_target.waveform.object_key,
                right_waveform_object_key=right_target.waveform.object_key,
                decision=decision,
                reason_code=reason_code,
                reason=reason,
                left_mono_wav_path=left_target.relative_wav_path.as_posix(),
                right_mono_wav_path=right_target.relative_wav_path.as_posix(),
                left_sample_rate=left_target.waveform.sample_rate,
                right_sample_rate=right_target.waveform.sample_rate,
                left_sample_width_bytes=left_target.waveform.sample_width_bytes,
                right_sample_width_bytes=right_target.waveform.sample_width_bytes,
                left_frame_count=left_target.waveform.frame_count,
                right_frame_count=right_target.waveform.frame_count,
                output_frame_count=output_frame_count,
                left_padding_frames=left_padding_frames,
                right_padding_frames=right_padding_frames,
            )
        )
    return tuple(decisions)


def _build_stereo_decisions(plan: WaveExportPlan) -> tuple[StereoExportDecision, ...]:
    decisions: list[StereoExportDecision] = []
    member_rows = _stereo_member_rows(plan)
    for scoped_sbnk_key, rows in sorted(member_rows.items()):
        left_row = rows.get("left_row")
        right_row = rows.get("right_row")
        if left_row is None or right_row is None:
            continue
        left_targets = _decision_row_targets(plan, left_row)
        right_targets = _decision_row_targets(plan, right_row)
        left_target = left_targets[0] if left_targets else None
        right_target = right_targets[0] if right_targets else None
        source_image = ""
        sbnk_object_key = ""
        basis = ""
        for row in (left_row, right_row):
            if row is None:
                continue
            source_image = row.source_image
            sbnk_object_key = row.source_key
            basis = row.basis
            break
        if not source_image:
            for target in (left_target, right_target):
                if target is not None:
                    source_image = target.waveform.source_image
                    break
        partition_name, volume_name, category_name, sample_bank_name, sample_name = (
            _decision_context(left_target, right_target)
        )
        if left_target is None or right_target is None:
            if left_target is None and right_target is None:
                continue
            decisions.append(
                StereoExportDecision(
                    source_image=source_image,
                    scoped_sbnk_key=scoped_sbnk_key,
                    sbnk_object_key=sbnk_object_key,
                    relationship_quality="Known",
                    basis=basis,
                    partition_name=partition_name,
                    volume_name=volume_name,
                    category_name=category_name,
                    sample_bank_name=sample_bank_name,
                    sample_name=sample_name or sbnk_object_key,
                    left_target_id=left_target.stable_id if left_target else "",
                    right_target_id=right_target.stable_id if right_target else "",
                    left_waveform_name=left_target.waveform.sample_name if left_target else "",
                    right_waveform_name=right_target.waveform.sample_name if right_target else "",
                    left_waveform_object_key=left_target.waveform.object_key if left_target else "",
                    right_waveform_object_key=right_target.waveform.object_key
                    if right_target
                    else "",
                    decision=_STEREO_DECISION_MISSING_SIDE,
                    reason_code=_STEREO_REASON_MISSING_SIDE,
                    reason="Stereo companion not selected/exported",
                    left_mono_wav_path=left_target.relative_wav_path.as_posix()
                    if left_target
                    else "",
                    right_mono_wav_path=right_target.relative_wav_path.as_posix()
                    if right_target
                    else "",
                    left_sample_rate=left_target.waveform.sample_rate if left_target else None,
                    right_sample_rate=right_target.waveform.sample_rate if right_target else None,
                    left_sample_width_bytes=left_target.waveform.sample_width_bytes
                    if left_target
                    else None,
                    right_sample_width_bytes=right_target.waveform.sample_width_bytes
                    if right_target
                    else None,
                    left_frame_count=left_target.waveform.frame_count if left_target else None,
                    right_frame_count=right_target.waveform.frame_count if right_target else None,
                )
            )
            continue
        relationship_qualities = {left_row.quality, right_row.quality}
        output_frame_count: int | None = None
        left_padding_frames = 0
        right_padding_frames = 0
        if relationship_qualities != {"Known"}:
            reason_code = _STEREO_REASON_UNKNOWN
            reason = "L/R relationship is not known; mono files kept to avoid hiding candidate waveform objects"
            decision = _STEREO_DECISION_UNKNOWN
        else:
            reason_code, reason = _stereo_interleave_reason(
                left_target.waveform, right_target.waveform
            )
            interleavable = reason_code in {_STEREO_REASON_EXACT, _STEREO_REASON_PADDED}
            if (
                interleavable
                and left_target.relative_wav_path.parent != right_target.relative_wav_path.parent
            ):
                decision = _STEREO_DECISION_PARENT_CONFLICT
                reason_code = _STEREO_REASON_PARENT_CONFLICT
                reason = "L/R interleavable targets have different sampler-facing export parents"
            else:
                decision = _STEREO_DECISION_SUCCESS if interleavable else _STEREO_DECISION_NOT_EXACT
                output_frame_count = _stereo_output_frame_count(
                    left_target.waveform, right_target.waveform, reason_code
                )
                left_padding_frames, right_padding_frames = _stereo_padding_frames(
                    left_target.waveform,
                    right_target.waveform,
                    reason_code,
                )
        decisions.append(
            StereoExportDecision(
                source_image=source_image,
                scoped_sbnk_key=scoped_sbnk_key,
                sbnk_object_key=sbnk_object_key,
                relationship_quality="+".join(sorted(relationship_qualities)),
                basis=basis,
                partition_name=partition_name,
                volume_name=volume_name,
                category_name=category_name,
                sample_bank_name=sample_bank_name,
                sample_name=_stereo_display_name(left_target, right_target),
                left_target_id=left_target.stable_id,
                right_target_id=right_target.stable_id,
                left_waveform_name=left_target.waveform.sample_name,
                right_waveform_name=right_target.waveform.sample_name,
                left_waveform_object_key=left_target.waveform.object_key,
                right_waveform_object_key=right_target.waveform.object_key,
                decision=decision,
                reason_code=reason_code,
                reason=reason,
                left_mono_wav_path=left_target.relative_wav_path.as_posix(),
                right_mono_wav_path=right_target.relative_wav_path.as_posix(),
                left_sample_rate=left_target.waveform.sample_rate,
                right_sample_rate=right_target.waveform.sample_rate,
                left_sample_width_bytes=left_target.waveform.sample_width_bytes,
                right_sample_width_bytes=right_target.waveform.sample_width_bytes,
                left_frame_count=left_target.waveform.frame_count,
                right_frame_count=right_target.waveform.frame_count,
                output_frame_count=output_frame_count,
                left_padding_frames=left_padding_frames,
                right_padding_frames=right_padding_frames,
            )
        )
    decisions.extend(_same_sbac_paired_sbnk_decisions(plan, member_rows))
    return tuple(decisions)


def _stereo_compatible(left: Waveform, right: Waveform) -> bool:
    return (
        left.sample_rate == right.sample_rate
        and left.sample_width_bytes == right.sample_width_bytes
        and left.frame_count == right.frame_count
    )


def _stereo_display_name(left_target: SampleExportTarget, right_target: SampleExportTarget) -> str:
    left_sample_lr = _terminal_lr_name(left_target.sampler_sample_name)
    right_sample_lr = _terminal_lr_name(right_target.sampler_sample_name)
    if (
        left_sample_lr is not None
        and right_sample_lr is not None
        and left_sample_lr[0] == right_sample_lr[0]
        and left_sample_lr[1] == "left"
        and right_sample_lr[1] == "right"
    ):
        duplicate_base = _trailing_duplicate_base(left_sample_lr[0])
        if duplicate_base and left_target.sample_bank_name:
            return _safe_display_path_name(
                f"{left_target.sample_bank_name} - {duplicate_base}", "stereo sample"
            )
        return _safe_display_path_name(left_sample_lr[0], "stereo sample")
    for candidate in (left_target, right_target):
        duplicate_base = _trailing_duplicate_base(candidate.sampler_sample_name)
        if duplicate_base and candidate.sample_bank_name:
            return _safe_display_path_name(
                f"{candidate.sample_bank_name} - {duplicate_base}", "stereo sample"
            )
    for value in (
        left_target.sampler_sample_name,
        right_target.sampler_sample_name,
        left_target.sample_bank_name,
        right_target.sample_bank_name,
    ):
        if value.strip():
            return _safe_display_path_name(value, "stereo sample")
    left_name = left_target.waveform.sample_name.strip()
    right_name = right_target.waveform.sample_name.strip()
    if left_name and right_name:
        if (
            left_name.endswith("-L")
            and right_name.endswith("-R")
            and left_name[:-2] == right_name[:-2]
        ):
            return _safe_display_path_name(left_name[:-2], "stereo sample")
        return _safe_display_path_name(f"{left_name} + {right_name}", "stereo sample")
    return "stereo sample"


def _stereo_target_paths(
    left_target: SampleExportTarget,
    right_target: SampleExportTarget,
    used_paths: set[str],
) -> tuple[Path, Path]:
    parent = left_target.relative_wav_path.parent
    stem = _stereo_display_name(left_target, right_target)
    return _dedupe_pair(parent / f"{stem}.wav", parent / f"{stem}.json", used_paths)


def _exact_stereo_replacement_ids(
    plan: WaveExportPlan, decisions: tuple[StereoExportDecision, ...]
) -> set[str]:
    suppressed_waveforms = {
        scoped_object_key(decision.source_image, waveform_key)
        for decision in decisions
        if decision.decision == _STEREO_DECISION_SUCCESS
        for waveform_key in (decision.left_waveform_object_key, decision.right_waveform_object_key)
        if waveform_key
    }
    return {
        target.stable_id
        for target in plan.targets
        if scoped_object_key(target.waveform.source_image, target.waveform.object_key)
        in suppressed_waveforms
    }


def _target_by_id(plan: WaveExportPlan) -> dict[str, SampleExportTarget]:
    return {target.stable_id: target for target in plan.targets}


def _stereo_context_by_target_id(
    decisions: tuple[StereoExportDecision, ...],
) -> dict[str, tuple[StereoExportDecision, str]]:
    contexts: dict[str, tuple[StereoExportDecision, str]] = {}
    for decision in decisions:
        if decision.left_target_id:
            contexts[decision.left_target_id] = (decision, "left")
        if decision.right_target_id:
            contexts[decision.right_target_id] = (decision, "right")
    return contexts


def _stereo_manifest_row(
    decision: StereoExportDecision, left_target: SampleExportTarget, rel_wav: Path, rel_json: Path
) -> dict[str, object]:
    row = _sample_manifest_row(left_target)
    row.update(
        {
            "stable_id": f"{left_target.stable_id}__stereo",
            "sample_name": decision.sample_name,
            "wav_path": rel_wav.as_posix(),
            "json_path": rel_json.as_posix(),
            "channels": 2,
            "stereo_role": "interleaved_stereo",
            "left_waveform_object_key": decision.left_waveform_object_key,
            "right_waveform_object_key": decision.right_waveform_object_key,
        }
    )
    return row


def _stereo_warning(decision: StereoExportDecision) -> str:
    path_bits = [
        value
        for value in (
            decision.partition_name,
            decision.volume_name,
            f"Sample Bank {decision.sample_bank_name}" if decision.sample_bank_name else "",
            decision.sample_name,
        )
        if value
    ]
    label = " / ".join(path_bits) or decision.sample_name or decision.sbnk_object_key
    return (
        f"{label}: kept mono because {decision.reason}. Technical key: {decision.sbnk_object_key}"
    )


def _stereo_decision_row(decision: StereoExportDecision) -> dict[str, object]:
    return asdict(decision)


def _write_csv_manifest(path: Path, rows: list[dict[str, object]], *, overwrite: bool) -> None:
    if path.exists() and not overwrite:
        raise FileExistsError(f"refusing to overwrite existing file: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = list(rows[0]) if rows else []
    tmp = path.with_name(f".{path.name}.tmp")
    with tmp.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
    tmp.replace(path)


def _target_volume_root(target: SampleExportTarget) -> Path:
    return _volume_root(target.source_scope, target.placement)


def _volume_relative(path: Path, volume_root: Path) -> str:
    return path.relative_to(volume_root).as_posix()


def _dedupe_audio_path(base: Path, used_paths: set[str]) -> Path:
    candidate = base
    index = 2
    while candidate.as_posix().lower() in used_paths:
        candidate = base.with_name(f"{base.stem} ({index}){base.suffix}")
        index += 1
    used_paths.add(candidate.as_posix().lower())
    return candidate


def _sbnk_member_role(
    target: SampleExportTarget, relationships: tuple[WaveformRelationship, ...]
) -> str:
    if not target.sampler_sample_key:
        return "mono"
    for row in relationships:
        if (
            row.source_image == target.waveform.source_image
            and row.source_key == target.sampler_sample_key
            and _single_target_key(row) == target.waveform.object_key
        ):
            if row.relationship_type == "SBNK_LEFT_MEMBER_TO_SMPL":
                return "left"
            if row.relationship_type == "SBNK_RIGHT_MEMBER_TO_SMPL":
                return "right"
    return "mono"


def _write_physical_smpl_wavs(
    plan: WaveExportPlan,
    *,
    overwrite: bool,
    progress_start: Callable[[str], None] | None = None,
    progress_done: Callable[[str], None] | None = None,
) -> tuple[dict[str, str], list[Path]]:
    used_by_volume: dict[str, set[str]] = defaultdict(set)
    refs: dict[str, str] = {}
    written: list[Path] = []
    for target in sorted(
        plan.targets,
        key=lambda item: (
            _target_volume_root(item).as_posix().lower(),
            item.waveform.source_image,
            item.waveform.object_key,
            item.waveform.sample_name.lower(),
        ),
    ):
        volume_root = _target_volume_root(target)
        key = f"{volume_root.as_posix()}|{_graph_source_ref(target.waveform.source_image, target.waveform.object_key)}"
        if key in refs:
            continue
        stem = _safe_display_path_name(target.waveform.sample_name, "waveform")
        rel_path = _dedupe_audio_path(
            volume_root / "SMPL" / f"{stem}.wav", used_by_volume[volume_root.as_posix()]
        )
        if progress_start is not None:
            progress_start(f"writing {rel_path.as_posix()}")
        _atomic_write_bytes(
            plan.output_dir / rel_path, _wav_bytes(target.waveform), overwrite=overwrite
        )
        refs[key] = _volume_relative(rel_path, volume_root)
        written.append(plan.output_dir / rel_path)
        if progress_done is not None:
            progress_done(f"wrote {rel_path.as_posix()}")
    return refs, written


def _rendered_stereo_audio_key(
    left_target: SampleExportTarget, right_target: SampleExportTarget
) -> tuple[str, str, str]:
    volume_root = _target_volume_root(left_target)
    return (
        volume_root.as_posix().lower(),
        scoped_object_key(left_target.waveform.source_image, left_target.waveform.object_key),
        scoped_object_key(right_target.waveform.source_image, right_target.waveform.object_key),
    )


def _write_rendered_stereo_wavs(
    plan: WaveExportPlan,
    decisions: tuple[StereoExportDecision, ...],
    *,
    overwrite: bool,
    progress_start: Callable[[str], None] | None = None,
    progress_done: Callable[[str], None] | None = None,
) -> tuple[dict[str, dict[str, object]], tuple[StereoExportDecision, ...], list[Path], list[str]]:
    by_id = _target_by_id(plan)
    used_by_volume: dict[str, set[str]] = defaultdict(set)
    rendered_path_by_audio: dict[tuple[str, str, str], Path] = {}
    rendered_by_sbnk: dict[str, dict[str, object]] = {}
    updated: list[StereoExportDecision] = []
    written: list[Path] = []
    warnings: list[str] = []
    for decision in decisions:
        left_target = by_id.get(decision.left_target_id)
        right_target = by_id.get(decision.right_target_id)
        if decision.decision != _STEREO_DECISION_SUCCESS:
            if decision.decision in {
                _STEREO_DECISION_NOT_EXACT,
                _STEREO_DECISION_MISSING_SIDE,
                _STEREO_DECISION_PARENT_CONFLICT,
                _STEREO_DECISION_UNKNOWN,
            }:
                warnings.append(_stereo_warning(decision))
            updated.append(decision)
            continue
        if left_target is None or right_target is None:
            missing = replace(
                decision,
                decision=_STEREO_DECISION_MISSING_SIDE,
                reason_code=_STEREO_REASON_MISSING_SIDE,
                reason="Stereo decision lost one decoded target before writing",
            )
            warnings.append(_stereo_warning(missing))
            updated.append(missing)
            continue
        volume_root = _target_volume_root(left_target)
        stem = _stereo_display_name(left_target, right_target)
        audio_key = _rendered_stereo_audio_key(left_target, right_target)
        rel_path = rendered_path_by_audio.get(audio_key)
        output_frames = decision.output_frame_count or max(
            left_target.waveform.frame_count, right_target.waveform.frame_count
        )
        left_padding = decision.left_padding_frames
        right_padding = decision.right_padding_frames
        if rel_path is None:
            rel_path = _dedupe_audio_path(
                volume_root / "RENDERED" / f"{stem}.wav", used_by_volume[volume_root.as_posix()]
            )
            rendered_path_by_audio[audio_key] = rel_path
            if progress_start is not None:
                progress_start(f"writing {rel_path.as_posix()}")
            wav_bytes, output_frames, left_padding, right_padding = _stereo_wav_bytes(
                left_target.waveform,
                right_target.waveform,
            )
            _atomic_write_bytes(plan.output_dir / rel_path, wav_bytes, overwrite=overwrite)
            written.append(plan.output_dir / rel_path)
            if progress_done is not None:
                progress_done(f"wrote {rel_path.as_posix()}")
        volume_rel = _volume_relative(rel_path, volume_root)
        rendered_id = _graph_object_id("RENDERED", decision.source_image, decision.sbnk_object_key)
        rendered = {
            "id": rendered_id,
            "kind": "interleaved_stereo",
            "wav_path": volume_rel,
            "source_sbnk_id": _graph_object_id(
                "SBNK", decision.source_image, decision.sbnk_object_key
            ),
            "source_sbnk_ref": _graph_source_ref(decision.source_image, decision.sbnk_object_key),
            "channels": 2,
            "sample_rate": left_target.waveform.sample_rate,
            "sample_width_bytes": left_target.waveform.sample_width_bytes,
            "frames": output_frames,
            "exactness_status": "padded-interleaved-stereo"
            if decision.reason_code == _STEREO_REASON_PADDED
            else "exact-interleaved-stereo",
            "decision": decision.decision,
            "reason_code": decision.reason_code,
            "reason": decision.reason,
            "relationship_quality": decision.relationship_quality,
            "basis": decision.basis,
            "left_smpl_id": _graph_object_id(
                "SMPL", left_target.waveform.source_image, left_target.waveform.object_key
            ),
            "right_smpl_id": _graph_object_id(
                "SMPL", right_target.waveform.source_image, right_target.waveform.object_key
            ),
            "padding": {
                "left_frames": left_padding,
                "right_frames": right_padding,
            },
        }
        rendered_by_sbnk[scoped_object_key(decision.source_image, decision.sbnk_object_key)] = (
            rendered
        )
        next_decision = replace(
            decision,
            stereo_wav_path=rel_path.as_posix(),
            stereo_json_path="",
            left_mono_wav_path="",
            right_mono_wav_path="",
            output_frame_count=output_frames,
            left_padding_frames=left_padding,
            right_padding_frames=right_padding,
        )
        updated.append(next_decision)
    return rendered_by_sbnk, tuple(updated), written, warnings


def _relationship_graph_row(
    row: WaveformRelationship, ref_to_id: dict[str, str]
) -> dict[str, object]:
    targets = [_graph_source_ref(row.source_image, target) for target in _targets(row.target_key)]
    return {
        "id": _graph_relationship_id(row),
        "relationship_type": row.relationship_type,
        "source_ref": _graph_source_ref(row.source_image, row.source_key),
        "source_id": ref_to_id.get(_graph_source_ref(row.source_image, row.source_key), ""),
        "target_refs": targets,
        "target_ids": [ref_to_id.get(target, "") for target in targets],
        "quality": row.quality,
        "basis": row.basis,
        "raw_fields": row.raw_fields,
        "ambiguity_notes": row.ambiguity_notes,
        "source_image": row.source_image,
        "scope_key": row.scope_key,
        "assignment_index": row.assignment_index,
        "assignment_name": row.assignment_name,
        "assignment_row_state": row.assignment_row_state,
        "active_assignment_state": row.active_assignment_state,
        "assignment_rch_assign_display": row.assignment_rch_assign_display,
    }


def _build_volume_graph(
    plan: WaveExportPlan,
    volume_root: Path,
    targets: tuple[SampleExportTarget, ...],
    physical_refs: dict[str, str],
    rendered_by_sbnk: dict[str, dict[str, object]],
    stereo_decisions: tuple[StereoExportDecision, ...],
) -> dict[str, object]:
    ref_to_id: dict[str, str] = {}
    smpl_objects: dict[str, dict[str, object]] = {}
    sbnk_objects: dict[str, dict[str, object]] = {}
    sbac_objects: dict[str, dict[str, object]] = {}
    prog_objects: dict[str, dict[str, object]] = {}
    relationship_rows: dict[str, WaveformRelationship] = {}

    def remember_relationships(
        rows: tuple[WaveformRelationship, ...], fallback_source: str
    ) -> None:
        for row in rows:
            normalized = row if row.source_image else replace(row, source_image=fallback_source)
            relationship_rows[_graph_relationship_id(normalized)] = normalized

    for target in targets:
        waveform = target.waveform
        smpl_ref = _graph_source_ref(waveform.source_image, waveform.object_key)
        smpl_id = _graph_object_id("SMPL", waveform.source_image, waveform.object_key)
        ref_to_id[smpl_ref] = smpl_id
        physical_key = f"{volume_root.as_posix()}|{smpl_ref}"
        smpl_objects.setdefault(
            smpl_id,
            {
                "id": smpl_id,
                "source_ref": smpl_ref,
                "object_key": waveform.object_key,
                "display_name": waveform.sample_name,
                "wav_path": physical_refs[physical_key],
                "audio": {
                    "channels": waveform.channel_count,
                    "sample_rate": waveform.sample_rate,
                    "sample_width_bytes": waveform.sample_width_bytes,
                    "frames": waveform.frame_count,
                    "stored_payload_size": waveform.stored_payload_size,
                    "decoded_pcm_size": len(waveform.pcm),
                    "stored_payload_transform": waveform.stored_payload_transform,
                    "exactness_status": waveform.exactness_status,
                },
                "playback": {
                    "root_key_midi": waveform.root_key,
                    "root_key_name": note_name(waveform.root_key),
                    "fine_tune_cents": waveform.fine_tune,
                    "loop_mode_raw": waveform.loop_mode,
                    "loop_mode_label": waveform.loop_mode_label,
                    "loop_start_frame": waveform.loop_start,
                    "loop_length_frames": waveform.loop_length,
                    "loop_end_frame_a4000_ui": waveform.loop_end_a4000_ui,
                },
                "origin": {
                    "source_container": waveform.source_image,
                    "container_kind": waveform.container_kind,
                    "partition_index": waveform.partition_index,
                    "object_offset": waveform.object_offset,
                    "quality": waveform.quality.quality.value,
                    "basis": waveform.quality.source,
                    "quality_notes": waveform.quality.notes,
                    "alternating_byte_payload_detected": waveform.alternating_byte_payload_detected,
                    "metadata": waveform.metadata,
                },
            },
        )
        parents = _target_parent_rows(
            target, _parents(plan, waveform.source_image, waveform.object_key)
        )
        ancestors = _target_ancestor_rows(
            target, _ancestors(plan, waveform.source_image, waveform.object_key)
        )
        remember_relationships(parents, waveform.source_image)
        remember_relationships(ancestors, waveform.source_image)
        if target.sampler_sample_key:
            sbnk_ref = _graph_source_ref(waveform.source_image, target.sampler_sample_key)
            sbnk_id = _graph_object_id("SBNK", waveform.source_image, target.sampler_sample_key)
            ref_to_id[sbnk_ref] = sbnk_id
            sbnk = sbnk_objects.setdefault(
                sbnk_id,
                {
                    "id": sbnk_id,
                    "source_ref": sbnk_ref,
                    "object_key": target.sampler_sample_key,
                    "display_name": target.sampler_sample_name or target.sampler_sample_key,
                    "sample_bank_id": "",
                    "physical_waveforms": [],
                    "rendered_audio": None,
                    "parameters": {
                        "decoded_current_sbnk_member_parameters": waveform.metadata.get(
                            "decoded_current_sbnk_member_parameters",
                            [],
                        )
                    },
                },
            )
            waveform_entry: dict[str, object] = {
                "role": _sbnk_member_role(target, plan.relationships),
                "smpl_id": smpl_id,
                "smpl_ref": smpl_ref,
                "wav_path": physical_refs[physical_key],
                "relationship_quality": _relationship_quality_summary(parents),
            }
            physical_waveforms = cast(list[dict[str, object]], sbnk["physical_waveforms"])
            if waveform_entry not in physical_waveforms:
                physical_waveforms.append(waveform_entry)
            rendered = rendered_by_sbnk.get(sbnk_ref)
            if rendered:
                sbnk["rendered_audio"] = rendered
            if target.sample_bank_key:
                sbac_ref = _graph_source_ref(waveform.source_image, target.sample_bank_key)
                sbac_id = _graph_object_id("SBAC", waveform.source_image, target.sample_bank_key)
                ref_to_id[sbac_ref] = sbac_id
                sbnk["sample_bank_id"] = sbac_id
                sbac = sbac_objects.setdefault(
                    sbac_id,
                    {
                        "id": sbac_id,
                        "source_ref": sbac_ref,
                        "object_key": target.sample_bank_key,
                        "display_name": target.sample_bank_name or target.sample_bank_key,
                        "members": [],
                    },
                )
                member: dict[str, object] = {"sbnk_id": sbnk_id, "sbnk_ref": sbnk_ref}
                members = cast(list[dict[str, object]], sbac["members"])
                if member not in members:
                    members.append(member)
        for row in ancestors:
            if row.relationship_type.startswith("PROG_"):
                prog_ref = _graph_source_ref(row.source_image, row.source_key)
                prog_id = _graph_object_id("PROG", row.source_image, row.source_key)
                ref_to_id[prog_ref] = prog_id
                prog = prog_objects.setdefault(
                    prog_id,
                    {
                        "id": prog_id,
                        "source_ref": prog_ref,
                        "object_key": row.source_key,
                        "display_name": row.source_key,
                        "assignments": [],
                    },
                )
                assignment: dict[str, object] = {
                    "relationship_id": _graph_relationship_id(row),
                    "relationship_type": row.relationship_type,
                    "target_refs": [
                        _graph_source_ref(row.source_image, value)
                        for value in _targets(row.target_key)
                    ],
                    "quality": row.quality,
                    "assignment_index": row.assignment_index,
                    "assignment_name": row.assignment_name,
                    "assignment_row_state": row.assignment_row_state,
                    "active_assignment_state": row.active_assignment_state,
                    "assignment_rch_assign_display": row.assignment_rch_assign_display,
                }
                assignments = cast(list[dict[str, object]], prog["assignments"])
                if assignment not in assignments:
                    assignments.append(assignment)

    relationship_entries = [
        _relationship_graph_row(row, ref_to_id)
        for row in sorted(
            relationship_rows.values(),
            key=lambda item: (
                item.source_image,
                item.scope_key,
                item.source_key,
                item.relationship_type,
                item.target_key,
            ),
        )
    ]
    first_target = targets[0]
    placement = first_target.placement
    decisions = [
        asdict(decision)
        for decision in stereo_decisions
        if decision.source_image in {target.waveform.source_image for target in targets}
        and (
            _graph_source_ref(decision.source_image, decision.left_waveform_object_key) in ref_to_id
            or _graph_source_ref(decision.source_image, decision.right_waveform_object_key)
            in ref_to_id
            or _graph_source_ref(decision.source_image, decision.sbnk_object_key) in ref_to_id
        )
    ]
    return {
        "schema": "axklib.volume_graph.v1",
        "source": {
            "containers": sorted({target.waveform.source_image for target in targets}),
            "container_kinds": sorted({target.waveform.container_kind for target in targets}),
            "source_scope": first_target.source_scope,
        },
        "volume": {
            "path": volume_root.as_posix(),
            "partition_index": placement.partition_index if placement else None,
            "partition_name": placement.partition_name if placement else "",
            "name": placement.volume_name if placement else "_unplaced",
            "placement_quality": first_target.placement_quality.value,
            "placement_source": first_target.placement_source,
        },
        "objects": {
            "smpl": list(sorted(smpl_objects.values(), key=lambda row: str(row["id"]))),
            "sbnk": list(sorted(sbnk_objects.values(), key=lambda row: str(row["id"]))),
            "sbac": list(sorted(sbac_objects.values(), key=lambda row: str(row["id"]))),
            "prog": list(sorted(prog_objects.values(), key=lambda row: str(row["id"]))),
            "sequ": [],
        },
        "relationships": relationship_entries,
        "rendered_audio": list(
            sorted(
                {
                    str(row["id"]): row
                    for row in rendered_by_sbnk.values()
                    if str(row.get("source_sbnk_ref", "")) in ref_to_id
                }.values(),
                key=lambda row: str(row["id"]),
            )
        ),
        "stereo_decisions": decisions,
        "unresolved": [
            {
                "stable_id": target.stable_id,
                "object_key": target.waveform.object_key,
                "display_name": target.sampler_sample_name or target.waveform.sample_name,
                "reason": "no known volume/category placement",
                "placement_quality": target.placement_quality.value,
                "placement_source": target.placement_source,
            }
            for target in targets
            if target.placement_quality != DataQuality.KNOWN
        ],
    }


def _write_stereo_exports(
    plan: WaveExportPlan,
    *,
    overwrite: bool,
    mono_replacement_ids: set[str],
    decisions: tuple[StereoExportDecision, ...],
) -> tuple[
    list[Path],
    list[str],
    list[dict[str, object]],
    list[dict[str, object]],
    tuple[StereoExportDecision, ...],
]:
    written: list[Path] = []
    warnings: list[str] = []
    rows: list[dict[str, object]] = []
    manifest_rows: list[dict[str, object]] = []
    updated_decisions: list[StereoExportDecision] = []
    by_id = _target_by_id(plan)
    used_paths = {
        str(path).lower()
        for target in plan.targets
        if target.stable_id not in mono_replacement_ids
        for path in (target.relative_wav_path, target.relative_json_path)
    }
    for decision in decisions:
        left_target = by_id.get(decision.left_target_id)
        right_target = by_id.get(decision.right_target_id)
        if decision.decision != _STEREO_DECISION_SUCCESS:
            if decision.decision in {
                _STEREO_DECISION_NOT_EXACT,
                _STEREO_DECISION_MISSING_SIDE,
                _STEREO_DECISION_PARENT_CONFLICT,
                _STEREO_DECISION_UNKNOWN,
            }:
                warnings.append(_stereo_warning(decision))
            updated_decisions.append(decision)
            continue
        if left_target is None or right_target is None:
            missing = replace(
                decision,
                decision=_STEREO_DECISION_MISSING_SIDE,
                reason_code=_STEREO_REASON_MISSING_SIDE,
                reason="Stereo decision lost one decoded target before writing",
            )
            warnings.append(_stereo_warning(missing))
            updated_decisions.append(missing)
            continue
        rel_wav, rel_json = _stereo_target_paths(left_target, right_target, used_paths)
        wav_path = plan.output_dir / rel_wav
        json_path = plan.output_dir / rel_json
        stereo_target = replace(left_target, relative_wav_path=rel_wav, relative_json_path=rel_json)
        sidecar = sidecar_v2(stereo_target, wav_path, plan)
        audio = cast(dict[str, object], sidecar["audio"])
        exactness_status = (
            "padded-interleaved-stereo"
            if decision.reason_code == _STEREO_REASON_PADDED
            else "exact-interleaved-stereo"
        )
        output_frames = decision.output_frame_count or max(
            left_target.waveform.frame_count,
            right_target.waveform.frame_count,
        )
        audio.update(
            {
                "channels": 2,
                "frames": output_frames,
                "duration_seconds": output_frames / left_target.waveform.sample_rate
                if left_target.waveform.sample_rate
                else 0.0,
                "exactness_status": exactness_status,
                "decoded_pcm_size": output_frames * left_target.waveform.sample_width_bytes * 2,
                "left_input_frames": left_target.waveform.frame_count,
                "right_input_frames": right_target.waveform.frame_count,
                "left_padding_frames": decision.left_padding_frames,
                "right_padding_frames": decision.right_padding_frames,
            }
        )
        relationships = cast(dict[str, object], sidecar["relationships"])
        stereo_parent_banks = [
            {
                "object_key": decision.sbnk_object_key,
                "relationship_type": "SBNK_LEFT_MEMBER_TO_SMPL",
                "source_image": decision.source_image,
            },
            {
                "object_key": decision.sbnk_object_key,
                "relationship_type": "SBNK_RIGHT_MEMBER_TO_SMPL",
                "source_image": decision.source_image,
            },
        ]
        relationships["parent_sample_banks"] = stereo_parent_banks
        parameters = cast(dict[str, object], sidecar["parameters"])
        parameters["sample_bank_membership"] = stereo_parent_banks
        conversion = cast(dict[str, object], sidecar["conversion"])
        conversion["sample_bank_membership"] = stereo_parent_banks
        relationships.update(
            {
                "stereo_role": "interleaved_stereo",
                "linked_left_sample": decision.left_waveform_object_key,
                "linked_right_sample": decision.right_waveform_object_key,
                "stereo_decision": decision.decision,
                "stereo_reason_code": decision.reason_code,
                "stereo_reason": decision.reason,
                "stereo_relationship_quality": decision.relationship_quality,
                "stereo_basis": decision.basis,
                "left_padding_frames": decision.left_padding_frames,
                "right_padding_frames": decision.right_padding_frames,
            }
        )
        conversion["warnings"] = (
            [decision.reason] if decision.reason_code == _STEREO_REASON_PADDED else []
        )
        wav_bytes, actual_output_frames, actual_left_padding, actual_right_padding = (
            _stereo_wav_bytes(
                left_target.waveform,
                right_target.waveform,
            )
        )
        _atomic_write_bytes(wav_path, wav_bytes, overwrite=overwrite)
        try:
            _atomic_write_text(
                json_path, json.dumps(to_plain(sidecar), indent=2) + "\n", overwrite=overwrite
            )
        except Exception:
            try:
                wav_path.unlink()
            except FileNotFoundError:
                pass
            raise
        updated = replace(
            decision,
            stereo_wav_path=rel_wav.as_posix(),
            stereo_json_path=rel_json.as_posix(),
            left_mono_wav_path="",
            right_mono_wav_path="",
            output_frame_count=actual_output_frames,
            left_padding_frames=actual_left_padding,
            right_padding_frames=actual_right_padding,
        )
        written.extend([wav_path, json_path])
        rows.append(sidecar)
        manifest_rows.append(_stereo_manifest_row(updated, left_target, rel_wav, rel_json))
        updated_decisions.append(updated)
    return written, warnings, rows, manifest_rows, tuple(updated_decisions)


def _structured_export_progress_total(
    plan: WaveExportPlan,
    stereo_decisions: tuple[StereoExportDecision, ...],
    stereo_policy: StereoPolicy,
) -> int:
    physical_keys = {
        f"{_target_volume_root(target).as_posix()}|{_graph_source_ref(target.waveform.source_image, target.waveform.object_key)}"
        for target in plan.targets
    }
    rendered_keys: set[tuple[str, str, str]] = set()
    if stereo_policy == "auto":
        by_id = _target_by_id(plan)
        for decision in stereo_decisions:
            if decision.decision != _STEREO_DECISION_SUCCESS:
                continue
            left_target = by_id.get(decision.left_target_id)
            right_target = by_id.get(decision.right_target_id)
            if left_target is None or right_target is None:
                continue
            rendered_keys.add(_rendered_stereo_audio_key(left_target, right_target))
    graph_count = len({_target_volume_root(target).as_posix() for target in plan.targets})
    return len(physical_keys) + len(rendered_keys) + graph_count


def write_structured_export(
    plan: WaveExportPlan,
    *,
    overwrite_policy: OverwritePolicy,
    stereo_policy: StereoPolicy = "auto",
    progress_callback: Callable[[WavExportProgress], None] | None = None,
) -> tuple[tuple[Path, ...], tuple[Path, ...], tuple[str, ...], tuple[dict[str, object], ...]]:
    overwrite = overwrite_policy == "replace"
    skipped: list[Path] = []
    warnings: list[str] = []
    records: list[dict[str, object]] = []
    written: list[Path] = []

    stereo_decisions = _build_stereo_decisions(plan) if stereo_policy == "auto" else ()
    progress_total = _structured_export_progress_total(plan, stereo_decisions, stereo_policy)
    progress_completed = 0

    def report_progress(message: str) -> None:
        if progress_callback is not None:
            progress_callback(
                WavExportProgress(
                    stage="exporting",
                    completed=progress_completed,
                    total=progress_total,
                    message=message,
                )
            )

    def advance_progress(message: str) -> None:
        nonlocal progress_completed
        progress_completed += 1
        if progress_callback is not None:
            progress_callback(
                WavExportProgress(
                    stage="exporting",
                    completed=progress_completed,
                    total=progress_total,
                    message=message,
                )
            )

    if progress_callback is not None:
        progress_callback(
            WavExportProgress(
                stage="exporting",
                completed=0,
                total=progress_total,
                message="building export plan",
            )
        )
    physical_refs, physical_written = _write_physical_smpl_wavs(
        plan,
        overwrite=overwrite,
        progress_start=report_progress,
        progress_done=advance_progress,
    )
    written.extend(physical_written)

    rendered_by_sbnk: dict[str, dict[str, object]] = {}
    final_stereo_decisions: tuple[StereoExportDecision, ...] = stereo_decisions
    if stereo_policy == "auto":
        rendered_by_sbnk, final_stereo_decisions, rendered_written, stereo_warnings = (
            _write_rendered_stereo_wavs(
                plan,
                stereo_decisions,
                overwrite=overwrite,
                progress_start=report_progress,
                progress_done=advance_progress,
            )
        )
        written.extend(rendered_written)
        warnings.extend(stereo_warnings)

    groups: dict[str, list[SampleExportTarget]] = defaultdict(list)
    roots: dict[str, Path] = {}
    for target in plan.targets:
        root = _target_volume_root(target)
        key = root.as_posix()
        roots[key] = root
        groups[key].append(target)

    for key in sorted(groups):
        root = roots[key]
        graph_rel_path = root / "volume.axklib.json"
        report_progress(f"building {graph_rel_path.as_posix()}")
        graph = _build_volume_graph(
            plan,
            root,
            tuple(groups[key]),
            physical_refs,
            rendered_by_sbnk,
            final_stereo_decisions,
        )
        graph_path = plan.output_dir / graph_rel_path
        write_manifest(graph_path, graph, overwrite=overwrite)
        written.append(graph_path)
        records.append(graph)
        advance_progress(f"wrote {graph_rel_path.as_posix()}")

    return tuple(written), tuple(skipped), tuple(warnings), tuple(records)


def _sample_manifest_row(target: SampleExportTarget) -> dict[str, object]:
    placement = target.placement
    sample_name = target.sampler_sample_name or target.waveform.sample_name
    sample_key = target.sampler_sample_key or target.waveform.object_key
    return {
        "stable_id": target.stable_id,
        "sample_name": sample_name,
        "sample_object_key": sample_key,
        "sample_bank_name": target.sample_bank_name,
        "sample_bank_object_key": target.sample_bank_key,
        "waveform_name": target.waveform.sample_name,
        "waveform_object_key": target.waveform.object_key,
        "source_image": target.waveform.source_image,
        "scoped_sample_key": scoped_object_key(target.waveform.source_image, sample_key),
        "scoped_waveform_key": scoped_object_key(
            target.waveform.source_image, target.waveform.object_key
        ),
        "wav_path": target.relative_wav_path.as_posix(),
        "json_path": target.relative_json_path.as_posix(),
        "sample_rate": target.waveform.sample_rate,
        "frames": target.waveform.frame_count,
        "partition_index": placement.partition_index if placement else "",
        "partition_name": placement.partition_name if placement else "",
        "volume_name": placement.volume_name if placement else "",
        "category_name": target.export_category,
        "source_category": placement.category_name if placement else "",
        "placement_quality": target.placement_quality.value,
        "placement_source": target.placement_source,
    }


def _write_manifests(
    plan: WaveExportPlan,
    sample_manifest: list[dict[str, object]],
    unresolved: list[dict[str, object]],
    *,
    stereo_decisions: tuple[StereoExportDecision, ...] = (),
    overwrite: bool,
) -> list[Path]:
    written: list[Path] = []
    write_manifest(plan.output_dir / "samples.json", sample_manifest, overwrite=overwrite)
    written.append(plan.output_dir / "samples.json")
    if plan.relationships:
        relationship_rows = [_relationship_row(row) for row in plan.relationships]
        write_manifest(
            plan.output_dir / "relationships.json", relationship_rows, overwrite=overwrite
        )
        written.append(plan.output_dir / "relationships.json")
        for filename, rows in _relationship_manifests(plan.relationships).items():
            if rows:
                write_manifest(plan.output_dir / filename, rows, overwrite=overwrite)
                written.append(plan.output_dir / filename)
    if unresolved:
        write_manifest(plan.output_dir / "unresolved.json", unresolved, overwrite=overwrite)
        written.append(plan.output_dir / "unresolved.json")
    stereo_counts = Counter(decision.decision for decision in stereo_decisions)
    summary = {
        "schema": "axklib.wave_export_summary.v1",
        "sample_export_count": len(sample_manifest),
        "sidecar_count": len(sample_manifest),
        "relationship_count": len(plan.relationships),
        "quality_counts": dict(Counter(row["placement_quality"] for row in sample_manifest)),
        "stereo_decisions_total": len(stereo_decisions),
        "stereo_interleaved_written": stereo_counts.get(_STEREO_DECISION_SUCCESS, 0),
        "stereo_kept_physical_only_not_exact_representable": stereo_counts.get(
            _STEREO_DECISION_NOT_EXACT, 0
        ),
        "stereo_kept_physical_only_missing_side": stereo_counts.get(
            _STEREO_DECISION_MISSING_SIDE, 0
        ),
        "stereo_kept_physical_only_placement_conflict": stereo_counts.get(
            _STEREO_DECISION_PARENT_CONFLICT, 0
        ),
        "stereo_kept_physical_only_unknown_relationship": stereo_counts.get(
            _STEREO_DECISION_UNKNOWN, 0
        ),
    }
    write_manifest(plan.output_dir / "export_summary.json", summary, overwrite=overwrite)
    written.append(plan.output_dir / "export_summary.json")
    return written


def _relationship_manifests(
    relationships: tuple[WaveformRelationship, ...],
) -> dict[str, list[dict[str, object]]]:
    sample_banks: dict[str, list[dict[str, object]]] = defaultdict(list)
    sbacs: dict[str, list[dict[str, object]]] = defaultdict(list)
    programs: dict[str, list[dict[str, object]]] = defaultdict(list)
    source_by_group: dict[str, tuple[str, str]] = {}
    for row in relationships:
        entry = _relationship_row(row)
        group_key = (
            scoped_object_key(row.source_image, row.source_key)
            if row.source_image
            else row.source_key
        )
        source_by_group[group_key] = (row.source_image, row.source_key)
        if row.relationship_type.startswith("SBNK_"):
            sample_banks[group_key].append(entry)
        elif row.relationship_type.startswith("SBAC_"):
            sbacs[group_key].append(entry)
        elif row.relationship_type.startswith("PROG_"):
            programs[group_key].append(entry)

    def rows_for(groups: dict[str, list[dict[str, object]]]) -> list[dict[str, object]]:
        result: list[dict[str, object]] = []
        for group_key, rows in sorted(groups.items()):
            source_image, object_key = source_by_group[group_key]
            result.append(
                {
                    "scoped_object_key": group_key,
                    "source_image": source_image,
                    "object_key": object_key,
                    "relationships": rows,
                }
            )
        return result

    return {
        "sample_banks.json": rows_for(sample_banks),
        "sample_bank_accessories.json": rows_for(sbacs),
        "programs.json": rows_for(programs),
    }


def write_manifest(path: Path, payload: object, *, overwrite: bool) -> None:
    if path.exists() and not overwrite:
        raise FileExistsError(f"refusing to overwrite existing file: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f".{path.name}.tmp")
    tmp.write_text(json.dumps(to_plain(payload), indent=2) + "\n", encoding="utf-8")
    tmp.replace(path)
