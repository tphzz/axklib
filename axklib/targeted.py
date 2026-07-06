"""Targeted shared-pool WAV and SFZ export helpers."""

from __future__ import annotations

import copy
import hashlib
import os
from collections import defaultdict
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Literal

from axklib.audio import (
    OverwritePolicy,
    StereoPolicy,
    WavExportProgress,
    _atomic_write_bytes,
    _wav_bytes,
)
from axklib.audio.structured import (
    _STEREO_DECISION_MISSING_SIDE,
    _STEREO_DECISION_NOT_EXACT,
    _STEREO_DECISION_PARENT_CONFLICT,
    _STEREO_DECISION_SUCCESS,
    _STEREO_DECISION_UNKNOWN,
    _STEREO_REASON_MISSING_SIDE,
    _STEREO_REASON_PADDED,
    SampleExportTarget,
    StereoExportDecision,
    WaveExportPlan,
    _build_stereo_decisions,
    _build_volume_graph,
    _graph_object_id,
    _graph_source_ref,
    _rendered_stereo_audio_key,
    _safe_display_path_name,
    _stereo_display_name,
    _stereo_warning,
    _stereo_wav_bytes,
    _target_volume_root,
    scoped_object_key,
)
from axklib.reports import write_csv, write_json
from axklib.sfz import SfzExportRequest, SfzExportResult, export_sfz

TargetScope = Literal["file", "volume", "program", "sbac", "sbnk"]


@dataclass(frozen=True)
class TargetedExportRequest:
    output_dir: Path
    plan: WaveExportPlan
    scope: TargetScope
    selector_path: str = ""
    selector_object_key: str = ""
    write_sfz: bool = False
    stereo_policy: StereoPolicy = "auto"
    overwrite_policy: OverwritePolicy = "fail"
    progress_callback: Callable[[WavExportProgress], None] | None = None
    write_manifests: bool = False
    write_graphs: bool = False


@dataclass(frozen=True)
class TargetedExportResult:
    written_files: tuple[Path, ...]
    selection_graphs: tuple[dict[str, object], ...]
    sfz_result: SfzExportResult | None = None
    warnings: tuple[str, ...] = ()
    skipped_files: tuple[Path, ...] = ()


def _mapping(value: object) -> Mapping[str, object]:
    return value if isinstance(value, Mapping) else {}


def _list(value: object) -> list[object]:
    return value if isinstance(value, list) else []


def _text(value: object) -> str:
    return value.strip() if isinstance(value, str) else ""


def _id(row: Mapping[str, object]) -> str:
    return _text(row.get("id"))


def _object_key(row: Mapping[str, object]) -> str:
    return _text(row.get("object_key"))


def _safe_selector_dir(selector: str, fallback: str) -> Path:
    parts = [part for part in selector.replace("\\", "/").split("/") if part]
    if not parts:
        return Path(_safe_display_path_name(fallback, fallback))
    return Path(*[_safe_display_path_name(part, fallback) for part in parts])


def _relpath(path: Path, start: Path) -> str:
    return os.path.relpath(path, start=start).replace(os.sep, "/")


def _targets(value: str) -> tuple[str, ...]:
    return tuple(part for part in value.split("|") if part)


def _relationship_source_matches(row_source: str, source_image: str) -> bool:
    return not row_source or row_source == source_image


def _active_program_assignment(row: object) -> bool:
    relationship_type = getattr(row, "relationship_type", "")
    if not relationship_type.startswith("PROG_ASSIGNMENT_TO_"):
        return False
    state = getattr(row, "active_assignment_state", "")
    return state not in {"confirmed-visible-off", "confirmed-duplicate-not-active"}


