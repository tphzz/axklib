"""SFZ export helpers built from structured axklib volume graphs."""

from __future__ import annotations

import os
import re
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Literal

from axklib.audio.structured import _safe_display_path_name
from axklib.key_limits import int_value, is_orig_key_limit, resolve_sbnk_key_range
from axklib.reports import write_csv, write_json

OverwritePolicy = Literal["fail", "replace"]


@dataclass(frozen=True)
class SfzExportProgress:
    """Progress event emitted while writing SFZ files and manifests."""

    stage: str
    completed: int
    total: int
    message: str = ""


@dataclass(frozen=True)
class SfzExportRequest:
    """Request for generating SFZ files from structured export graphs.

    Args:
        output_dir: Root directory that already contains structured waveform
            exports.
        volume_graphs: Parsed ``volume.axklib.json`` graph records from the
            structured waveform export.
        overwrite_policy: ``"replace"`` permits replacing existing SFZ and
            manifest files. ``"fail"`` raises if a target already exists.
        progress_callback: Optional callback for CLI progress reporting.
        write_manifests: Write ``sfz_exports.csv`` and ``sfz_exports.json`` beside
            generated SFZ files. Defaults to ``False`` so normal CLI exports stay compact.
    """

    output_dir: Path
    volume_graphs: Sequence[Mapping[str, object]]
    overwrite_policy: OverwritePolicy = "fail"
    progress_callback: Callable[[SfzExportProgress], None] | None = None
    write_manifests: bool = False


@dataclass(frozen=True)
class SfzExportManifestRow:
    """One generated or skipped SFZ instrument row."""

    volume_path: str
    instrument_type: str
    instrument_id: str
    instrument_name: str
    sfz_path: str
    region_count: int
    rendered_region_count: int
    physical_region_count: int
    skipped_region_count: int
    status: str
    notes: str


@dataclass(frozen=True)
class SfzExportResult:
    """Result of an SFZ export pass."""

    written_files: tuple[Path, ...]
    manifest_rows: tuple[SfzExportManifestRow, ...]
    warnings: tuple[str, ...]


_INVALID_FILENAME_CHARS = re.compile(r'[<>:"/\\|?*\x00-\x1f]')


def _mapping(value: object) -> Mapping[str, object]:
    return value if isinstance(value, Mapping) else {}


def _list(value: object) -> list[object]:
    return value if isinstance(value, list) else []


def _text(value: object) -> str:
    return value.strip() if isinstance(value, str) else ""


def _int(value: object) -> int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    return None


def _safe_filename_stem(name: str, fallback: str) -> str:
    return _safe_display_path_name(name, fallback)


def _dedupe_path(path: Path, used: set[Path]) -> Path:
    if path not in used:
        used.add(path)
        return path
    stem = path.stem
    suffix = path.suffix
    index = 2
    while True:
        candidate = path.with_name(f"{stem} ({index}){suffix}")
        if candidate not in used:
            used.add(candidate)
            return candidate
        index += 1


def _volume_path(graph: Mapping[str, object]) -> str:
    volume = _mapping(graph.get("volume"))
    path = _text(volume.get("path"))
    if path:
        return path
    name = _text(volume.get("name"))
    return name or "_unplaced"


def _object_rows(graph: Mapping[str, object], kind: str) -> list[Mapping[str, object]]:
    objects = _mapping(graph.get("objects"))
    rows = []
    for item in _list(objects.get(kind)):
        if isinstance(item, Mapping):
            rows.append(item)
    return rows


def _id(row: Mapping[str, object]) -> str:
    return _text(row.get("id")) or _text(row.get("source_ref")) or _text(row.get("object_key"))


def _member_parameter_row(sbnk: Mapping[str, object]) -> Mapping[str, object]:
    parameters = _mapping(sbnk.get("parameters"))
    decoded = parameters.get("decoded_current_sbnk_member_parameters")
    if isinstance(decoded, Mapping):
        return decoded
    if isinstance(decoded, list):
        for item in decoded:
            if not isinstance(item, Mapping):
                continue
            member_parameters = item.get("member_parameters")
            if isinstance(member_parameters, Mapping):
                return member_parameters
    return {}


def _smpl_lookup(graph: Mapping[str, object]) -> dict[str, Mapping[str, object]]:
    rows = _object_rows(graph, "smpl")
    lookup: dict[str, Mapping[str, object]] = {}
    for row in rows:
        for key_name in ("id", "source_ref", "object_key"):
            key = _text(row.get(key_name))
            if key:
                lookup[key] = row
    return lookup


