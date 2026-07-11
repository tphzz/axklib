"""Unified axklib command-line entrypoint."""

from __future__ import annotations

import argparse
import json
import sys
import traceback
from collections import Counter
from collections.abc import Sequence
from dataclasses import replace
from pathlib import Path
from typing import Any

import axklib
from axklib.alteration import alter_hds, load_alteration_manifest
from axklib.audio import (
    WaveformIssue,
    WaveformPlacement,
    WaveformRelationship,
    decode_container_waveforms,
    decode_waveform,
)
from axklib.audio.structured import build_export_plan
from axklib.build_manifest import build_hds_from_manifest, load_hds_build_manifest
from axklib.containers import expand_inputs, sfs_inventory
from axklib.content_tree import (
    ContentNode,
    ContentTree,
    ContentTreeRenderOptions,
    build_content_tree_for_container,
    build_content_trees_for_paths,
    content_tree_path_rows,
    content_tree_to_json,
    load_known_object_placements,
    render_content_tree_paths,
    render_content_tree_text,
    summary_line,
)
from axklib.coverage import summarize_relationship_coverage
from axklib.model import DataQuality
from axklib.objects.decoded import decode_objects, result_field_names, result_issue_codes
from axklib.parameters import current as current_parameters
from axklib.parameters import sbnk_links
from axklib.relationships import (
    ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE,
    build_relationship_graph,
    build_relationship_graph_for_loaded_results,
    build_relationship_graph_for_path_results,
)
from axklib.reports import to_plain, write_csv, write_dict_csv, write_json, write_rows_json
from axklib.reports.schema import (
    ReportSchemaManifest,
    write_schema_index,
    write_schema_manifest,
)
from axklib.targeted import (
    TargetedExportRequest,
    TargetedExportResult,
    TargetScope,
    export_targeted,
)
from axklib.validation import (
    VALIDATION_POLICIES,
    validate_export_sidecars,
    validate_path_results,
    validate_paths,
)
from axklib.validation import allocation as allocation_validation
from axklib.validation import volume as volume_validation

VERSION = "0.1.0-plan008"


class _ProgressLine:
    def __init__(self, mode: str) -> None:
        self.enabled = mode == "always" or (mode == "auto" and sys.stderr.isatty())
        self._last_length = 0
        self._wrote = False

    def update(self, stage: str, completed: int, total: int, message: str = "") -> None:
        if not self.enabled:
            return
        percent = 100 if total <= 0 else max(0, min(100, int(completed * 100 / total)))
        text = f"{stage}: {percent:3d}% ({completed}/{total})"
        if message:
            detail = str(message).replace("\r", " ").replace("\n", " ")
            if len(detail) > 72:
                detail = f"...{detail[-69:]}"
            text = f"{text} {detail}"
        padding = " " * max(0, self._last_length - len(text))
        sys.stderr.write(f"\r{text}{padding}")
        sys.stderr.flush()
        self._last_length = len(text)
        self._wrote = True

    def finish(self) -> None:
        if self.enabled and self._wrote:
            sys.stderr.write("\n")
            sys.stderr.flush()


def run_info(args: argparse.Namespace) -> int:
    options = axklib.OpenOptions(strict=args.strict, include_payloads=True)
    results = build_content_trees_for_paths(
        _input_paths(args.paths),
        options=options,
        include_unresolved=args.show_unresolved,
        include_default_programs=args.show_default_programs,
    )
    exit_code = 0
    if args.format == "json":
        payload = {
            "trees": [content_tree_to_json(tree) for tree in results.trees],
            "load_errors": [
                {
                    "path": str(error.path),
                    "error_code": error.error.error_code
                    if error.error
                    else "AXKLIB_CONTAINER_OPEN_FAILED",
                    "message": error.error.message if error.error else "unknown error",
                }
                for error in results.load_errors
            ],
        }
        print(json.dumps(payload, indent=2))
        return 1 if results.load_errors else 0

    for error_result in results.load_errors:
        exit_code = 1
        error = error_result.error
        message = error.message if error else "unknown error"
        code = error.error_code if error else "AXKLIB_CONTAINER_OPEN_FAILED"
        print(f"{error_result.path}\tERROR\t{code}\t{message}")

    if args.format == "paths":
        for tree in results.trees:
            print(render_content_tree_paths(tree))
        return exit_code

    if args.format == "summary":
        open_results = axklib.open_many(_input_paths(args.paths), options=options)
        for result in open_results:
            if result.container is not None:
                print(summary_line(result.container))
        return exit_code

    render_options = ContentTreeRenderOptions(
        max_depth=args.max_depth,
        show_quality=args.show_quality,
        show_unresolved=args.show_unresolved,
    )
    for tree in results.trees:
        print(render_content_tree_text(tree, render_options))
    return exit_code


def _quality(value: str) -> DataQuality:
    for item in DataQuality:
        if item.value == value:
            return item
    return DataQuality.UNKNOWN


def _result_objects_by_source_and_key(results: Sequence[object]) -> dict[tuple[str, str], Any]:
    objects: dict[tuple[str, str], Any] = {}
    for result in results:
        container = getattr(result, "container", None)
        if container is None:
            continue
        for item in container.objects:
            objects[(item.image, item.object_key)] = item
    return objects


def _known_single_target(row: WaveformRelationship) -> str | None:
    targets = [part for part in row.target_key.split("|") if part]
    return targets[0] if len(targets) == 1 else None


def _relationship_drives_active_program_context(row: WaveformRelationship) -> bool:
    if not row.relationship_type.startswith("PROG_ASSIGNMENT_TO_"):
        return False
    if not row.active_assignment_state:
        return True
    return row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE


def _current_sbnk_parameter_context(
    waveform: Any,
    objects: dict[tuple[str, str], Any],
    relationships: tuple[WaveformRelationship, ...],
) -> list[dict[str, object]]:
    contexts: list[dict[str, object]] = []
    for row in relationships:
        if (
            row.quality != "Known"
            or row.relationship_type
            not in {"SBNK_LEFT_MEMBER_TO_SMPL", "SBNK_RIGHT_MEMBER_TO_SMPL"}
            or row.source_image != waveform.source_image
            or _known_single_target(row) != waveform.object_key
        ):
            continue
        sbnk = objects.get((waveform.source_image, row.source_key))
        if sbnk is None or sbnk.type != "SBNK":
            continue
        try:
            decoded = current_parameters.decode_current_sbnk_members(sbnk.payload)
        except Exception:
            continue
        contexts.append(
            {
                "sample_bank_object_key": row.source_key,
                "sample_bank_name": sbnk.name,
                "relationship_type": row.relationship_type,
                "member_parameters": to_plain(decoded.member_parameters),
                "left_member": to_plain(decoded.left),
                "right_member": to_plain(decoded.right) if decoded.right is not None else None,
                "bank_topology": decoded.bank_topology,
                "linked_program_numbers": list(decoded.linked_program_numbers),
            }
        )
    return contexts