def _selected_relationship_keys(
    plan: WaveExportPlan,
    scope: TargetScope,
    selector_object_key: str,
) -> tuple[set[str], set[str], set[str]]:
    selected_prog_keys: set[str] = set()
    selected_sbac_keys: set[str] = set()
    selected_sbnk_keys: set[str] = set()
    if scope == "program" and selector_object_key:
        selected_prog_keys.add(selector_object_key)
    elif scope == "sbac" and selector_object_key:
        selected_sbac_keys.add(selector_object_key)
    elif scope == "sbnk" and selector_object_key:
        selected_sbnk_keys.add(selector_object_key)

    changed = True
    while changed:
        changed = False
        for row in plan.relationships:
            if row.source_key in selected_prog_keys and _active_program_assignment(row):
                for target in _targets(row.target_key):
                    if row.relationship_type == "PROG_ASSIGNMENT_TO_SBAC":
                        if target not in selected_sbac_keys:
                            selected_sbac_keys.add(target)
                            changed = True
                    elif row.relationship_type == "PROG_ASSIGNMENT_TO_SBNK":
                        if target not in selected_sbnk_keys:
                            selected_sbnk_keys.add(target)
                            changed = True
            if row.source_key in selected_sbac_keys and row.relationship_type == "SBAC_SLOT_TO_SBNK":
                for target in _targets(row.target_key):
                    if target not in selected_sbnk_keys and row.quality in {"Known", "Likely"}:
                        selected_sbnk_keys.add(target)
                        changed = True
    return selected_prog_keys, selected_sbac_keys, selected_sbnk_keys


def _filter_plan_targets(
    plan: WaveExportPlan,
    scope: TargetScope,
    selector_path: str,
    selector_object_key: str,
) -> tuple[SampleExportTarget, ...]:
    if scope == "file":
        return plan.targets
    if scope == "volume":
        return tuple(
            target
            for target in plan.targets
            if _target_volume_root(target).as_posix() == selector_path
        )
    selected_prog_keys, selected_sbac_keys, selected_sbnk_keys = _selected_relationship_keys(
        plan,
        scope,
        selector_object_key,
    )
    return tuple(
        target
        for target in plan.targets
        if target.sample_bank_key in selected_sbac_keys
        or target.sampler_sample_key in selected_sbnk_keys
        or target.waveform.object_key in selected_sbnk_keys
        or (
            selected_prog_keys
            and not target.sample_bank_key
            and target.sampler_sample_key in selected_sbnk_keys
        )
    )


def _selection_rel(scope: TargetScope, selector_path: str) -> Path:
    selection_rel = _safe_selector_dir(
        selector_path or scope,
        "selection" if scope == "file" else scope,
    )
    if scope != "file":
        return Path(scope) / selection_rel
    return Path("file")


def _pool_target(output_dir: Path, pool_kind: str, stem: str, data: bytes) -> Path:
    digest = hashlib.sha1(data).hexdigest()[:12]
    safe_stem = _safe_display_path_name(stem, "sample")
    return output_dir / "_samples" / pool_kind / f"{safe_stem}__{digest}.wav"


def _write_pool_bytes(
    *,
    output_dir: Path,
    selection_root: Path,
    pool_kind: str,
    stem: str,
    data: bytes,
    overwrite: bool,
    progress_start: Callable[[str], None] | None = None,
    progress_done: Callable[[str], None] | None = None,
) -> tuple[str, Path, bool]:
    target = _pool_target(output_dir, pool_kind, stem, data)
    rel = _relpath(target, selection_root)
    if target.exists():
        return rel, target, False
    if progress_start is not None:
        progress_start(f"writing {rel}")
    _atomic_write_bytes(target, data, overwrite=overwrite)
    if progress_done is not None:
        progress_done(f"wrote {rel}")
    return rel, target, True


def _physical_ref_key(volume_root: Path, target: SampleExportTarget) -> str:
    return (
        f"{volume_root.as_posix()}|"
        f"{_graph_source_ref(target.waveform.source_image, target.waveform.object_key)}"
    )


def _write_selected_physical_wavs(
    *,
    output_dir: Path,
    selection_root: Path,
    targets: Sequence[SampleExportTarget],
    overwrite: bool,
    progress_start: Callable[[str], None] | None = None,
    progress_done: Callable[[str], None] | None = None,
) -> tuple[dict[str, str], list[Path]]:
    refs: dict[str, str] = {}
    written: list[Path] = []
    seen_waveforms: dict[tuple[str, str], str] = {}
    for target in sorted(
        targets,
        key=lambda item: (
            _target_volume_root(item).as_posix().lower(),
            item.waveform.source_image,
            item.waveform.object_key,
            item.waveform.sample_name.lower(),
        ),
    ):
        volume_root = _target_volume_root(target)
        ref_key = _physical_ref_key(volume_root, target)
        waveform_key = (target.waveform.source_image, target.waveform.object_key)
        rel = seen_waveforms.get(waveform_key)
        if rel is None:
            rel, wav_path, did_write = _write_pool_bytes(
                output_dir=output_dir,
                selection_root=selection_root,
                pool_kind="physical",
                stem=target.waveform.sample_name,
                data=_wav_bytes(target.waveform),
                overwrite=overwrite,
                progress_start=progress_start,
                progress_done=progress_done,
            )
            seen_waveforms[waveform_key] = rel
            if did_write:
                written.append(wav_path)
        refs[ref_key] = rel
    return refs, written


