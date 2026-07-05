"""Waveform decode and WAV export services for axklib objects."""

from __future__ import annotations

import json
import os
import wave
from dataclasses import dataclass, field
from pathlib import Path
from typing import Literal, cast

from axklib.containers import AxklibContainer
from axklib.model import AxklibObject, AxklibQuality, DataQuality
from axklib.objects import current as object_current
from axklib.reports import to_plain

StereoPolicy = Literal["none", "auto"]
OverwritePolicy = Literal["fail", "replace"]
WaveExportLayout = Literal["structured", "flat"]


@dataclass(frozen=True)
class Waveform:
    """Decoded exact waveform payload and playback metadata for one SMPL object.

    Use this as the in-memory audio unit before writing WAV files or building structured export graphs."""

    object_key: str
    source_image: str
    partition_index: int | None
    object_offset: int | None
    container_kind: str
    object_type: str
    sample_name: str
    pcm: bytes
    channel_count: int
    sample_width_bytes: int
    stored_sample_width_bytes: int
    sample_rate: int
    frame_count: int
    stored_payload_size: int
    stored_payload_transform: str
    marker_lane_payload_detected: bool
    root_key: int | None
    fine_tune: int | None
    loop_mode: int | None
    loop_mode_label: str
    loop_start: int | None
    loop_length: int | None
    loop_end_a4000_ui: int | None
    exactness_status: str
    quality: AxklibQuality
    field_quality: dict[str, dict[str, str]] = field(default_factory=dict)
    metadata: dict[str, object] = field(default_factory=dict)


@dataclass(frozen=True)
class WaveformPlacement:
    """Best-known sampler-facing placement for a decoded waveform.

    Use it to organize exports by partition, volume, category, and owner while keeping placement quality explicit."""

    partition_index: int | None = None
    partition_name: str = ""
    volume_name: str = ""
    category_name: str = "Samples"
    display_name: str = ""
    quality: DataQuality = DataQuality.UNKNOWN
    source: str = ""
    relationship_path: str = ""
    owner_object_key: str = ""


@dataclass(frozen=True)
class WaveformRelationship:
    """Relationship edge relevant to waveform export context.

    Use it to connect exported audio back to parent SBNK, SBAC, or PROG objects without re-running relationship inference inside the exporter."""

    source_key: str
    target_key: str
    relationship_type: str
    quality: str
    basis: str
    raw_fields: str = ""
    ambiguity_notes: str = ""
    source_image: str = ""
    scope_key: str = ""
    assignment_index: int | None = None
    assignment_name: str = ""
    assignment_row_state: str = ""
    active_assignment_state: str = ""
    assignment_rch_assign_display: str = ""


@dataclass(frozen=True)
class WaveformIssue:
    """Structured waveform decode issue for one object.

    Use it when a waveform cannot be decoded exactly, has truncated payload bytes, or needs a stable warning/error code in corpus reports."""

    object_key: str
    sample_name: str
    code: str
    severity: str
    message: str


@dataclass(frozen=True)
class WaveformSet:
    """Batch waveform decode result.

    Use it to pass decoded waveforms and their non-fatal decode issues together through export and validation code."""

    waveforms: tuple[Waveform, ...]
    issues: tuple[WaveformIssue, ...]


@dataclass(frozen=True)
class WavExportRequest:
    """Immutable request for writing decoded waveforms to disk.

    Use it to specify output directory, stereo policy, overwrite policy, layout, placement hints, and relationship hints in one testable value."""

    output_dir: Path
    waveforms: tuple[Waveform, ...]
    stereo_policy: StereoPolicy = "auto"
    overwrite_policy: OverwritePolicy = "fail"
    sidecar_policy: Literal["json"] = "json"
    layout: WaveExportLayout = "structured"
    placements: dict[str, WaveformPlacement] = field(default_factory=dict)
    relationships: tuple[WaveformRelationship, ...] = ()


@dataclass(frozen=True)
class WavExportResult:
    """Result summary from a WAV export run.

    Use it to inspect written files, skipped files, warnings, and sidecar rows without scraping console output."""

    written_files: tuple[Path, ...]
    skipped_files: tuple[Path, ...]
    warnings: tuple[str, ...]
    sidecar_records: tuple[dict[str, object], ...]