def _current_prog_parameter_context(
    waveform: Any,
    objects: dict[tuple[str, str], Any],
    relationships: tuple[WaveformRelationship, ...],
) -> list[dict[str, object]]:
    parent_keys = {
        row.source_key
        for row in relationships
        if row.quality == "Known"
        and row.source_image == waveform.source_image
        and _known_single_target(row) == waveform.object_key
        and row.relationship_type in {"SBNK_LEFT_MEMBER_TO_SMPL", "SBNK_RIGHT_MEMBER_TO_SMPL"}
    }
    sbac_keys = {
        row.source_key
        for row in relationships
        if row.quality == "Known"
        and row.source_image == waveform.source_image
        and _known_single_target(row) in parent_keys
        and row.relationship_type == "SBAC_SLOT_TO_SBNK"
    }
    targets = parent_keys | sbac_keys
    contexts: list[dict[str, object]] = []
    for row in relationships:
        if (
            row.quality != "Known"
            or row.source_image != waveform.source_image
            or _known_single_target(row) not in targets
            or not _relationship_drives_active_program_context(row)
        ):
            continue
        prog = objects.get((waveform.source_image, row.source_key))
        if prog is None or prog.type != "PROG":
            continue
        try:
            common = current_parameters.decode_prog_common_fields(prog.payload)
            effects = current_parameters.iter_prog_effect_common_blocks(prog.payload)
        except Exception:
            continue
        contexts.append(
            {
                "program_object_key": row.source_key,
                "program_name": prog.name,
                "relationship_type": row.relationship_type,
                "common": to_plain(common),
                "effects": to_plain(effects),
            }
        )
    return contexts


def _enrich_waveform_parameter_metadata(
    waveforms: Sequence[Any],
    results: Sequence[object],
    relationships: tuple[WaveformRelationship, ...],
) -> tuple[Any, ...]:
    objects = _result_objects_by_source_and_key(results)
    enriched = []
    for waveform in waveforms:
        metadata = dict(waveform.metadata)
        sbnk_context = _current_sbnk_parameter_context(waveform, objects, relationships)
        prog_context = _current_prog_parameter_context(waveform, objects, relationships)
        if sbnk_context:
            metadata["decoded_current_sbnk_member_parameters"] = sbnk_context
        if prog_context:
            metadata["decoded_current_prog_parameters"] = prog_context
        enriched.append(replace(waveform, metadata=metadata))
    return tuple(enriched)


def _wave_export_context(
    results: Sequence[object],
) -> tuple[dict[str, WaveformPlacement], tuple[WaveformRelationship, ...]]:
    placements: dict[str, WaveformPlacement] = {}
    relationship_items: list[Any] = []
    for result in results:
        container = getattr(result, "container", None)
        if container is None:
            continue
        placement_rows, _issues = load_known_object_placements(container)
        source_image = (
            container.objects[0].image if container.objects else str(container.source_path)
        )
        for object_key, placement in placement_rows.items():
            placements[f"{source_image}\u241f{object_key}"] = WaveformPlacement(
                partition_index=placement.partition_index,
                partition_name=placement.partition_name,
                volume_name=placement.volume_name,
                category_name=placement.category_name,
                display_name=placement.entry_name,
                quality=_quality(placement.quality),
                source=placement.basis,
                relationship_path=f"{placement.category_name or 'object'}-category-entry",
                raw_volume_path=placement.raw_volume_path,
            )
        relationship_items.extend(container.objects)
    if relationship_items:
        graph = build_relationship_graph(relationship_items)
        relationships = tuple(
            WaveformRelationship(
                source_key=row.source_key,
                target_key=row.target_key,
                relationship_type=row.relationship_type,
                quality=row.quality,
                basis=row.basis,
                raw_fields=row.raw_fields,
                ambiguity_notes=row.ambiguity_notes,
                source_image=row.source_image,
                scope_key=row.scope_key,
                assignment_index=row.assignment_index,
                assignment_name=row.assignment_name,
                assignment_row_state=row.assignment_row_state,
                active_assignment_state=row.active_assignment_state,
                assignment_rch_assign_display=row.assignment_rch_assign_display,
            )
            for row in graph.relationships
        )
    else:
        relationships = ()
    return placements, relationships


def _ensure_output_dir(path: Path, *, overwrite: bool = False) -> None:
    if path.exists() and any(path.iterdir()) and not overwrite:
        raise FileExistsError(f"output directory already exists and is not empty: {path}")
    path.mkdir(parents=True, exist_ok=True)


def _schema_dir(output_dir: Path) -> Path:
    return output_dir / "_schemas"


def _write_report_schema(
    output_dir: Path,
    report_name: str,
    rows: Sequence[object],
    *,
    semantic_notes: str = "",
    replacement_notes: str = "",
) -> ReportSchemaManifest:
    return write_schema_manifest(
        _schema_dir(output_dir) / f"{report_name}.schema.json",
        report_name,
        rows,
        semantic_notes=semantic_notes,
        replacement_notes=replacement_notes,
        source_command="axklib",
        library_version=VERSION,
    )


def _read_json_rows(path: Path) -> list[object]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if isinstance(payload, list):
        return payload
    return [payload]


_TARGET_SCOPES = {"file", "volume", "program", "sbac", "sbnk"}


def _load_extract_results(
    paths: Sequence[Path],
    *,
    strict: bool,
    progress: _ProgressLine,
) -> tuple[list[Any], int]:
    options = axklib.OpenOptions(strict=strict, include_payloads=True, lazy_payloads=True)
    results: list[Any] = []
    load_errors = 0
    progress.update("loading", 0, len(paths), "opening inputs")
    for index, path in enumerate(paths, start=1):
        progress.update("loading", index - 1, len(paths), str(path))
        path_results = axklib.open_many([path], options=options)
        results.extend(path_results)
        for result in path_results:
            if result.container is None:
                load_errors += 1
                error = result.error
                message = error.message if error else "unknown error"
                code = error.error_code if error else "AXKLIB_CONTAINER_OPEN_FAILED"
                print(f"{result.path}\tERROR\t{code}\t{message}")
        progress.update("loading", index, len(paths), path.name)
    return results, load_errors


def _content_trees_for_results(results: Sequence[object]) -> tuple[ContentTree, ...]:
    trees: list[ContentTree] = []
    for result in results:
        container = getattr(result, "container", None)
        if container is not None:
            trees.append(build_content_tree_for_container(container, include_validation=False))
    return tuple(trees)


def _selector_object_keys_from_trees(
    trees: Sequence[ContentTree],
    scope: TargetScope,
    selector_paths: Sequence[str],
) -> tuple[tuple[str, str], ...]:
    if scope == "file":
        return (("", ""),)
    cleaned = tuple(path for path in selector_paths if path)
    if not cleaned:
        raise ValueError(
            f"extract {scope} requires at least one --path from `axklib info --format paths`"
        )
    rows = [row for tree in trees for row in content_tree_path_rows(tree)]
    selections: list[tuple[str, str]] = []
    for selector_path in cleaned:
        matches = [row for row in rows if row.scope == scope and row.path == selector_path]
        if not matches:
            raise ValueError(
                f"selector path not found for {scope}: {selector_path}. "
                "Run `axklib info --format paths` and copy the path column."
            )
        object_keys = {row.object_key for row in matches}
        if len(object_keys) > 1:
            raise ValueError(f"selector path is ambiguous for {scope}: {selector_path}")
        selections.append((selector_path, matches[0].object_key))
    return tuple(selections)


def _find_selector_node(
    trees: Sequence[ContentTree], scope: TargetScope, selector_path: str
) -> ContentNode | None:
    def visit(node: ContentNode) -> ContentNode | None:
        if node.selector_path == selector_path:
            if scope == "volume" and node.node_type == "volume":
                return node
            if scope == "program" and node.object_type == "PROG":
                return node
            if scope == "sbac" and node.object_type == "SBAC":
                return node
            if scope == "sbnk" and node.object_type == "SBNK":
                return node
        for child in node.children:
            found = visit(child)
            if found is not None:
                return found
        return None

    for tree in trees:
        for root in tree.roots:
            found = visit(root)
            if found is not None:
                return found
    return None