def _playback_for_waveform(
    waveform: Mapping[str, object], smpl_by_id: Mapping[str, Mapping[str, object]]
) -> Mapping[str, object]:
    for key_name in ("smpl_id", "smpl_ref"):
        key = _text(waveform.get(key_name))
        if key and key in smpl_by_id:
            return _mapping(smpl_by_id[key].get("playback"))
    return {}


def _region_sample_path(volume_root: Path, sfz_path: Path, wav_path: str) -> str:
    wav_abs = volume_root / Path(wav_path)
    relative = os.path.relpath(wav_abs, start=sfz_path.parent)
    return relative.replace(os.sep, "/")


def _add_int_opcode(parts: list[str], name: str, value: int | None) -> None:
    if value is not None:
        parts.append(f"{name}={value}")


def _resolved_key_range_row(sbnk: Mapping[str, object]) -> Mapping[str, object]:
    parameters = _mapping(sbnk.get("parameters"))
    return _mapping(parameters.get("resolved_key_range"))


def _key_range(
    params: Mapping[str, object],
    resolved: Mapping[str, object],
    root_key: int | None,
) -> tuple[int | None, int | None, str]:
    resolved_low = int_value(resolved.get("low_midi"))
    resolved_high = int_value(resolved.get("high_midi"))
    if resolved_low is not None and resolved_high is not None:
        if 0 <= resolved_low <= 127 and 0 <= resolved_high <= 127:
            if resolved_high >= resolved_low:
                return resolved_low, resolved_high, ""
            return None, None, "resolved key range is invalid"

    projected = resolve_sbnk_key_range(params, root_key=root_key)
    if projected is not None:
        return (
            int_value(projected.get("low_midi")),
            int_value(projected.get("high_midi")),
            "",
        )

    low = _int(params.get("key_range_low_0x0e3"))
    high = _int(params.get("key_range_high_0x0e2"))
    if is_orig_key_limit(low, limit="low") or is_orig_key_limit(high, limit="high"):
        return None, None, "sampler Orig key limit could not be resolved without a root key"
    if low is None or high is None or not (0 <= low <= 127 and 0 <= high <= 127):
        return None, None, ""
    if high < low:
        return None, None, "decoded key range is invalid"
    return low, high, ""



def _member_value(
    params: Mapping[str, object],
    waveform: Mapping[str, object],
    left_name: str,
    right_name: str,
) -> int | None:
    role = _text(waveform.get("role")).lower()
    if role == "right":
        return _int(params.get(right_name))
    return _int(params.get(left_name))


def _loop_opcodes(playback: Mapping[str, object]) -> list[str]:
    label = _text(playback.get("loop_mode_label")).lower()
    if not label:
        return []
    if "one" in label and "shot" in label:
        return ["loop_mode=one_shot"]
    start = _int(playback.get("loop_start_frame"))
    length = _int(playback.get("loop_length_frames"))
    if start is None:
        return []
    if length is not None and length > 0:
        end = start + length - 1
    else:
        graph_end = _int(playback.get("loop_end_frame"))
        if graph_end is None:
            graph_end = _int(playback.get("loop_end_frame_a4000_ui"))
        if graph_end is None:
            return []
        end = graph_end - 1
    if end < start:
        return []
    return ["loop_mode=loop_continuous", f"loop_start={start}", f"loop_end={end}"]


def _region_line(
    *,
    volume_root: Path,
    sfz_path: Path,
    wav_path: str,
    params: Mapping[str, object],
    resolved_key_range: Mapping[str, object],
    playback: Mapping[str, object],
    waveform: Mapping[str, object],
    display_name: str,
    notes: list[str],
    pan: int | None = None,
) -> str:
    parts = ["<region>"]
    root = _member_value(
        params,
        waveform,
        "left_root_key_0x0d6",
        "right_root_key_0x0d7",
    )
    if root is None:
        root = _int(playback.get("root_key_midi"))
    low, high, key_range_note = _key_range(params, resolved_key_range, root)
    if key_range_note:
        notes.append(f"{display_name}: {key_range_note}")
    _add_int_opcode(parts, "lokey", low)
    _add_int_opcode(parts, "hikey", high)
    if root is not None and 0 <= root <= 127:
        parts.append(f"pitch_keycenter={root}")
    coarse = _int(params.get("coarse_tune_0x0d5"))
    if coarse is not None and -64 <= coarse <= 64:
        parts.append(f"transpose={coarse}")
    fine = _member_value(
        params,
        waveform,
        "left_fine_tune_cents_0x0dc",
        "right_fine_tune_cents_0x0dd",
    )
    if fine is None:
        fine = _int(playback.get("fine_tune_cents"))
    if fine is not None and -1200 <= fine <= 1200:
        parts.append(f"tune={fine}")
    if pan is not None:
        parts.append(f"pan={pan}")
    parts.extend(_loop_opcodes(playback))
    parts.append(f"sample={_region_sample_path(volume_root, sfz_path, wav_path)}")
    return " ".join(parts)