def _be16(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 2], "big")


def _be32(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], "big")


def _safe_name(value: str) -> str:
    return object_current.safe_name(value)


def _atomic_write_bytes(path: Path, data: bytes, *, overwrite: bool) -> None:
    if path.exists() and not overwrite:
        raise FileExistsError(f"refusing to overwrite existing file: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f".{path.name}.tmp")
    tmp.write_bytes(data)
    os.replace(tmp, path)


def _atomic_write_text(path: Path, text: str, *, overwrite: bool) -> None:
    if path.exists() and not overwrite:
        raise FileExistsError(f"refusing to overwrite existing file: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f".{path.name}.tmp")
    tmp.write_text(text, encoding="utf-8")
    os.replace(tmp, path)


def _wav_bytes(waveform: Waveform) -> bytes:
    import io

    handle = io.BytesIO()
    with wave.open(handle, "wb") as wav:
        wav.setnchannels(waveform.channel_count)
        wav.setsampwidth(waveform.sample_width_bytes)
        wav.setframerate(waveform.sample_rate)
        wav.writeframes(waveform.pcm)
    return handle.getvalue()


def decode_waveform(sample: AxklibObject) -> Waveform:
    if sample.type != "SMPL":
        raise ValueError(f"decode_waveform requires SMPL object, got {sample.type}")
    payload = sample.payload
    if len(payload) < 0xAC:
        raise ValueError(
            f"SMPL payload too short for current waveform decode: {len(payload)} bytes"
        )
    if not payload.startswith(object_current.OBJECT_MAGIC):
        raise ValueError("SMPL payload does not start with FSFSDEV3SPLX magic")
    if payload[0x0C:0x10] != b"SMPL":
        raise ValueError("payload header type is not SMPL")

    header_size = _be32(payload, 0x10)
    stored_payload_size = _be32(payload, 0x1C)
    sample_rate = _be16(payload, 0x28)
    stored_sample_width = _be16(payload, 0x2A)
    end = header_size + stored_payload_size
    if header_size < 0xAC or header_size > len(payload):
        raise ValueError(f"invalid SMPL header size {header_size} for object length {len(payload)}")
    if end > len(payload):
        raise ValueError(
            f"SMPL stored payload requires {end} bytes but object has {len(payload)} bytes"
        )

    stored = payload[header_size:end]
    decoded = object_current.decode_current_smpl_payload_info(stored, stored_sample_width)
    pcm = cast(bytes, decoded["pcm"])
    wav_sample_width_value = decoded["wav_sample_width_bytes"]
    wav_sample_width = wav_sample_width_value if isinstance(wav_sample_width_value, int) else 0
    transform = str(decoded["stored_payload_transform"])
    marker_lane = bool(decoded["marker_lane_payload_detected"])
    metadata = object_current.decode_current_smpl_metadata(payload[:0xAC])
    frame_count = len(pcm) // wav_sample_width if wav_sample_width else 0
    quality = AxklibQuality(
        quality=DataQuality.LIKELY if marker_lane else DataQuality.KNOWN,
        source=(
            "direct FSFSDEV3SPLXSMPL object header plus marker-lane payload detection"
            if marker_lane
            else "direct FSFSDEV3SPLXSMPL object header and stored payload bytes"
        ),
        notes=(
            "Current-looking marker-lane payload export is diagnostic salvage metadata, not write basis."
            if marker_lane
            else "Current SMPL payload span decoded without trimming or padding."
        ),
    )
    return Waveform(
        object_key=sample.object_key,
        source_image=sample.image,
        partition_index=sample.partition_index,
        object_offset=sample.payload_offset,
        container_kind=sample.container_kind,
        object_type=sample.type,
        sample_name=sample.name,
        pcm=pcm,
        channel_count=1,
        sample_width_bytes=wav_sample_width,
        stored_sample_width_bytes=stored_sample_width,
        sample_rate=sample_rate,
        frame_count=frame_count,
        stored_payload_size=stored_payload_size,
        stored_payload_transform=transform,
        marker_lane_payload_detected=marker_lane,
        root_key=metadata.root_key_midi_note_guess,
        fine_tune=metadata.fine_tune_cents_guess,
        loop_mode=metadata.loop_mode_candidate_0x085,
        loop_mode_label=metadata.loop_mode_a4000_ui_label_guess,
        loop_start=metadata.loop_start_frame_0x096,
        loop_length=metadata.loop_length_frames_0x09a,
        loop_end_a4000_ui=metadata.loop_end_frame_a4000_ui_guess,
        exactness_status=("current-marker-lane-salvage" if marker_lane else "exact-current-mono"),
        quality=quality,
        field_quality=object_current.current_smpl_field_quality(transform),
        metadata={
            "header_size": header_size,
            "stored_payload_size": stored_payload_size,
            "decoded_pcm_size": len(pcm),
            "source_wave_name_guess": metadata.source_wave_name_guess,
            "smpl_group_id_0x06c": metadata.smpl_group_id_0x06c,
            "smpl_link_id_0x078": metadata.smpl_link_id_0x078,
            "sample_rate_duplicate_0x07c": metadata.sample_rate_duplicate_0x07c,
            "wave_length_frames_0x092": metadata.wave_length_frames_0x092,
            "iso_raw_group": sample.metadata.get("iso_raw_group", ""),
            "iso_raw_volume": sample.metadata.get("iso_raw_volume", ""),
            "iso_group_label": sample.metadata.get("iso_group_label", ""),
            "iso_volume_label": sample.metadata.get("iso_volume_label", ""),
            "iso_group_label_source": sample.metadata.get("iso_group_label_source", ""),
            "iso_volume_label_source": sample.metadata.get("iso_volume_label_source", ""),
        },
    )


def decode_container_waveforms(
    container: AxklibContainer, *, selection: str | None = None
) -> WaveformSet:
    waveforms: list[Waveform] = []
    issues: list[WaveformIssue] = []
    for item in container.objects:
        if item.type != "SMPL":
            continue
        if (
            selection
            and selection.lower() not in item.name.lower()
            and selection != item.object_key
        ):
            continue
        try:
            waveforms.append(decode_waveform(item))
        except ValueError as exc:
            issues.append(
                WaveformIssue(
                    object_key=item.object_key,
                    sample_name=item.name,
                    code="WAVEFORM_DECODE_FAILED",
                    severity="error",
                    message=str(exc),
                )
            )
    return WaveformSet(waveforms=tuple(waveforms), issues=tuple(issues))


def iter_waveforms(
    container: AxklibContainer, *, selection: str | None = None
) -> tuple[Waveform, ...]:
    return decode_container_waveforms(container, selection=selection).waveforms


def waveform_sidecar(waveform: Waveform, wav_path: Path) -> dict[str, object]:
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
        "marker_lane_payload_detected": waveform.marker_lane_payload_detected,
        "extraction_quality": waveform.quality.quality.value,
        "extraction_basis": waveform.quality.source,
        "extraction_notes": waveform.quality.notes,
        "exactness_status": waveform.exactness_status,
        "export_mode": "sbnk-exact-mono",
        "original_wav_path": str(wav_path),
        "organization_source": "direct-smpl-category-visibility",
        "organization_relationship_path": "SMPL-category-entry",
        "organization_relationship_quality": "Known",
        "organization_owner_object_offset": None,
        "partition_name": "",
        "volume_name": "",
        "category_name": "Samples",
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


def _validate_sidecar(record: dict[str, object]) -> None:
    required = (
        "source_container",
        "object_key",
        "sample_rate",
        "sample_width_bytes",
        "stored_payload_size",
        "extraction_quality",
        "extraction_basis",
        "field_quality",
    )
    missing = [key for key in required if key not in record]
    if missing:
        raise ValueError(f"sidecar missing required fields: {', '.join(missing)}")


def export_waveforms(request: WavExportRequest) -> WavExportResult:
    if request.layout == "structured":
        from axklib.audio.structured import build_export_plan, write_structured_export

        request.output_dir.mkdir(parents=True, exist_ok=True)
        plan = build_export_plan(
            request.output_dir,
            request.waveforms,
            layout="structured",
            placements=request.placements,
            relationships=request.relationships,
        )
        structured_written, structured_skipped, structured_warnings, structured_records = (
            write_structured_export(
                plan,
                overwrite_policy=request.overwrite_policy,
                stereo_policy=request.stereo_policy,
            )
        )
        return WavExportResult(
            written_files=structured_written,
            skipped_files=structured_skipped,
            warnings=structured_warnings,
            sidecar_records=structured_records,
        )

    written: list[Path] = []
    skipped: list[Path] = []
    warnings: list[str] = []
    records: list[dict[str, object]] = []
    overwrite = request.overwrite_policy == "replace"
    request.output_dir.mkdir(parents=True, exist_ok=True)
    for index, waveform in enumerate(request.waveforms, start=1):
        source_stem = _safe_name(Path(waveform.source_image).stem or "source")
        sample_name = _safe_name(waveform.sample_name)
        object_key = _safe_name(waveform.object_key.replace(":", "_"))
        stem = f"{source_stem}_{index:05d}_{object_key}_{sample_name}"
        wav_path = request.output_dir / f"{stem}.wav"
        sidecar_path = request.output_dir / f"{stem}.json"
        if not overwrite and (wav_path.exists() or sidecar_path.exists()):
            skipped.extend([wav_path, sidecar_path])
            warnings.append(f"skipped existing export target for {waveform.object_key}")
            continue
        record = waveform_sidecar(waveform, wav_path)
        _validate_sidecar(record)
        _atomic_write_bytes(wav_path, _wav_bytes(waveform), overwrite=overwrite)
        try:
            _atomic_write_text(
                sidecar_path,
                json.dumps(to_plain(record), indent=2) + "\n",
                overwrite=overwrite,
            )
        except Exception:
            try:
                wav_path.unlink()
            except FileNotFoundError:
                pass
            raise
        written.extend([wav_path, sidecar_path])
        records.append(record)
    manifest_rows = [
        {
            "object_offset": record.get("object_offset")
            if record.get("object_offset") is not None
            else "",
            "name": record.get("name_guess", ""),
            "sample_rate": record.get("sample_rate", ""),
            "sample_width_bytes": record.get("sample_width_bytes", ""),
            "frames": record.get("frames", ""),
            "source_wav_path": record.get("wav_path", ""),
            "exported_wav_path": record.get("wav_path", ""),
            "exported_json_path": str(Path(str(record.get("wav_path", ""))).with_suffix(".json")),
            "partition_index": record.get("partition_index")
            if record.get("partition_index") is not None
            else "",
            "partition_name": record.get("partition_name", ""),
            "volume_name": record.get("volume_name", ""),
            "category_name": record.get("category_name", ""),
            "organization_source": record.get("organization_source", ""),
            "organization_relationship_path": record.get("organization_relationship_path", ""),
            "organization_relationship_quality": record.get(
                "organization_relationship_quality", ""
            ),
            "organization_owner_object_offset": "",
        }
        for record in records
    ]
    if manifest_rows:
        import csv

        manifest_path = request.output_dir / "mono_exports.csv"
        if manifest_path.exists() and not overwrite:
            warnings.append(f"manifest exists and overwrite is disabled: {manifest_path}")
        else:
            tmp_manifest_path = manifest_path.with_name(f".{manifest_path.name}.tmp")
            with tmp_manifest_path.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=list(manifest_rows[0]))
                writer.writeheader()
                writer.writerows(manifest_rows)
            os.replace(tmp_manifest_path, manifest_path)
            written.append(manifest_path)
    if request.stereo_policy == "auto":
        warnings.append(
            "stereo auto policy is reserved for the shared stereo service; mono exact exports were written"
        )
    return WavExportResult(
        written_files=tuple(written),
        skipped_files=tuple(skipped),
        warnings=tuple(warnings),
        sidecar_records=tuple(records),
    )