def _descendant_keys(node: ContentNode) -> dict[str, set[str]]:
    keys: dict[str, set[str]] = {"PROG": set(), "SBAC": set(), "SBNK": set(), "SMPL": set()}

    def visit(current: ContentNode) -> None:
        if current.object_type in keys and current.object_key:
            keys[current.object_type].add(current.object_key)
        for child in current.children:
            visit(child)

    visit(node)
    return keys


def _target_keys(value: str) -> tuple[str, ...]:
    return tuple(part for part in value.split("|") if part)


def _relationship_target_type(relationship_type: str) -> str:
    if relationship_type.endswith("_TO_SBAC"):
        return "SBAC"
    if relationship_type.endswith("_TO_SBNK"):
        return "SBNK"
    if relationship_type.endswith("_TO_SMPL"):
        return "SMPL"
    return ""


def _row_is_active_assignment(row: WaveformRelationship) -> bool:
    if not row.relationship_type.startswith("PROG_ASSIGNMENT_TO_"):
        return False
    return row.active_assignment_state not in {
        "confirmed-visible-off",
        "confirmed-duplicate-not-active",
    }


def _all_smpl_refs(results: Sequence[object]) -> set[tuple[str, str]]:
    refs: set[tuple[str, str]] = set()
    for result in results:
        container = getattr(result, "container", None)
        if container is None:
            continue
        for obj in container.objects:
            if obj.type == "SMPL":
                refs.add((obj.image, obj.object_key))
    return refs


def _smpl_refs_for_selection(
    *,
    results: Sequence[object],
    trees: Sequence[ContentTree],
    relationships: Sequence[WaveformRelationship],
    scope: TargetScope,
    selector_path: str,
    selector_object_key: str,
) -> set[tuple[str, str]]:
    if scope == "file":
        return _all_smpl_refs(results)

    by_type: dict[str, set[str]] = {"PROG": set(), "SBAC": set(), "SBNK": set(), "SMPL": set()}
    if scope == "volume":
        node = _find_selector_node(trees, scope, selector_path)
        if node is not None:
            by_type = _descendant_keys(node)
    elif scope == "program":
        by_type["PROG"].add(selector_object_key)
    elif scope == "sbac":
        by_type["SBAC"].add(selector_object_key)
    elif scope == "sbnk":
        by_type["SBNK"].add(selector_object_key)

    source_by_key: dict[str, set[str]] = {}
    for result in results:
        container = getattr(result, "container", None)
        if container is None:
            continue
        for obj in container.objects:
            source_by_key.setdefault(obj.object_key, set()).add(obj.image)

    smpl_refs: set[tuple[str, str]] = {
        (source, key) for key in by_type["SMPL"] for source in source_by_key.get(key, set())
    }
    changed = True
    while changed:
        changed = False
        for row in relationships:
            target_type = _relationship_target_type(row.relationship_type)
            if row.quality not in {"Known", "Likely"}:
                continue
            if row.relationship_type.startswith("PROG_ASSIGNMENT_TO_"):
                if row.source_key not in by_type["PROG"] or not _row_is_active_assignment(row):
                    continue
                for target in _target_keys(row.target_key):
                    if (
                        target_type in {"SBAC", "SBNK", "SMPL"}
                        and target not in by_type[target_type]
                    ):
                        by_type[target_type].add(target)
                        changed = True
            elif row.relationship_type == "SBAC_SLOT_TO_SBNK":
                if row.source_key not in by_type["SBAC"]:
                    continue
                for target in _target_keys(row.target_key):
                    if target not in by_type["SBNK"]:
                        by_type["SBNK"].add(target)
                        changed = True
            elif row.relationship_type in {"SBNK_LEFT_MEMBER_TO_SMPL", "SBNK_RIGHT_MEMBER_TO_SMPL"}:
                if row.source_key not in by_type["SBNK"]:
                    continue
                for target in _target_keys(row.target_key):
                    ref = (row.source_image, target)
                    if ref not in smpl_refs:
                        smpl_refs.add(ref)
                        changed = True
    return smpl_refs


def _decode_selected_waveforms(
    results: Sequence[object],
    selected_refs: set[tuple[str, str]],
    progress: _ProgressLine,
) -> tuple[list[Any], int]:
    objects: list[Any] = []
    for result in results:
        container = getattr(result, "container", None)
        if container is None:
            continue
        objects.extend(
            obj
            for obj in container.objects
            if obj.type == "SMPL" and (obj.image, obj.object_key) in selected_refs
        )
    waveforms: list[Any] = []
    decode_errors = 0
    progress.update("reading", 0, len(objects), "reading selected waveform payloads")
    for index, obj in enumerate(objects, start=1):
        progress.update("reading", index - 1, len(objects), obj.name or obj.object_key)
        try:
            waveforms.append(decode_waveform(obj))
        except ValueError as exc:
            decode_errors += 1
            issue = WaveformIssue(
                object_key=obj.object_key,
                sample_name=obj.name,
                code="WAVEFORM_DECODE_FAILED",
                severity="error",
                message=str(exc),
            )
            if decode_errors <= 20:
                print(f"{issue.object_key}\t{issue.code}\t{issue.message}")
        progress.update("reading", index, len(objects), obj.name or obj.object_key)
    return waveforms, decode_errors


def _run_targeted_extract(
    args: argparse.Namespace,
    *,
    scope: TargetScope,
    paths: Sequence[Path],
    write_sfz_files: bool,
) -> int:
    output_dir = Path(args.output_dir)
    _ensure_output_dir(output_dir, overwrite=args.overwrite)
    selector_paths = tuple(getattr(args, "selector_paths", ()) or ())
    progress = _ProgressLine(args.progress)
    all_written_files: list[Path] = []
    all_selection_graphs: list[dict[str, object]] = []
    targeted_results: list[TargetedExportResult] = []
    decode_errors = 0
    load_errors = 0
    waveforms: list[Any] = []
    try:
        results, load_errors = _load_extract_results(paths, strict=args.strict, progress=progress)
        trees = _content_trees_for_results(results)
        selections = _selector_object_keys_from_trees(trees, scope, selector_paths)
        progress.update("resolving", 0, 3, "building placement and relationship context")
        placements, relationships = _wave_export_context(results)
        progress.update("resolving", 1, 3, "computing selected dependency closure")
        selected_refs: set[tuple[str, str]] = set()
        for selector_path, selector_key in selections:
            selected_refs.update(
                _smpl_refs_for_selection(
                    results=results,
                    trees=trees,
                    relationships=relationships,
                    scope=scope,
                    selector_path=selector_path,
                    selector_object_key=selector_key,
                )
            )
        progress.update("resolving", 2, 3, f"selected waveform objects: {len(selected_refs)}")
        waveforms, decode_errors = _decode_selected_waveforms(results, selected_refs, progress)
        progress.update("resolving", 3, 3, "building export plan")
        enriched_waveforms = _enrich_waveform_parameter_metadata(waveforms, results, relationships)
        plan = build_export_plan(
            output_dir,
            enriched_waveforms,
            layout="structured",
            placements=placements,
            relationships=relationships,
        )
        for selector_path, selector_key in selections:
            targeted = export_targeted(
                TargetedExportRequest(
                    output_dir=output_dir,
                    plan=plan,
                    scope=scope,
                    selector_path=selector_path,
                    selector_object_key=selector_key,
                    write_sfz=write_sfz_files,
                    stereo_policy=args.stereo,
                    overwrite_policy="replace" if args.overwrite else "fail",
                    progress_callback=lambda event: progress.update(
                        event.stage, event.completed, event.total, event.message
                    ),
                )
            )
            targeted_results.append(targeted)
            all_written_files.extend(targeted.written_files)
            all_selection_graphs.extend(targeted.selection_graphs)
    finally:
        progress.finish()
    sfz_count = 0
    warning_count = 0
    skipped_files: list[Path] = []
    for targeted in targeted_results:
        warning_count += len(targeted.warnings)
        skipped_files.extend(targeted.skipped_files)
        if targeted.sfz_result is not None:
            sfz_count += sum(
                1 for path in targeted.sfz_result.written_files if path.suffix == ".sfz"
            )
    print(
        f"waveforms={len(waveforms)} written_files={len(dict.fromkeys(all_written_files))} "
        f"selection_graphs={len(all_selection_graphs)} sfz_files={sfz_count} "
        f"decode_errors={decode_errors} load_errors={load_errors}"
    )
    shown_warnings = 0
    for targeted in targeted_results:
        for warning in targeted.warnings[: max(0, 20 - shown_warnings)]:
            print(f"warning: {warning}")
            shown_warnings += 1
            if shown_warnings >= 20:
                break
    if warning_count > shown_warnings:
        print(f"warning: +{warning_count - shown_warnings} more warnings")
    return 1 if load_errors or skipped_files else 0