def _instrument_regions(
    *,
    graph: Mapping[str, object],
    volume_root: Path,
    sfz_path: Path,
    sbnks: Sequence[Mapping[str, object]],
) -> tuple[list[str], int, int, int, list[str]]:
    smpl_by_id = _smpl_lookup(graph)
    lines: list[str] = []
    rendered_count = 0
    physical_count = 0
    skipped_count = 0
    notes: list[str] = []
    for sbnk in sbnks:
        display_name = _text(sbnk.get("display_name")) or _id(sbnk)
        params = _member_parameter_row(sbnk)
        resolved_key_range = _resolved_key_range_row(sbnk)
        rendered = _mapping(sbnk.get("rendered_audio"))
        rendered_wav = _text(rendered.get("wav_path"))
        physical_waveforms = [
            item for item in _list(sbnk.get("physical_waveforms")) if isinstance(item, Mapping)
        ]
        if rendered_wav:
            playback = (
                _playback_for_waveform(physical_waveforms[0], smpl_by_id)
                if physical_waveforms
                else {}
            )
            lines.append(f"// {display_name}")
            lines.append(
                _region_line(
                    volume_root=volume_root,
                    sfz_path=sfz_path,
                    wav_path=rendered_wav,
                    params=params,
                    resolved_key_range=resolved_key_range,
                    playback=playback,
                    waveform=physical_waveforms[0] if physical_waveforms else {},
                    display_name=display_name,
                    notes=notes,
                )
            )
            rendered_count += 1
            continue
        physical_regions = [
            waveform for waveform in physical_waveforms if _text(waveform.get("wav_path"))
        ]
        physical_roles = {_text(waveform.get("role")).lower() for waveform in physical_regions}
        pan_physical_pair = {"left", "right"}.issubset(physical_roles)
        wrote_physical = False
        for waveform in physical_regions:
            wav_path = _text(waveform.get("wav_path"))
            role = _text(waveform.get("role")).lower()
            pan = None
            if pan_physical_pair:
                pan = -100 if role == "left" else 100 if role == "right" else None
            lines.append(f"// {display_name}")
            lines.append(
                _region_line(
                    volume_root=volume_root,
                    sfz_path=sfz_path,
                    wav_path=wav_path,
                    params=params,
                    resolved_key_range=resolved_key_range,
                    playback=_playback_for_waveform(waveform, smpl_by_id),
                    waveform=waveform,
                    pan=pan,
                    display_name=display_name,
                    notes=notes,
                )
            )
            physical_count += 1
            wrote_physical = True
        if not wrote_physical:
            skipped_count += 1
            notes.append(f"{display_name}: no exported WAV reference")
    return lines, rendered_count, physical_count, skipped_count, notes


def _sbac_instruments(
    graph: Mapping[str, object], sbnk_by_id: Mapping[str, Mapping[str, object]]
) -> list[tuple[str, str, str, list[Mapping[str, object]]]]:
    instruments: list[tuple[str, str, str, list[Mapping[str, object]]]] = []
    for sbac in _object_rows(graph, "sbac"):
        members: list[Mapping[str, object]] = []
        seen_members: set[str] = set()
        for item in _list(sbac.get("members")):
            if not isinstance(item, Mapping):
                continue
            member_id = _text(item.get("sbnk_id"))
            if member_id and member_id in sbnk_by_id and member_id not in seen_members:
                seen_members.add(member_id)
                members.append(sbnk_by_id[member_id])
        if members:
            name = _text(sbac.get("display_name")) or _id(sbac)
            filename_name = name if name.startswith("B ") else f"B {name}"
            instruments.append(("SBAC", _id(sbac), filename_name, members))
    return instruments


def _standalone_sbnk_instruments(
    graph: Mapping[str, object], member_ids: set[str]
) -> list[tuple[str, str, str, list[Mapping[str, object]]]]:
    instruments: list[tuple[str, str, str, list[Mapping[str, object]]]] = []
    for sbnk in _object_rows(graph, "sbnk"):
        sbnk_id = _id(sbnk)
        if sbnk_id in member_ids:
            continue
        name = _text(sbnk.get("display_name")) or sbnk_id
        instruments.append(("SBNK", sbnk_id, name, [sbnk]))
    return instruments