def _selected_stereo_decisions(
    decisions: tuple[StereoExportDecision, ...],
    targets: Sequence[SampleExportTarget],
) -> tuple[StereoExportDecision, ...]:
    selected_ids = {target.stable_id for target in targets}
    selected_sbnks = {target.sampler_sample_key for target in targets if target.sampler_sample_key}
    return tuple(
        decision
        for decision in decisions
        if decision.left_target_id in selected_ids
        or decision.right_target_id in selected_ids
        or decision.sbnk_object_key in selected_sbnks
    )


def _write_selected_rendered_wavs(
    *,
    output_dir: Path,
    selection_root: Path,
    targets: Sequence[SampleExportTarget],
    decisions: tuple[StereoExportDecision, ...],
    overwrite: bool,
    progress_start: Callable[[str], None] | None = None,
    progress_done: Callable[[str], None] | None = None,
) -> tuple[dict[str, dict[str, object]], tuple[StereoExportDecision, ...], list[Path], list[str]]:
    by_id = {target.stable_id: target for target in targets}
    rendered_by_audio: dict[tuple[str, str, str], str] = {}
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
        audio_key = _rendered_stereo_audio_key(left_target, right_target)
        rel = rendered_by_audio.get(audio_key)
        wav_bytes, output_frames, left_padding, right_padding = _stereo_wav_bytes(
            left_target.waveform,
            right_target.waveform,
        )
        if rel is None:
            rel, wav_path, did_write = _write_pool_bytes(
                output_dir=output_dir,
                selection_root=selection_root,
                pool_kind="rendered",
                stem=_stereo_display_name(left_target, right_target),
                data=wav_bytes,
                overwrite=overwrite,
                progress_start=progress_start,
                progress_done=progress_done,
            )
            rendered_by_audio[audio_key] = rel
            if did_write:
                written.append(wav_path)
        rendered_id = _graph_object_id("RENDERED", decision.source_image, decision.sbnk_object_key)
        rendered = {
            "id": rendered_id,
            "kind": "interleaved_stereo",
            "wav_path": rel,
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
        updated.append(
            replace(
                decision,
                stereo_wav_path=rel,
                stereo_json_path="",
                left_mono_wav_path="",
                right_mono_wav_path="",
                output_frame_count=output_frames,
                left_padding_frames=left_padding,
                right_padding_frames=right_padding,
            )
        )
    return rendered_by_sbnk, tuple(updated), written, warnings


def _rendered_progress_total(
    targets: Sequence[SampleExportTarget],
    decisions: tuple[StereoExportDecision, ...],
) -> int:
    by_id = {target.stable_id: target for target in targets}
    audio_keys: set[tuple[str, str, str]] = set()
    for decision in decisions:
        if decision.decision != _STEREO_DECISION_SUCCESS:
            continue
        left_target = by_id.get(decision.left_target_id)
        right_target = by_id.get(decision.right_target_id)
        if left_target is None or right_target is None:
            continue
        audio_keys.add(_rendered_stereo_audio_key(left_target, right_target))
    return len(audio_keys)


def _selection_manifest_row(
    *,
    scope: TargetScope,
    selector_path: str,
    selector_object_key: str,
    selection_rel: Path,
    graph_count: int,
    sfz_written: bool,
) -> dict[str, object]:
    return {
        "scope": scope,
        "selector_path": selector_path,
        "selector_object_key": selector_object_key,
        "selection_path": selection_rel.as_posix(),
        "graph_count": graph_count,
        "sfz_written": sfz_written,
    }


def export_targeted(request: TargetedExportRequest) -> TargetedExportResult:
    overwrite = request.overwrite_policy == "replace"
    request.output_dir.mkdir(parents=True, exist_ok=True)
    selection_rel = _selection_rel(request.scope, request.selector_path)
    selection_root = request.output_dir / selection_rel
    selection_root.mkdir(parents=True, exist_ok=True)

    selected_targets = _filter_plan_targets(
        request.plan,
        request.scope,
        request.selector_path,
        request.selector_object_key,
    )
    warnings: list[str] = []
    written: list[Path] = []
    stereo_decisions = (
        _selected_stereo_decisions(_build_stereo_decisions(request.plan), selected_targets)
        if request.stereo_policy == "auto"
        else ()
    )
    physical_progress_total = len(
        {(t.waveform.source_image, t.waveform.object_key) for t in selected_targets}
    )
    progress_total = physical_progress_total + _rendered_progress_total(
        selected_targets, stereo_decisions
    )
    progress_completed = 0

    def report_progress(message: str) -> None:
        if request.progress_callback is not None:
            request.progress_callback(
                WavExportProgress(
                    stage="exporting",
                    completed=progress_completed,
                    total=max(progress_total, 1),
                    message=message,
                )
            )

    def advance_progress(message: str) -> None:
        nonlocal progress_completed
        progress_completed += 1
        if request.progress_callback is not None:
            request.progress_callback(
                WavExportProgress(
                    stage="exporting",
                    completed=progress_completed,
                    total=max(progress_total, 1),
                    message=message,
                )
            )

    physical_refs, physical_written = _write_selected_physical_wavs(
        output_dir=request.output_dir,
        selection_root=selection_root,
        targets=selected_targets,
        overwrite=overwrite,
        progress_start=report_progress,
        progress_done=advance_progress,
    )
    written.extend(physical_written)

    rendered_by_sbnk: dict[str, dict[str, object]] = {}
    final_stereo_decisions: tuple[StereoExportDecision, ...] = ()
    if request.stereo_policy == "auto":
        rendered_by_sbnk, final_stereo_decisions, rendered_written, stereo_warnings = (
            _write_selected_rendered_wavs(
                output_dir=request.output_dir,
                selection_root=selection_root,
                targets=selected_targets,
                decisions=stereo_decisions,
                overwrite=overwrite,
                progress_start=report_progress,
                progress_done=advance_progress,
            )
        )
        written.extend(rendered_written)
        warnings.extend(stereo_warnings)

    groups: dict[str, list[SampleExportTarget]] = defaultdict(list)
    roots: dict[str, Path] = {}
    for target in selected_targets:
        root = _target_volume_root(target)
        key = root.as_posix()
        roots[key] = root
        groups[key].append(target)

    selection_graphs: list[dict[str, object]] = []
    for key in sorted(groups):
        graph = _build_volume_graph(
            request.plan,
            roots[key],
            tuple(groups[key]),
            physical_refs,
            rendered_by_sbnk,
            final_stereo_decisions,
        )
        graph.setdefault("selection", {})
        graph["selection"] = {
            "scope": request.scope,
            "selector_path": request.selector_path,
            "selector_object_key": request.selector_object_key,
        }
        volume = graph.get("volume")
        if isinstance(volume, dict):
            volume["source_volume_path"] = volume.get("path", "")
            volume["path"] = selection_rel.as_posix()
        selection_graphs.append(copy.deepcopy(graph))

    if request.write_graphs:
        graph_path = selection_root / "selection.axklib.json"
        if graph_path.exists() and not overwrite:
            raise FileExistsError(f"selection graph already exists: {graph_path}")
        write_json(graph_path, selection_graphs)
        written.append(graph_path)

    sfz_result = None
    if request.write_sfz:
        sfz_result = export_sfz(
            SfzExportRequest(
                output_dir=request.output_dir,
                volume_graphs=selection_graphs,
                overwrite_policy=request.overwrite_policy,
                write_manifests=request.write_manifests,
            )
        )
        written.extend(sfz_result.written_files)
        warnings.extend(sfz_result.warnings)

    if request.write_manifests:
        manifest_rows = [
            _selection_manifest_row(
                scope=request.scope,
                selector_path=request.selector_path,
                selector_object_key=request.selector_object_key,
                selection_rel=selection_rel,
                graph_count=len(selection_graphs),
                sfz_written=bool(sfz_result),
            )
        ]
        write_csv(selection_root / "selection_exports.csv", manifest_rows)
        write_json(selection_root / "selection_exports.json", manifest_rows)
        written.extend(
            [selection_root / "selection_exports.csv", selection_root / "selection_exports.json"]
        )
    return TargetedExportResult(
        tuple(dict.fromkeys(written)),
        tuple(selection_graphs),
        sfz_result,
        tuple(warnings),
        (),
    )