def run_extract_wav(args: argparse.Namespace) -> int:
    paths = _input_paths(args.paths)
    return _run_targeted_extract(args, scope=args.scope, paths=paths, write_sfz_files=False)


def _write_volume_graph_schemas(
    output_dir: Path,
    sidecar_records: Sequence[object],
    extra_schemas: Sequence[ReportSchemaManifest] = (),
) -> None:
    if not sidecar_records and not extra_schemas:
        return
    schemas: list[ReportSchemaManifest] = []
    if sidecar_records:
        schemas.append(
            _write_report_schema(
                output_dir,
                "volume_graphs",
                list(sidecar_records),
                semantic_notes="Per-volume axklib object graph manifests generated from decoded container objects and relationships.",
                replacement_notes="This is the canonical graph schema for axklib extract wav file.",
            )
        )
    for report_name in (
        "samples",
        "sample_banks",
        "sample_bank_accessories",
        "programs",
        "sequences",
        "relationships",
        "unresolved",
        "stereo_decisions",
        "stereo_exports",
        "export_summary",
    ):
        report_path = output_dir / f"{report_name}.json"
        if report_path.exists():
            schemas.append(
                _write_report_schema(output_dir, report_name, _read_json_rows(report_path))
            )
    schemas.extend(extra_schemas)
    write_schema_index(_schema_dir(output_dir), schemas)


def _object_origin_columns(raw: Any) -> dict[str, object]:
    metadata = raw.metadata
    return {
        "iso_extent_sector": metadata.get("iso_extent_sector", ""),
        "iso_data_offset": metadata.get("iso_data_offset", ""),
        "iso_file_size": metadata.get("iso_file_size", ""),
        "iso_raw_group": metadata.get("iso_raw_group", ""),
        "iso_raw_volume": metadata.get("iso_raw_volume", ""),
        "iso_group_label": metadata.get("iso_group_label", ""),
        "iso_volume_label": metadata.get("iso_volume_label", ""),
        "iso_group_label_source": metadata.get("iso_group_label_source", ""),
        "iso_volume_label_source": metadata.get("iso_volume_label_source", ""),
        "iso_recovery_quality": metadata.get("iso_recovery_quality", ""),
        "fat_directory_offset": metadata.get("fat_directory_offset", ""),
        "fat_first_cluster": metadata.get("fat_first_cluster", ""),
        "fat_cluster_count": metadata.get("fat_cluster_count", ""),
        "fat_file_size": metadata.get("fat_file_size", ""),
        "fat_object_offset": metadata.get("fat_object_offset", ""),
        "fat_stored_payload_offset": metadata.get("fat_stored_payload_offset", ""),
    }


def _inventory_rows(container: axklib.containers.AxklibContainer) -> list[dict[str, object]]:
    object_set = decode_objects(container)
    rows: list[dict[str, object]] = []
    for result in object_set.results:
        raw = result.raw_object
        row: dict[str, object] = {
            "source_path": str(container.source_path),
            "container_kind": container.kind,
            "detected_format": container.detected_format,
            "scope_key": raw.container.scope_key,
            "object_key": raw.object_key,
            "partition_index": raw.ref.partition_index
            if raw.ref.partition_index is not None
            else "",
            "sfs_id": raw.ref.sfs_id if raw.ref.sfs_id is not None else "",
            "fat_file": raw.ref.fat_file,
            "payload_offset": raw.ref.payload_offset if raw.ref.payload_offset is not None else "",
            "payload_size": raw.payload_size,
            "object_type": raw.type,
            "object_name": raw.name,
            "object_format": raw.object_format.value,
            "decoded_kind": result.decoded.decoded_kind,
            "decoded_field_count": len(result.decoded.fields),
            "decoded_fields": result_field_names(result),
            "decode_issue_count": len(result.issues),
            "decode_issue_codes": result_issue_codes(result),
        }
        row.update(_object_origin_columns(raw))
        rows.append(row)
    return rows


def _issue_rows(container: axklib.containers.AxklibContainer) -> list[dict[str, object]]:
    object_set = decode_objects(container)
    rows: list[dict[str, object]] = []
    for result in object_set.results:
        raw = result.raw_object
        for issue in result.issues:
            rows.append(
                {
                    "source_path": str(container.source_path),
                    "container_kind": container.kind,
                    "object_key": raw.object_key,
                    "object_type": raw.type,
                    "object_name": raw.name,
                    "code": issue.code,
                    "severity": issue.severity,
                    "message": issue.message,
                    "byte_start": issue.byte_start if issue.byte_start is not None else "",
                    "byte_end": issue.byte_end if issue.byte_end is not None else "",
                    "quality": issue.quality.value,
                    "basis": issue.basis,
                }
            )
    return rows


def run_inventory(args: argparse.Namespace) -> int:
    output_dir = Path(args.output_dir)
    _ensure_output_dir(output_dir, overwrite=args.overwrite)
    options = axklib.OpenOptions(strict=args.strict, include_payloads=True)
    results = axklib.open_many(_input_paths(args.paths), options=options)
    all_rows: list[dict[str, object]] = []
    all_issue_rows: list[dict[str, object]] = []
    load_errors: list[dict[str, object]] = []
    summary_counts: Counter[str] = Counter()
    exit_code = 0
    for result in results:
        if result.container is None:
            exit_code = 1
            error = result.error
            load_errors.append(
                {
                    "path": str(result.path),
                    "error_code": error.error_code if error else "AXKLIB_CONTAINER_OPEN_FAILED",
                    "message": error.message if error else "unknown error",
                    "recoverable": error.recoverable if error else False,
                }
            )
            continue
        container = result.container
        rows = _inventory_rows(container)
        issue_rows = _issue_rows(container)
        all_rows.extend(rows)
        all_issue_rows.extend(issue_rows)
        summary_counts.update(str(row["object_type"]) for row in rows)

    write_dict_csv(output_dir / "inventory_objects.csv", all_rows)
    write_json(output_dir / "inventory_objects.json", all_rows)
    write_dict_csv(output_dir / "decode_issues.csv", all_issue_rows)
    write_json(output_dir / "decode_issues.json", all_issue_rows)
    inventory_summary = {
        "input_count": len(results),
        "object_count": len(all_rows),
        "decode_issue_count": len(all_issue_rows),
        "load_error_count": len(load_errors),
        "object_type_counts": dict(sorted(summary_counts.items())),
        "load_errors": load_errors,
    }
    write_json(output_dir / "inventory_summary.json", inventory_summary)
    schemas = [
        _write_report_schema(
            output_dir,
            "inventory_objects",
            all_rows,
            semantic_notes="Decoded object inventory rows produced through axklib.objects.decoded.",
        ),
        _write_report_schema(
            output_dir,
            "decode_issues",
            all_issue_rows,
            semantic_notes="Decode issues use stable code/severity/quality fields.",
        ),
        _write_report_schema(output_dir, "inventory_summary", [inventory_summary]),
    ]
    write_schema_index(_schema_dir(output_dir), schemas)
    print(
        f"objects={len(all_rows)} decode_issues={len(all_issue_rows)} load_errors={len(load_errors)}"
    )
    print(f"reports written to {output_dir}")
    return exit_code