def _member_sbnk_ids(graph: Mapping[str, object]) -> set[str]:
    ids: set[str] = set()
    for sbac in _object_rows(graph, "sbac"):
        for item in _list(sbac.get("members")):
            if not isinstance(item, Mapping):
                continue
            member_id = _text(item.get("sbnk_id"))
            if member_id:
                ids.add(member_id)
    return ids


def _write_text(path: Path, text: str, overwrite_policy: OverwritePolicy) -> None:
    if path.exists() and overwrite_policy != "replace":
        raise FileExistsError(f"SFZ target already exists: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def _write_manifest(
    volume_root: Path, rows: Sequence[SfzExportManifestRow], overwrite_policy: OverwritePolicy
) -> list[Path]:
    written: list[Path] = []
    csv_path = volume_root / "sfz_exports.csv"
    json_path = volume_root / "sfz_exports.json"
    for path in (csv_path, json_path):
        if path.exists() and overwrite_policy != "replace":
            raise FileExistsError(f"SFZ manifest already exists: {path}")
    write_csv(csv_path, rows)
    write_json(json_path, rows)
    written.extend([csv_path, json_path])
    return written


def export_sfz(request: SfzExportRequest) -> SfzExportResult:
    """Write volume-scoped SFZ files from structured export graphs."""
    written_files: list[Path] = []
    manifest_rows: list[SfzExportManifestRow] = []
    warnings: list[str] = []
    total = len(request.volume_graphs)
    for index, graph in enumerate(request.volume_graphs, start=1):
        volume_rel = _volume_path(graph)
        volume_root = request.output_dir / Path(volume_rel)
        sfz_dir = volume_root
        request.progress_callback and request.progress_callback(
            SfzExportProgress("sfz", index - 1, total, volume_rel)
        )
        sbnk_by_id = {_id(row): row for row in _object_rows(graph, "sbnk") if _id(row)}
        member_ids = _member_sbnk_ids(graph)
        instruments = _sbac_instruments(graph, sbnk_by_id)
        instruments.extend(_standalone_sbnk_instruments(graph, member_ids))
        used_paths: set[Path] = set()
        volume_rows: list[SfzExportManifestRow] = []
        for instrument_type, instrument_id, instrument_name, sbnks in instruments:
            stem = _safe_filename_stem(instrument_name, instrument_type.lower())
            sfz_path = _dedupe_path(sfz_dir / f"{stem}.sfz", used_paths)
            region_lines, rendered_count, physical_count, skipped_count, notes = (
                _instrument_regions(
                    graph=graph,
                    volume_root=volume_root,
                    sfz_path=sfz_path,
                    sbnks=sbnks,
                )
            )
            if region_lines:
                text = "\n".join(
                    [
                        "// Generated by axklib",
                        f"// Volume: {volume_rel}",
                        f"// Instrument: {instrument_name}",
                        "",
                        "<group>",
                        *region_lines,
                        "",
                    ]
                )
                _write_text(sfz_path, text, request.overwrite_policy)
                written_files.append(sfz_path)
                status = "written"
                sfz_rel = sfz_path.relative_to(volume_root).as_posix()
            else:
                status = "skipped"
                sfz_rel = ""
                warnings.append(
                    f"{volume_rel}: skipped {instrument_name}; no exported WAV references"
                )
            row = SfzExportManifestRow(
                volume_path=volume_rel,
                instrument_type=instrument_type,
                instrument_id=instrument_id,
                instrument_name=instrument_name,
                sfz_path=sfz_rel,
                region_count=rendered_count + physical_count,
                rendered_region_count=rendered_count,
                physical_region_count=physical_count,
                skipped_region_count=skipped_count,
                status=status,
                notes="; ".join(notes),
            )
            volume_rows.append(row)
            manifest_rows.append(row)
            for note in notes:
                warnings.append(f"{volume_rel}: {instrument_name}: {note}")
        if request.write_manifests:
            written_files.extend(_write_manifest(volume_root, volume_rows, request.overwrite_policy))
        request.progress_callback and request.progress_callback(
            SfzExportProgress("sfz", index, total, volume_rel)
        )
    return SfzExportResult(tuple(written_files), tuple(manifest_rows), tuple(warnings))


__all__ = [
    "SfzExportManifestRow",
    "SfzExportProgress",
    "SfzExportRequest",
    "SfzExportResult",
    "export_sfz",
]