def run_extract_sfz(args: argparse.Namespace) -> int:
    paths = _input_paths(args.paths)
    return _run_targeted_extract(args, scope=args.scope, paths=paths, write_sfz_files=True)


def run_objects(args: argparse.Namespace) -> int:
    output_dir = Path(args.output_dir)
    _ensure_output_dir(output_dir, overwrite=args.overwrite)
    options = axklib.OpenOptions(strict=args.strict, include_payloads=args.with_payloads)
    results = axklib.open_many(_input_paths(args.paths), options=options)
    rows: list[dict[str, object]] = []
    load_errors = 0
    for result in results:
        if result.container is None:
            load_errors += 1
            continue
        for row in _inventory_rows(result.container):
            if args.object_type and row["object_type"] != args.object_type:
                continue
            rows.append(row)
    rows.sort(
        key=lambda row: (
            str(row["source_path"]),
            str(row["scope_key"]),
            str(row["object_type"]),
            str(row["object_key"]),
        )
    )
    write_dict_csv(output_dir / "objects.csv", rows)
    write_json(output_dir / "objects.json", rows)
    schemas = [
        _write_report_schema(
            output_dir,
            "objects",
            rows,
            semantic_notes="Filtered object summary rows produced through the canonical inventory view.",
        )
    ]
    write_schema_index(_schema_dir(output_dir), schemas)
    print(f"objects={len(rows)} load_errors={load_errors}")
    print(f"reports written to {output_dir}")
    return 3 if load_errors else 0


def _expand_corpus_paths(paths: list[Path]) -> list[Path]:
    supported_suffixes = {".hda", ".hds", ".ima", ".img", ".iso"}
    expanded: list[Path] = []
    for path in expand_inputs(paths):
        if path.is_dir():
            expanded.extend(
                child
                for child in path.rglob("*")
                if child.is_file() and child.suffix.lower() in supported_suffixes
            )
        else:
            expanded.append(path)
    return sorted(dict.fromkeys(expanded), key=lambda item: str(item).lower())


def _input_paths(paths: Sequence[str]) -> list[Path]:
    return _expand_corpus_paths([Path(path) for path in paths])


def _input_manifest_rows(paths: Sequence[Path]) -> list[dict[str, object]]:
    return [
        {
            "path": str(path),
            "exists": path.exists(),
            "is_file": path.is_file(),
            "is_dir": path.is_dir(),
            "suffix": path.suffix.lower(),
        }
        for path in paths
    ]


def _load_error_rows(results: Sequence[object]) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for result in results:
        error = getattr(result, "error", None)
        if error is None:
            continue
        rows.append(
            {
                "path": str(getattr(result, "path", "")),
                "error_code": error.error_code,
                "message": error.message,
                "recoverable": error.recoverable,
                "original_exception": error.original_exception,
            }
        )
    return rows


def run_corpus_audit(args: argparse.Namespace) -> int:
    output_dir = Path(args.output_dir)
    _ensure_output_dir(output_dir, overwrite=args.overwrite)
    paths = _input_paths(args.paths)
    options = axklib.OpenOptions(include_payloads=True)
    results = axklib.open_many(paths, options=options)
    containers = [result.container for result in results if result.container is not None]
    load_errors = [result for result in results if result.container is None]
    inventory_rows: list[dict[str, object]] = []
    wave_smoke_issue_rows: list[dict[str, object]] = []
    wave_smoke_decoded = 0
    wave_smoke_errors = 0
    for container in containers:
        inventory_rows.extend(_inventory_rows(container))
        if not args.skip_wave_smoke:
            waveform_set = decode_container_waveforms(container)
            wave_smoke_decoded += min(len(waveform_set.waveforms), args.wave_smoke_limit)
            wave_smoke_errors += len(waveform_set.issues)
            wave_smoke_issue_rows.extend(
                {
                    "source_path": str(container.source_path),
                    "container_kind": container.kind,
                    "object_key": issue.object_key,
                    "sample_name": issue.sample_name,
                    "code": issue.code,
                    "severity": issue.severity,
                    "message": issue.message,
                }
                for issue in waveform_set.issues
            )
    validation = validate_path_results(results, policy=args.policy)
    relationship_result = build_relationship_graph_for_loaded_results(results)
    graph = relationship_result.graph
    relationship_load_errors = relationship_result.load_errors
    coverage = summarize_relationship_coverage(graph)
    input_manifest_rows = _input_manifest_rows(paths)
    corpus_summary = {
        "input_count": len(results),
        "loaded_container_count": len(containers),
        "load_error_count": len(load_errors),
        "relationship_load_error_count": len(relationship_load_errors),
        "object_count": len(inventory_rows),
        "validation_issue_count": validation.issue_count,
        "validation_failed": validation.failed,
        "relationship_count": coverage.relationship_count,
        "ambiguous_relationship_count": coverage.ambiguous_relationship_count,
        "wave_smoke_decoded": wave_smoke_decoded,
        "wave_smoke_errors": wave_smoke_errors,
    }
    write_json(output_dir / "corpus_audit_summary.json", corpus_summary)
    write_dict_csv(output_dir / "input_manifest.csv", input_manifest_rows)
    write_json(output_dir / "input_manifest.json", input_manifest_rows)
    validation_rows = list(validation.issues)
    relationship_rows = list(graph.relationships)
    write_dict_csv(output_dir / "inventory_objects.csv", inventory_rows)
    write_csv(output_dir / "validation_issues.csv", validation_rows)
    write_csv(output_dir / "relationships.csv", relationship_rows)
    write_dict_csv(output_dir / "wave_smoke_issues.csv", wave_smoke_issue_rows)
    schemas = [
        _write_report_schema(output_dir, "corpus_audit_summary", [corpus_summary]),
        _write_report_schema(output_dir, "input_manifest", input_manifest_rows),
        _write_report_schema(output_dir, "inventory_objects", inventory_rows),
        _write_report_schema(output_dir, "validation_issues", validation_rows),
        _write_report_schema(output_dir, "relationships", relationship_rows),
        _write_report_schema(output_dir, "wave_smoke_issues", wave_smoke_issue_rows),
    ]
    write_schema_index(_schema_dir(output_dir), schemas)
    print(
        f"containers={len(containers)} objects={len(inventory_rows)} validation_issues={validation.issue_count} "
        f"relationships={coverage.relationship_count} wave_smoke={wave_smoke_decoded}/{wave_smoke_errors}"
    )
    print(f"reports written to {output_dir}")
    if load_errors or relationship_load_errors:
        return 3
    return 1 if validation.failed else 0


def _safe_output_stem(path: Path, index: int) -> str:
    safe = "".join(char if char.isalnum() or char in "._-" else "_" for char in path.stem)
    return f"{index:03d}_{safe or 'image'}"


def _sfs_detail_paths(paths: Sequence[Path]) -> list[Path]:
    return [path for path in paths if path.exists() and path.suffix.lower() in {".hda", ".hds"}]


def _write_validation_detail_reports(
    output_dir: Path,
    paths: Sequence[Path],
) -> list[ReportSchemaManifest]:
    allocation_summaries: list[Any] = []
    allocation_extents: list[Any] = []
    allocation_mismatches: list[Any] = []
    volume_rows: list[Any] = []
    volume_issue_rows: list[Any] = []
    volume_summary_rows: list[Any] = []
    intermediate_root = output_dir / "_intermediate"

    for index, image in enumerate(_sfs_detail_paths(paths)):
        stem = _safe_output_stem(image, index)
        allocation_dir = intermediate_root / stem / "allocation"
        inventory_dir = intermediate_root / stem / "inventory"
        volume_dir = intermediate_root / stem / "volume"
        try:
            allocation_validation.build_report(image, allocation_dir)
            sfs_inventory.build_inventory(image, inventory_dir)
            rows, issues, summary = volume_validation.build_report(
                inventory_dir=inventory_dir,
                allocation_dir=allocation_dir,
                output_dir=volume_dir,
            )
        except Exception as exc:
            write_json(
                output_dir / f"{stem}_detail_error.json",
                {"source_path": str(image), "error": str(exc)},
            )
            continue
        allocation_report = (
            allocation_validation.analyze_image(image)
            if hasattr(allocation_validation, "analyze_image")
            else None
        )
        if allocation_report is not None:
            allocation_summaries.extend(allocation_report.summaries)
            allocation_extents.extend(allocation_report.extents)
            allocation_mismatches.extend(allocation_report.mismatches)
        volume_rows.extend(rows)
        volume_issue_rows.extend(issues)
        volume_summary_rows.append(summary)

    schemas: list[ReportSchemaManifest] = []
    if allocation_summaries or allocation_extents or allocation_mismatches:
        write_csv(output_dir / "allocation_summary.csv", allocation_summaries)
        write_rows_json(output_dir / "allocation_summary.json", allocation_summaries)
        write_csv(output_dir / "allocation_extents.csv", allocation_extents)
        write_rows_json(output_dir / "allocation_extents.json", allocation_extents)
        write_csv(output_dir / "allocation_mismatches.csv", allocation_mismatches)
        write_rows_json(output_dir / "allocation_mismatches.json", allocation_mismatches)
        schemas.extend(
            [
                _write_report_schema(output_dir, "allocation_summary", allocation_summaries),
                _write_report_schema(output_dir, "allocation_extents", allocation_extents),
                _write_report_schema(output_dir, "allocation_mismatches", allocation_mismatches),
            ]
        )
    if volume_rows or volume_issue_rows or volume_summary_rows:
        write_csv(output_dir / "volume_validation.csv", volume_rows)
        write_rows_json(output_dir / "volume_validation.json", volume_rows)
        write_csv(output_dir / "volume_validation_issues.csv", volume_issue_rows)
        write_rows_json(output_dir / "volume_validation_issues.json", volume_issue_rows)
        write_csv(output_dir / "volume_validation_summary.csv", volume_summary_rows)
        write_rows_json(output_dir / "volume_validation_summary.json", volume_summary_rows)
        schemas.extend(
            [
                _write_report_schema(output_dir, "volume_validation", volume_rows),
                _write_report_schema(output_dir, "volume_validation_issues", volume_issue_rows),
                _write_report_schema(output_dir, "volume_validation_summary", volume_summary_rows),
            ]
        )
    return schemas


def run_validate(args: argparse.Namespace) -> int:
    policy = "strict" if args.strict else args.policy
    output_dir = Path(args.output_dir)
    _ensure_output_dir(output_dir, overwrite=args.overwrite)
    input_paths: list[Path] = []
    if args.exports:
        report = validate_export_sidecars(Path(args.exports), policy=policy)
    else:
        if not args.paths:
            raise ValueError("validate requires input paths unless --exports is supplied")
        input_paths = _input_paths(args.paths)
        report = validate_paths(input_paths, policy=policy)
    rows = list(report.issues)
    write_csv(output_dir / "validation_issues.csv", rows)
    write_rows_json(output_dir / "validation_issues.json", rows)
    validation_summary = {
        "policy": policy,
        "failed": report.failed,
        "issue_count": report.issue_count,
        "summary_counts": report.summary_counts,
    }
    write_json(output_dir / "validation_summary.json", validation_summary)
    schemas = [
        _write_report_schema(
            output_dir,
            "validation_issues",
            rows,
            semantic_notes="Validation issues use stable issue codes intended for regression and CI gates.",
        ),
        _write_report_schema(output_dir, "validation_summary", [validation_summary]),
    ]
    if input_paths:
        schemas.extend(_write_validation_detail_reports(output_dir, input_paths))
    write_schema_index(_schema_dir(output_dir), schemas)
    print(f"issues={report.issue_count} failed={report.failed} policy={policy}")
    print(f"reports written to {output_dir}")
    return 1 if report.failed else 0


def _write_relationship_graph(output_dir: Path, graph: Any) -> None:
    _ensure_output_dir(output_dir, overwrite=True)
    write_csv(output_dir / "relationships.csv", list(graph.relationships))
    write_rows_json(output_dir / "relationships.json", list(graph.relationships))
    write_csv(output_dir / "current_sbac_sbnk_links.csv", list(graph.sbac_sbnk_rows))
    write_rows_json(output_dir / "current_sbac_sbnk_links.json", list(graph.sbac_sbnk_rows))
    write_csv(output_dir / "current_prog_bank_links.csv", list(graph.prog_bank_rows))
    write_rows_json(output_dir / "current_prog_bank_links.json", list(graph.prog_bank_rows))
    write_csv(
        output_dir / "current_prog_ignored_reserved_or_tail.csv", list(graph.prog_ignored_rows)
    )
    write_rows_json(
        output_dir / "current_prog_ignored_reserved_or_tail.json", list(graph.prog_ignored_rows)
    )
    write_csv(
        output_dir / "current_sbnk_program_bitmap_crosscheck.csv", list(graph.sbnk_bitmap_rows)
    )
    write_rows_json(
        output_dir / "current_sbnk_program_bitmap_crosscheck.json", list(graph.sbnk_bitmap_rows)
    )
    schemas = [
        _write_report_schema(output_dir, "relationships", list(graph.relationships)),
        _write_report_schema(output_dir, "current_sbac_sbnk_links", list(graph.sbac_sbnk_rows)),
        _write_report_schema(output_dir, "current_prog_bank_links", list(graph.prog_bank_rows)),
        _write_report_schema(
            output_dir, "current_prog_ignored_reserved_or_tail", list(graph.prog_ignored_rows)
        ),
        _write_report_schema(
            output_dir, "current_sbnk_program_bitmap_crosscheck", list(graph.sbnk_bitmap_rows)
        ),
    ]
    write_schema_index(_schema_dir(output_dir), schemas)


def run_relationships(args: argparse.Namespace) -> int:
    output_dir = Path(args.output_dir)
    _ensure_output_dir(output_dir, overwrite=args.overwrite)
    input_paths = _input_paths(args.paths)
    result = build_relationship_graph_for_path_results(input_paths, container="auto")
    graph = result.graph
    _write_relationship_graph(output_dir, graph)
    sbnk_schema: ReportSchemaManifest | None = None
    if args.mono_dir:
        sbnk_rows: list[sbnk_links.SbnkLink] = []
        for image in input_paths:
            if image.exists() and image.suffix.lower() in {".hda", ".hds"}:
                sbnk_rows.extend(sbnk_links.build_sbnk_links(image, Path(args.mono_dir)))
        sbnk_links.write_report(output_dir, sbnk_rows)
        sbnk_schema = _write_report_schema(
            output_dir,
            "sbnk_links",
            sbnk_rows,
            semantic_notes="Detailed current SBNK-to-SMPL rows formerly emitted by the removed SBNK-link wrapper.",
            replacement_notes="Canonical public workflow is axklib relationships --mono-dir.",
        )

    load_error_rows = _load_error_rows(result.load_errors)
    write_dict_csv(output_dir / "load_errors.csv", load_error_rows)
    write_json(output_dir / "load_errors.json", load_error_rows)
    summary = summarize_relationship_coverage(graph)
    summary_payload = {
        **summary.__dict__,
        "load_error_count": len(load_error_rows),
    }
    write_json(output_dir / "relationship_summary.json", summary_payload)
    summary_schema = _write_report_schema(output_dir, "relationship_summary", [summary_payload])
    base_schemas = [
        _write_report_schema(output_dir, "relationships", list(graph.relationships)),
        _write_report_schema(output_dir, "current_sbac_sbnk_links", list(graph.sbac_sbnk_rows)),
        _write_report_schema(output_dir, "current_prog_bank_links", list(graph.prog_bank_rows)),
        _write_report_schema(
            output_dir, "current_prog_ignored_reserved_or_tail", list(graph.prog_ignored_rows)
        ),
        _write_report_schema(
            output_dir, "current_sbnk_program_bitmap_crosscheck", list(graph.sbnk_bitmap_rows)
        ),
        _write_report_schema(output_dir, "load_errors", load_error_rows),
        summary_schema,
    ]
    if sbnk_schema is not None:
        base_schemas.append(sbnk_schema)
    write_schema_index(_schema_dir(output_dir), base_schemas)
    print(
        f"relationships={len(graph.relationships)} ambiguous={len(graph.ambiguous())} "
        f"load_errors={len(load_error_rows)}"
    )
    print(f"reports written to {output_dir}")
    return 3 if load_error_rows else 0


def run_coverage(args: argparse.Namespace) -> int:
    output_dir = Path(args.output_dir)
    _ensure_output_dir(output_dir, overwrite=args.overwrite)
    input_paths = _input_paths(args.paths)
    result = build_relationship_graph_for_path_results(input_paths, container="auto")
    graph = result.graph
    summary = summarize_relationship_coverage(graph)
    relationship_rows = list(graph.relationships)
    load_error_rows = _load_error_rows(result.load_errors)
    summary_payload = {
        **summary.__dict__,
        "load_error_count": len(load_error_rows),
    }
    write_json(output_dir / "coverage_summary.json", summary_payload)
    write_csv(output_dir / "coverage_summary.csv", [summary_payload])
    write_csv(output_dir / "relationships.csv", relationship_rows)
    write_dict_csv(output_dir / "load_errors.csv", load_error_rows)
    write_json(output_dir / "load_errors.json", load_error_rows)
    schemas = [
        _write_report_schema(output_dir, "coverage_summary", [summary_payload]),
        _write_report_schema(output_dir, "relationships", relationship_rows),
        _write_report_schema(output_dir, "load_errors", load_error_rows),
    ]
    write_schema_index(_schema_dir(output_dir), schemas)
    print(
        f"relationships={summary.relationship_count} "
        f"ambiguous={summary.ambiguous_relationship_count} load_errors={len(load_error_rows)}"
    )
    print(f"reports written to {output_dir}")
    return 3 if load_error_rows else 0


def run_create_hds(args: argparse.Namespace) -> int:
    try:
        manifest = load_hds_build_manifest(Path(args.manifest))
        result = build_hds_from_manifest(
            manifest,
            Path(args.output),
            overwrite=bool(args.overwrite),
        )
    except FileExistsError:
        raise
    except (ValueError, json.JSONDecodeError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(
        f"image={result.path} size_bytes={result.size_bytes} "
        f"partitions={result.partitions} objects={len(result.objects)} "
        f"unused_tail_sectors={result.unused_tail_sectors}"
    )
    for layout in result.partition_layouts:
        print(
            f"partition={layout.index} name={layout.name!r} "
            f"start_sector={layout.start_sector} sector_count={layout.sector_count} "
            f"cluster_count={layout.cluster_count} "
            f"free_kib={layout.sampler_visible_free_kib}"
        )
    for report in result.audio_imports:
        actions = ["split-stereo"] if report.split_stereo else []
        if report.resampled:
            actions.append("resampled")
        if report.quantized:
            actions.append("quantized-16bit")
        if not actions:
            actions.append("pcm16-passthrough")
        print(
            f"audio_import={report.source_path} partition={report.partition_index} "
            f"volume={report.volume_name!r} format={report.source_format}/{report.source_subtype} "
            f"channels={report.source_channels} rate={report.source_sample_rate}->"
            f"{report.output_sample_rate} frames={report.output_frames} "
            f"actions={','.join(actions)} waveforms={','.join(report.waveform_names)!r} "
            f"clipped_samples={report.clipped_samples}"
        )
    for warning in result.warnings:
        print(f"warning: {warning}", file=sys.stderr)
    return 0


def run_alter_hds(args: argparse.Namespace) -> int:
    """Plan or apply an ordered HDS alteration transaction."""
    try:
        manifest = load_alteration_manifest(args.transaction)
        result = alter_hds(args.source, manifest, output_path=args.output)
    except (FileExistsError, OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(
        json.dumps(
            {
                "source_path": str(result.source_path),
                "output_path": str(result.output_path) if result.output_path else None,
                "applied": result.applied,
                "operations": [to_plain(item) for item in result.operations],
            },
            indent=2,
        )
    )
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="axklib",
        description="Unified Yamaha A-series disk/object tooling.",
    )
    parser.add_argument(
        "--version",
        action="version",
        version=f"%(prog)s {VERSION}",
    )
    parser.add_argument(
        "--debug", action="store_true", help="show traceback for unexpected internal errors"
    )

    subparsers = parser.add_subparsers(dest="command", metavar="command")
    info = subparsers.add_parser("info", help="summarize supported axklib containers")
    info.add_argument("paths", nargs="+", help="input files, directories, or glob patterns")
    info.add_argument("--strict", action="store_true", help="raise on the first load error")
    info.add_argument(
        "--format",
        choices=("tree", "json", "summary", "paths"),
        default="tree",
        help="output format; tree is the default contents view",
    )
    info.add_argument("--max-depth", type=int, help="maximum tree depth for text output")
    info.add_argument(
        "--show-quality", action="store_true", help="show quality labels on all tree nodes"
    )
    info.add_argument(
        "--show-unresolved",
        action="store_true",
        help="show unresolved/candidate notes when available",
    )
    info.add_argument(
        "--show-default-programs",
        action="store_true",
        help="show empty default program slots in tree and JSON output",
    )
    info.set_defaults(func=run_info)

    inventory = subparsers.add_parser(
        "inventory",
        help="decode object inventory through the axklib model",
        description="Decode object inventory through the axklib model.",
    )
    inventory.add_argument("paths", nargs="+", help="input files, directories, or glob patterns")
    inventory.add_argument(
        "-o", "--output-dir", required=True, help="directory for CSV/JSON inventory reports"
    )
    inventory.add_argument("--strict", action="store_true", help="raise on the first load error")
    inventory.add_argument(
        "--overwrite", action="store_true", help="allow writing into a non-empty output directory"
    )
    inventory.set_defaults(func=run_inventory)

    validate = subparsers.add_parser(
        "validate", help="validate containers and decoded object relationships"
    )
    validate.add_argument("paths", nargs="*", help="input files, directories, or glob patterns")
    validate.add_argument("--exports", help="validate WAV/JSON sidecars under an export directory")
    validate.add_argument(
        "-o", "--output-dir", required=True, help="directory for validation CSV/JSON reports"
    )
    validate.add_argument("--policy", choices=sorted(VALIDATION_POLICIES), default="normal")
    validate.add_argument("--strict", action="store_true", help="alias for --policy strict")
    validate.add_argument(
        "--overwrite", action="store_true", help="allow writing into a non-empty output directory"
    )
    validate.set_defaults(func=run_validate)

    relationships = subparsers.add_parser(
        "relationships", help="build current object relationship graph"
    )
    relationships.add_argument(
        "paths", nargs="+", help="input files, directories, or glob patterns"
    )
    relationships.add_argument(
        "-o", "--output-dir", required=True, help="directory for relationship CSV/JSON reports"
    )
    relationships.add_argument(
        "--overwrite", action="store_true", help="allow writing into a non-empty output directory"
    )
    relationships.add_argument(
        "--mono-dir",
        help="mono exact-export sidecar directory for detailed SBNK-link parity reports",
    )
    relationships.set_defaults(func=run_relationships)

    coverage = subparsers.add_parser("coverage", help="summarize current relationship coverage")
    coverage.add_argument("paths", nargs="+", help="input files, directories, or glob patterns")
    coverage.add_argument(
        "-o", "--output-dir", required=True, help="directory for coverage CSV/JSON reports"
    )
    coverage.add_argument(
        "--overwrite", action="store_true", help="allow writing into a non-empty output directory"
    )
    coverage.set_defaults(func=run_coverage)

    create = subparsers.add_parser("create", help="create a fresh supported container")
    create_subparsers = create.add_subparsers(dest="create_command", metavar="create-command")
    create_hds = create_subparsers.add_parser(
        "hds",
        help="create a fresh HDS image from a versioned JSON manifest",
    )
    create_hds.add_argument("manifest", help="versioned HDS build manifest JSON")
    create_hds.add_argument("-o", "--output", required=True, help="output HDS image path")
    create_hds.add_argument("--overwrite", action="store_true", help="replace an existing image")
    create_hds.set_defaults(func=run_create_hds)

    alter = subparsers.add_parser("alter", help="alter an existing supported container")
    alter_subparsers = alter.add_subparsers(dest="alter_command", metavar="alter-command")
    alter_hds_parser = alter_subparsers.add_parser(
        "hds",
        help="plan or apply an ordered HDS alteration transaction",
    )
    alter_hds_parser.add_argument("source", help="existing source HDS image")
    alter_hds_parser.add_argument("transaction", help="versioned alteration transaction JSON")
    alter_hds_parser.add_argument(
        "-o",
        "--output",
        help="new output HDS path; omit to validate and report a dry run",
    )
    alter_hds_parser.set_defaults(func=run_alter_hds)

    objects_cmd = subparsers.add_parser("objects", help="inspect raw and decoded object summaries")
    objects_cmd.add_argument("paths", nargs="+", help="input files, directories, or glob patterns")
    objects_cmd.add_argument(
        "-o", "--output-dir", required=True, help="directory for object reports"
    )
    objects_cmd.add_argument(
        "--object-type", choices=("SMPL", "SBNK", "SBAC", "PROG", "SEQU", "PRF3")
    )
    objects_cmd.add_argument(
        "--with-payloads",
        action="store_true",
        help="load payloads and decoded fields instead of metadata summaries",
    )
    objects_cmd.add_argument("--strict", action="store_true", help="raise on the first load error")
    objects_cmd.add_argument(
        "--overwrite", action="store_true", help="allow writing into a non-empty output directory"
    )
    objects_cmd.set_defaults(func=run_objects)

    corpus = subparsers.add_parser("corpus", help="run corpus-level workflows")
    corpus_subparsers = corpus.add_subparsers(dest="corpus_command", metavar="corpus-command")
    audit = corpus_subparsers.add_parser(
        "audit", help="run inventory, validation, relationship, and waveform smoke checks"
    )
    audit.add_argument("paths", nargs="+", help="input files, directories, or glob patterns")
    audit.add_argument(
        "-o", "--output-dir", required=True, help="directory for corpus audit reports"
    )
    audit.add_argument("--policy", choices=sorted(VALIDATION_POLICIES), default="normal")
    audit.add_argument("--wave-smoke-limit", type=int, default=10)
    audit.add_argument("--skip-wave-smoke", action="store_true")
    audit.add_argument(
        "--overwrite", action="store_true", help="allow writing into a non-empty output directory"
    )
    audit.set_defaults(func=run_corpus_audit)

    extract = subparsers.add_parser("extract", help="extract data from supported axklib containers")
    extract_subparsers = extract.add_subparsers(dest="extract_command", metavar="extract-command")
    wav = extract_subparsers.add_parser(
        "wav",
        help="export targeted WAVs to a shared sample pool",
        description="Export targeted WAVs to a shared sample pool.",
    )
    wav.add_argument("scope", choices=sorted(_TARGET_SCOPES), help="selection scope")
    wav.add_argument("paths", nargs="+", help="input files, directories, or glob patterns")
    wav.add_argument("-o", "--output-dir", required=True, help="directory for targeted exports")
    wav.add_argument(
        "--path",
        action="append",
        dest="selector_paths",
        default=[],
        help="selector path copied from `axklib info --format paths`; repeatable; required except for file scope",
    )
    wav.add_argument(
        "--stereo",
        choices=("none", "auto"),
        default="auto",
        help="stereo export policy; default is auto",
    )
    wav.add_argument("--overwrite", action="store_true", help="replace existing export files")
    wav.add_argument("--strict", action="store_true", help="raise on the first load error")
    wav.add_argument(
        "--progress",
        choices=("auto", "always", "never"),
        default="auto",
        help="show same-line progress on stderr; default is auto for interactive terminals",
    )
    wav.set_defaults(func=run_extract_wav)

    sfz = extract_subparsers.add_parser(
        "sfz",
        help="export targeted WAVs and generate SFZ files",
        description="Export targeted WAVs to a shared sample pool and generate SFZ files.",
    )
    sfz.add_argument("scope", choices=sorted(_TARGET_SCOPES), help="selection scope")
    sfz.add_argument("paths", nargs="+", help="input files, directories, or glob patterns")
    sfz.add_argument("-o", "--output-dir", required=True, help="directory for WAV/SFZ exports")
    sfz.add_argument(
        "--stereo",
        choices=("none", "auto"),
        default="auto",
        help="stereo export policy; default is auto",
    )
    sfz.add_argument("--overwrite", action="store_true", help="replace existing export files")
    sfz.add_argument(
        "--path",
        action="append",
        dest="selector_paths",
        default=[],
        help="selector path copied from `axklib info --format paths`; repeatable; required except for file scope",
    )
    sfz.add_argument("--strict", action="store_true", help="raise on the first load error")
    sfz.add_argument(
        "--progress",
        choices=("auto", "always", "never"),
        default="auto",
        help="show same-line progress on stderr; default is auto for interactive terminals",
    )
    sfz.set_defaults(func=run_extract_sfz)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if not hasattr(args, "func"):
        parser.print_help()
        return 0
    try:
        return int(args.func(args))
    except FileExistsError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except BrokenPipeError:
        return 0
    except Exception as exc:
        print(f"internal error: {exc}", file=sys.stderr)
        if getattr(args, "debug", False):
            traceback.print_exc()
        return 4


if __name__ == "__main__":
    raise SystemExit(main())
