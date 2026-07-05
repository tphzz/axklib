"""Validation services for axklib containers and decoded objects."""

from __future__ import annotations

import json
import tempfile
import wave
from collections import Counter, defaultdict
from collections.abc import Sequence
from dataclasses import dataclass
from enum import StrEnum
from pathlib import Path

from axklib.containers import AxklibContainer, AxklibContainerLoadResult, OpenOptions, open_many
from axklib.model import AxklibObject, DataQuality
from axklib.objects import current as object_current
from axklib.objects.decoded import decode_object
from axklib.relationships import (
    ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE,
    ACTIVE_ASSIGNMENT_STATE_SOURCE_LOAD_ASSIGNMENT,
    Relationship,
    RelationshipGraph,
    build_relationship_graph,
)


class ValidationSeverity(StrEnum):
    """Severity level for validation issues.

    Use it to decide whether a validation policy should fail, warn, or merely record informational quality."""

    INFO = "info"
    WARNING = "warning"
    ERROR = "error"
    FATAL = "fatal"


class ValidationScope(StrEnum):
    """Part of the system affected by a validation issue.

    Use it to separate container, object, relationship, export, and sidecar problems in reports and CLI output."""

    CONTAINER = "container"
    PARTITION = "partition"
    VOLUME = "volume"
    CATEGORY = "category"
    OBJECT = "object"
    RELATIONSHIP = "relationship"
    EXPORT = "export"
    SIDECAR = "sidecar"


@dataclass(frozen=True)
class ValidationIssue:
    """One stable validation finding.

    Use it for machine-readable validation output with a stable code, severity, source path, sampler path, quality, and recommended follow-up."""

    severity: ValidationSeverity
    code: str
    message: str
    scope: ValidationScope
    source_path: str
    sampler_path: str = ""
    object_key: str = ""
    quality: DataQuality = DataQuality.KNOWN
    basis: str = "validation"
    recommended_next_check: str = ""


@dataclass(frozen=True)
class ValidationReport:
    """Collection of validation issues evaluated under a policy.

    Use it to get summary counts and a deterministic pass/fail result for normal, strict, or salvage-aware validation modes."""

    issues: tuple[ValidationIssue, ...]
    policy: str = "normal"

    @property
    def issue_count(self) -> int:
        return len(self.issues)

    @property
    def failed(self) -> bool:
        return validation_failed(self.issues, self.policy)

    @property
    def summary_counts(self) -> dict[str, int]:
        return dict(sorted(Counter(issue.code for issue in self.issues).items()))


VALIDATION_POLICIES = {"normal", "strict", "salvage-aware"}


def validation_failed(
    issues: tuple[ValidationIssue, ...] | list[ValidationIssue], policy: str
) -> bool:
    if policy not in VALIDATION_POLICIES:
        raise ValueError(f"unknown validation policy: {policy}")
    if policy == "strict":
        return any(
            issue.severity
            in {ValidationSeverity.WARNING, ValidationSeverity.ERROR, ValidationSeverity.FATAL}
            for issue in issues
        )
    return any(
        issue.severity in {ValidationSeverity.ERROR, ValidationSeverity.FATAL} for issue in issues
    )


def _issue(
    *,
    severity: ValidationSeverity,
    code: str,
    message: str,
    scope: ValidationScope,
    source_path: str,
    sampler_path: str = "",
    object_key: str = "",
    quality: DataQuality = DataQuality.KNOWN,
    basis: str = "validation",
    recommended_next_check: str = "",
) -> ValidationIssue:
    return ValidationIssue(
        severity=severity,
        code=code,
        message=message,
        scope=scope,
        source_path=source_path,
        sampler_path=sampler_path,
        object_key=object_key,
        quality=quality,
        basis=basis,
        recommended_next_check=recommended_next_check,
    )


_ACTIVE_PROGRAM_ASSIGNMENT_STATES = {
    ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE,
    ACTIVE_ASSIGNMENT_STATE_SOURCE_LOAD_ASSIGNMENT,
}
_SBNK_MEMBER_RELATIONSHIP_TYPES = {
    "SBNK_LEFT_MEMBER_TO_SMPL",
    "SBNK_RIGHT_MEMBER_TO_SMPL",
}


def _relationship_target_keys(target_key: str) -> tuple[str, ...]:
    return tuple(part for part in target_key.split("|") if part)


def _object_report_path(objects_by_key: dict[str, AxklibObject], object_key: str) -> str:
    item = objects_by_key.get(object_key)
    if item is None:
        return object_key
    return item.fat_file or item.object_key


def _logical_object_group_path(objects_by_key: dict[str, AxklibObject], object_key: str) -> str:
    item = objects_by_key.get(object_key)
    if item is None:
        return object_key
    path = item.fat_file.replace("\\", "/")
    parts = path.split("/")
    if len(parts) >= 3 and parts[-2] in {"PROG", "SBAC", "SBNK", "SMPL", "SEQU", "PRF3"}:
        return "/".join(parts[:-2])
    return path or item.scope_key or item.object_key


def _active_program_assignment_label(
    objects_by_key: dict[str, AxklibObject], row: Relationship
) -> str:
    program_path = _object_report_path(objects_by_key, row.source_key)
    assignment_name = row.assignment_name or row.target_key or "unnamed assignment"
    if row.assignment_index is None:
        return f"{program_path}: {assignment_name}"
    return f"{program_path}: assignment {row.assignment_index + 1} {assignment_name}"


def _sbnk_member_target_issues(
    container: AxklibContainer, graph: RelationshipGraph, source_path: str
) -> tuple[list[ValidationIssue], set[str]]:
    objects_by_key = {item.object_key: item for item in container.objects}
    sbac_to_sbnk: dict[str, set[str]] = defaultdict(set)
    reachable_from_active_program: dict[str, list[str]] = defaultdict(list)

    for row in graph.relationships:
        if row.relationship_type != "SBAC_SLOT_TO_SBNK":
            continue
        if row.quality not in {"Known", "Likely"}:
            continue
        for target_key in _relationship_target_keys(row.target_key):
            sbac_to_sbnk[row.source_key].add(target_key)

    for row in graph.relationships:
        if row.relationship_type not in {"PROG_ASSIGNMENT_TO_SBAC", "PROG_ASSIGNMENT_TO_SBNK"}:
            continue
        if row.active_assignment_state not in _ACTIVE_PROGRAM_ASSIGNMENT_STATES:
            continue
        if row.quality not in {"Known", "Likely"}:
            continue
        label = _active_program_assignment_label(objects_by_key, row)
        for target_key in _relationship_target_keys(row.target_key):
            target_item = objects_by_key.get(target_key)
            target_is_sbnk = target_item is not None and target_item.type == "SBNK"
            if row.relationship_type == "PROG_ASSIGNMENT_TO_SBNK" or target_is_sbnk:
                reachable_from_active_program[target_key].append(label)
            else:
                for sbnk_key in sbac_to_sbnk.get(target_key, ()):
                    reachable_from_active_program[sbnk_key].append(label)

    grouped_rows: dict[tuple[str, bool], list[Relationship]] = defaultdict(list)
    grouped_active_labels: dict[tuple[str, bool], set[str]] = defaultdict(set)
    covered_relationship_keys: set[str] = set()
    for row in graph.relationships:
        if row.relationship_type not in _SBNK_MEMBER_RELATIONSHIP_TYPES or row.quality != "Unknown":
            continue
        active_labels = reachable_from_active_program.get(row.source_key, [])
        group_path = _logical_object_group_path(objects_by_key, row.source_key)
        group_key = (group_path, bool(active_labels))
        grouped_rows[group_key].append(row)
        grouped_active_labels[group_key].update(active_labels)
        covered_relationship_keys.add(row.key)

    issues: list[ValidationIssue] = []
    for (group_path, is_active_reachable), rows in sorted(grouped_rows.items()):
        source_objects = {row.source_key for row in rows}
        member_count = len(rows)
        sbnk_count = len(source_objects)
        first_source_object = sorted(source_objects)[0]
        if is_active_reachable:
            active_labels = sorted(grouped_active_labels[(group_path, is_active_reachable)])
            active_summary = "; ".join(active_labels[:4])
            if len(active_labels) > 4:
                active_summary += f"; +{len(active_labels) - 4} more"
            issues.append(
                _issue(
                    severity=ValidationSeverity.ERROR,
                    code="REL_ACTIVE_PROGRAM_SBNK_MEMBER_TARGET_MISSING",
                    message=(
                        f"{member_count} sample-bank member link(s) across {sbnk_count} "
                        "sample bank(s) do not resolve to physical sample objects and are "
                        "reachable from active Program assignments."
                    ),
                    scope=ValidationScope.RELATIONSHIP,
                    source_path=source_path,
                    sampler_path=f"{group_path} | {active_summary}",
                    object_key=first_source_object,
                    quality=DataQuality.UNKNOWN,
                    basis="SBNK member target aggregation",
                    recommended_next_check=(
                        "Treat the affected Program/sample-bank path as incomplete until the "
                        "missing physical sample objects are found or the source is confirmed "
                        "partially loadable."
                    ),
                )
            )
        else:
            issues.append(
                _issue(
                    severity=ValidationSeverity.WARNING,
                    code="REL_SBNK_MEMBER_TARGET_MISSING",
                    message=(
                        f"{member_count} sample-bank member link(s) across {sbnk_count} "
                        "sample bank(s) do not resolve to physical sample objects."
                    ),
                    scope=ValidationScope.RELATIONSHIP,
                    source_path=source_path,
                    sampler_path=group_path,
                    object_key=first_source_object,
                    quality=DataQuality.UNKNOWN,
                    basis="SBNK member target aggregation",
                    recommended_next_check=(
                        "Inspect the sample-bank member links before treating this object group "
                        "as complete."
                    ),
                )
            )
    return issues, covered_relationship_keys


def validate_container(container: AxklibContainer, *, policy: str = "normal") -> ValidationReport:
    issues: list[ValidationIssue] = []
    source_path = str(container.source_path)
    for item in container.objects:
        if item.payload and not item.payload.startswith(object_current.OBJECT_MAGIC):
            issues.append(
                _issue(
                    severity=ValidationSeverity.ERROR,
                    code="OBJECT_BAD_MAGIC",
                    message="Object payload does not start with FSFSDEV3SPLX magic.",
                    scope=ValidationScope.OBJECT,
                    source_path=source_path,
                    object_key=item.object_key,
                )
            )
        if (
            item.payload
            and item.payload.startswith(object_current.OBJECT_MAGIC)
            and len(item.payload) >= 0x24
        ):
            header_size = (
                item.header.header_size
                if item.header
                else int.from_bytes(item.payload[0x10:0x14], "big")
            )
            stored_size = (
                item.header.stored_payload_size
                if item.header
                else int.from_bytes(item.payload[0x1C:0x20], "big")
            )
            required = (header_size or 0) + (stored_size or 0)
            if required > len(item.payload):
                issues.append(
                    _issue(
                        severity=ValidationSeverity.ERROR,
                        code="OBJECT_PAYLOAD_TRUNCATED",
                        message=f"Object header requires {required} bytes but payload has {len(item.payload)} bytes.",
                        scope=ValidationScope.OBJECT,
                        source_path=source_path,
                        object_key=item.object_key,
                    )
                )
        result = decode_object(item)
        for decode_issue in result.issues:
            severity = (
                ValidationSeverity.ERROR
                if decode_issue.severity == "error"
                else ValidationSeverity.WARNING
                if decode_issue.severity == "warning"
                else ValidationSeverity.INFO
            )
            issues.append(
                _issue(
                    severity=severity,
                    code=decode_issue.code,
                    message=decode_issue.message,
                    scope=ValidationScope.OBJECT,
                    source_path=source_path,
                    object_key=item.object_key,
                    quality=decode_issue.quality,
                    basis=decode_issue.basis,
                )
            )
        quality = item.metadata.get("iso_recovery_quality", "")
        if quality == "raw-scan-recovered-object":
            issues.append(
                _issue(
                    severity=ValidationSeverity.INFO,
                    code="ISO_RECOVERED_RAW_SCAN_OBJECT",
                    message="ISO object was recovered by raw FSFSDEV3SPLX magic scanning.",
                    scope=ValidationScope.OBJECT,
                    source_path=source_path,
                    object_key=item.object_key,
                    basis="ISO recovery metadata",
                )
            )
        elif quality == "raw-scan-impossible-internal-capacity":
            issues.append(
                _issue(
                    severity=ValidationSeverity.WARNING,
                    code="ISO_IMPOSSIBLE_INTERNAL_CAPACITY",
                    message="Raw-scan recovered ISO object has impossible internal capacity metadata.",
                    scope=ValidationScope.OBJECT,
                    source_path=source_path,
                    object_key=item.object_key,
                    basis="ISO recovery metadata",
                    recommended_next_check="Treat as recovery artifact unless source directory quality proves object extent.",
                )
            )
    graph = build_relationship_graph(list(container.objects))
    member_target_issues, aggregated_member_relationship_keys = _sbnk_member_target_issues(
        container, graph, source_path
    )
    issues.extend(member_target_issues)
    for row in graph.relationships:
        if row.key in aggregated_member_relationship_keys:
            continue
        if row.quality == "Tentative":
            issues.append(
                _issue(
                    severity=ValidationSeverity.WARNING,
                    code="REL_AMBIGUOUS_TARGET",
                    message="Relationship has ambiguous candidate targets.",
                    scope=ValidationScope.RELATIONSHIP,
                    source_path=source_path,
                    object_key=row.source_key,
                    quality=DataQuality.TENTATIVE,
                    basis=row.basis,
                    recommended_next_check="Inspect candidate set before using for authoritative placement.",
                )
            )
        elif row.quality == "Unknown":
            issues.append(
                _issue(
                    severity=ValidationSeverity.WARNING,
                    code="REL_MISSING_TARGET",
                    message="Relationship target could not be resolved.",
                    scope=ValidationScope.RELATIONSHIP,
                    source_path=source_path,
                    object_key=row.source_key,
                    quality=DataQuality.UNKNOWN,
                    basis=row.basis,
                )
            )
    return ValidationReport(
        tuple(
            sorted(
                issues,
                key=lambda issue: (issue.source_path, issue.code, issue.object_key, issue.message),
            )
        ),
        policy=policy,
    )


def _quality_from_text(value: str) -> DataQuality:
    for quality in DataQuality:
        if value == quality.value:
            return quality
    return DataQuality.KNOWN


def _volume_issue_code(issue_type: str) -> str:
    return {
        "category-object-count-mismatch": "SFS_VOLUME_CATEGORY_OBJECT_COUNT_MISMATCH",
        "malformed-category-entry": "SFS_VOLUME_MALFORMED_CATEGORY_ENTRY",
        "visible-alternating-byte-compatibility-artifact-objects": "SFS_VOLUME_VISIBLE_ALTERNATING_BYTE_ARTIFACT",
        "partition-allocation-consistency": "SFS_ALLOCATION_PARTITION_CONSISTENCY",
    }.get(issue_type, "SFS_VOLUME_VALIDATION_ISSUE")


def _severity_from_text(value: str) -> ValidationSeverity:
    normalized = value.lower()
    if normalized == "fatal":
        return ValidationSeverity.FATAL
    if normalized == "error":
        return ValidationSeverity.ERROR
    if normalized == "warning":
        return ValidationSeverity.WARNING
    return ValidationSeverity.INFO


def _sfs_allocation_issues(source_path: Path) -> list[ValidationIssue]:
    from axklib.containers.sfs_allocation import analyze_image

    try:
        report = analyze_image(source_path)
    except Exception as exc:
        return [
            _issue(
                severity=ValidationSeverity.FATAL,
                code="SFS_ALLOCATION_ANALYSIS_FAILED",
                message=f"Allocation bitmap analysis failed: {exc}",
                scope=ValidationScope.PARTITION,
                source_path=str(source_path),
                basis="axklib.validation.allocation",
            )
        ]

    issues: list[ValidationIssue] = []
    for summary in report.summaries:
        sampler_path = f"partition {summary.partition_index}"
        if summary.stored_used_not_reconstructed_count:
            issues.append(
                _issue(
                    severity=ValidationSeverity.ERROR,
                    code="SFS_ALLOCATION_STORED_USED_NOT_RECONSTRUCTED",
                    message=(
                        f"Stored allocation bitmap marks {summary.stored_used_not_reconstructed_count} "
                        "clusters that reconstructed object extents do not use."
                    ),
                    scope=ValidationScope.PARTITION,
                    source_path=str(source_path),
                    sampler_path=sampler_path,
                    basis="axklib.validation.allocation",
                )
            )
        if summary.reconstructed_used_not_stored_count:
            issues.append(
                _issue(
                    severity=ValidationSeverity.ERROR,
                    code="SFS_ALLOCATION_RECONSTRUCTED_USED_NOT_STORED",
                    message=(
                        f"Reconstructed object extents use {summary.reconstructed_used_not_stored_count} "
                        "clusters not marked used in the stored allocation bitmap."
                    ),
                    scope=ValidationScope.PARTITION,
                    source_path=str(source_path),
                    sampler_path=sampler_path,
                    basis="axklib.validation.allocation",
                )
            )
        if summary.extent_total_mismatch_count:
            issues.append(
                _issue(
                    severity=ValidationSeverity.ERROR,
                    code="SFS_EXTENT_TOTAL_MISMATCH",
                    message=f"{summary.extent_total_mismatch_count} extent records disagree with stored object byte totals.",
                    scope=ValidationScope.PARTITION,
                    source_path=str(source_path),
                    sampler_path=sampler_path,
                    basis="axklib.validation.allocation",
                )
            )
        if summary.warning_count:
            issues.append(
                _issue(
                    severity=ValidationSeverity.WARNING,
                    code="SFS_ALLOCATION_WARNING",
                    message=summary.warnings
                    or f"{summary.warning_count} allocation analysis warnings.",
                    scope=ValidationScope.PARTITION,
                    source_path=str(source_path),
                    sampler_path=sampler_path,
                    basis="axklib.validation.allocation",
                )
            )
    for mismatch in report.mismatches:
        issues.append(
            _issue(
                severity=ValidationSeverity.ERROR,
                code="SFS_ALLOCATION_MISMATCH_RANGE",
                message=(
                    f"Allocation mismatch {mismatch.direction} covers clusters "
                    f"{mismatch.start_cluster}..{mismatch.end_cluster}."
                ),
                scope=ValidationScope.PARTITION,
                source_path=str(source_path),
                sampler_path=f"partition {mismatch.partition_index}",
                basis="axklib.validation.allocation",
            )
        )
    return issues


def _sfs_volume_issues(source_path: Path) -> list[ValidationIssue]:
    from axklib.containers import sfs_inventory
    from axklib.validation import volume as volume_validation

    try:
        with tempfile.TemporaryDirectory(prefix="axklib-validation-") as tmp:
            tmp_root = Path(tmp)
            inventory_dir = tmp_root / "inventory"
            volume_dir = tmp_root / "volume"
            sfs_inventory.build_inventory(source_path, inventory_dir)
            _rows, issue_rows, _summary = volume_validation.build_report(
                inventory_dir=inventory_dir,
                output_dir=volume_dir,
                allocation_dir=None,
            )
    except Exception as exc:
        return [
            _issue(
                severity=ValidationSeverity.FATAL,
                code="SFS_VOLUME_VALIDATION_FAILED",
                message=f"Volume/category validation failed: {exc}",
                scope=ValidationScope.VOLUME,
                source_path=str(source_path),
                basis="axklib.validation.volume",
            )
        ]

    issues: list[ValidationIssue] = []
    for row in issue_rows:
        issues.append(
            _issue(
                severity=_severity_from_text(row.severity),
                code=_volume_issue_code(row.issue_type),
                message=row.details or row.issue_type,
                scope=ValidationScope.VOLUME,
                source_path=str(source_path),
                sampler_path=row.volume_path,
                object_key=str(row.target_sfs_id or ""),
                quality=_quality_from_text(row.match_quality),
                basis="axklib.validation.volume",
                recommended_next_check=row.unmatched_reason,
            )
        )
    return issues


def _sfs_detail_issues(source_path: Path) -> list[ValidationIssue]:
    if source_path.suffix.lower() not in {".hda", ".hds"}:
        return []
    return [*_sfs_allocation_issues(source_path), *_sfs_volume_issues(source_path)]


def _fat_span_issue_groups(
    containers: Sequence[AxklibContainer],
) -> tuple[set[str], list[ValidationIssue]]:
    candidates: dict[
        tuple[str, str, str, str, int, int], list[tuple[AxklibContainer, AxklibObject, int]]
    ] = {}
    for container in containers:
        if container.kind != "fat12_floppy":
            continue
        for item in container.objects:
            if item.type != "SMPL" or not item.payload or item.header is None:
                continue
            header_size = item.header.header_size or 0
            stored_size = item.header.stored_payload_size or 0
            if header_size <= 0 or stored_size <= 0:
                continue
            required = header_size + stored_size
            if required <= len(item.payload):
                continue
            available_payload = max(0, len(item.payload) - header_size)
            if available_payload <= 0:
                continue
            key = (
                str(Path(container.source_path).parent),
                item.ref.fat_file,
                item.type,
                item.name,
                header_size,
                stored_size,
            )
            candidates.setdefault(key, []).append((container, item, available_payload))

    span_keys: set[str] = set()
    issues: list[ValidationIssue] = []
    for key, parts in sorted(candidates.items(), key=lambda entry: entry[0]):
        _parent, fat_file, object_type, name, _header_size, stored_size = key
        if len(parts) < 2:
            continue
        available_total = sum(part_size for _container, _item, part_size in parts)
        if available_total != stored_size:
            continue
        part_paths = ", ".join(str(container.source_path) for container, _item, _size in parts)
        for container, item, part_size in parts:
            span_keys.add(item.object_key)
            issues.append(
                _issue(
                    severity=ValidationSeverity.WARNING,
                    code="OBJECT_FLOPPY_SPANNED_PAYLOAD_PARTIAL",
                    message=(
                        f"FAT/floppy {object_type} {name!r} appears to be one part of a multi-disk "
                        f"spanned object: part payload bytes {part_size}, sibling total {available_total}, "
                        f"stored payload size {stored_size}, FAT file {fat_file}."
                    ),
                    scope=ValidationScope.OBJECT,
                    source_path=str(container.source_path),
                    object_key=item.object_key,
                    quality=DataQuality.TENTATIVE,
                    basis="sibling FAT/floppy payload-size sum",
                    recommended_next_check=f"Do not extract as complete until span reassembly is implemented. Sibling parts: {part_paths}",
                )
            )
    return span_keys, issues


def validate_path_results(
    results: Sequence[AxklibContainerLoadResult], *, policy: str = "normal"
) -> ValidationReport:
    issues: list[ValidationIssue] = []
    containers = [result.container for result in results if result.container is not None]
    fat_span_keys, fat_span_issues = _fat_span_issue_groups(containers)
    for result in results:
        if result.container is None:
            error = result.error
            issues.append(
                _issue(
                    severity=ValidationSeverity.ERROR,
                    code="CONTAINER_OPEN_FAILED",
                    message=error.message if error else "unknown container load error",
                    scope=ValidationScope.CONTAINER,
                    source_path=str(result.path),
                )
            )
            continue
        container_issues = [
            issue
            for issue in validate_container(result.container, policy=policy).issues
            if not (issue.code == "OBJECT_PAYLOAD_TRUNCATED" and issue.object_key in fat_span_keys)
        ]
        issues.extend(container_issues)
        issues.extend(_sfs_detail_issues(Path(result.path)))
    issues.extend(fat_span_issues)
    return ValidationReport(
        tuple(
            sorted(
                issues,
                key=lambda issue: (
                    issue.source_path,
                    issue.code,
                    issue.object_key,
                    issue.message,
                ),
            )
        ),
        policy=policy,
    )


def validate_paths(paths: Sequence[str | Path], *, policy: str = "normal") -> ValidationReport:
    results = open_many(paths, options=OpenOptions(include_payloads=True))
    return validate_path_results(results, policy=policy)


def _load_json_rows(path: Path) -> list[dict[str, object]]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return []
    if isinstance(payload, list):
        return [row for row in payload if isinstance(row, dict)]
    if isinstance(payload, dict):
        return [payload]
    return []


def _wav_channel_count(path: Path) -> int | None:
    try:
        with wave.open(str(path), "rb") as wav:
            return wav.getnchannels()
    except Exception:
        return None


def _stereo_decision_export_issues(export_dir: Path) -> list[ValidationIssue]:
    issues: list[ValidationIssue] = []
    decisions_path = export_dir / "stereo_decisions.json"
    if not decisions_path.exists():
        return issues
    decisions = _load_json_rows(decisions_path)
    samples = (
        _load_json_rows(export_dir / "samples.json")
        if (export_dir / "samples.json").exists()
        else []
    )
    listed_wavs = {str(row.get("wav_path", "")) for row in samples}
    for row in decisions:
        decision = str(row.get("decision", ""))
        source_path = str(decisions_path)
        sbnk_key = str(row.get("sbnk_object_key", ""))
        stereo_wav = str(row.get("stereo_wav_path", ""))
        left_mono = str(row.get("left_mono_wav_path", ""))
        right_mono = str(row.get("right_mono_wav_path", ""))
        if decision == "interleaved_stereo_written":
            if not stereo_wav:
                issues.append(
                    _issue(
                        severity=ValidationSeverity.ERROR,
                        code="EXPORT_STEREO_DECISION_MISSING_OUTPUT",
                        message="Interleaved stereo decision has no stereo_wav_path.",
                        scope=ValidationScope.EXPORT,
                        source_path=source_path,
                        object_key=sbnk_key,
                    )
                )
                continue
            wav_path = export_dir / stereo_wav
            if not wav_path.exists():
                issues.append(
                    _issue(
                        severity=ValidationSeverity.ERROR,
                        code="EXPORT_STEREO_DECISION_MISSING_OUTPUT",
                        message=f"Interleaved stereo WAV does not exist: {stereo_wav}",
                        scope=ValidationScope.EXPORT,
                        source_path=source_path,
                        object_key=sbnk_key,
                    )
                )
            elif _wav_channel_count(wav_path) != 2:
                issues.append(
                    _issue(
                        severity=ValidationSeverity.ERROR,
                        code="EXPORT_STEREO_CHANNEL_MISMATCH",
                        message=f"Interleaved stereo WAV is not two-channel: {stereo_wav}",
                        scope=ValidationScope.EXPORT,
                        source_path=str(wav_path),
                        object_key=sbnk_key,
                    )
                )
            stale = sorted(path for path in (left_mono, right_mono) if path and path in listed_wavs)
            if stale:
                issues.append(
                    _issue(
                        severity=ValidationSeverity.ERROR,
                        code="EXPORT_STEREO_SUPPRESSED_MONO_STILL_LISTED",
                        message="Suppressed mono paths are still listed in samples.json: "
                        + ", ".join(stale),
                        scope=ValidationScope.EXPORT,
                        source_path=source_path,
                        object_key=sbnk_key,
                    )
                )
        elif decision.startswith(("kept_mono", "kept_physical_only")):
            missing = [
                path
                for path in (left_mono, right_mono)
                if path and not (export_dir / path).exists()
            ]
            if missing:
                issues.append(
                    _issue(
                        severity=ValidationSeverity.ERROR,
                        code="EXPORT_STEREO_MONO_MISSING_FOR_NONEXACT",
                        message="Mono path missing for non-interleaved stereo decision: "
                        + ", ".join(missing),
                        scope=ValidationScope.EXPORT,
                        source_path=source_path,
                        object_key=sbnk_key,
                    )
                )
    schema_path = export_dir / "_schemas" / "stereo_decisions.schema.json"
    if decisions and not schema_path.exists():
        issues.append(
            _issue(
                severity=ValidationSeverity.ERROR,
                code="EXPORT_STEREO_DECISION_SCHEMA_MISSING",
                message="stereo_decisions.json exists but _schemas/stereo_decisions.schema.json is missing.",
                scope=ValidationScope.EXPORT,
                source_path=str(decisions_path),
            )
        )
    return issues


def _relative_export_path_issue(
    path_text: str, *, source_path: str, object_key: str, field_name: str
) -> ValidationIssue | None:
    rel = Path(path_text)
    if rel.is_absolute() or ".." in rel.parts:
        return _issue(
            severity=ValidationSeverity.ERROR,
            code="EXPORT_GRAPH_PATH_ESCAPE",
            message=f"Volume graph {field_name} must be relative and stay inside the volume directory.",
            scope=ValidationScope.EXPORT,
            source_path=source_path,
            object_key=object_key,
        )
    return None


def _volume_graph_wav_issues(export_dir: Path) -> list[ValidationIssue]:
    issues: list[ValidationIssue] = []
    for graph_path in sorted(export_dir.rglob("volume.axklib.json")):
        try:
            graph = json.loads(graph_path.read_text(encoding="utf-8"))
        except Exception as exc:
            issues.append(
                _issue(
                    severity=ValidationSeverity.ERROR,
                    code="EXPORT_GRAPH_BAD_JSON",
                    message=f"Volume graph JSON could not be parsed: {exc}",
                    scope=ValidationScope.EXPORT,
                    source_path=str(graph_path),
                )
            )
            continue
        if not isinstance(graph, dict) or graph.get("schema") != "axklib.volume_graph.v1":
            continue
        volume_root = graph_path.parent
        objects = graph.get("objects", {})
        smpl_rows = objects.get("smpl", []) if isinstance(objects, dict) else []
        rendered_rows = graph.get("rendered_audio", [])
        checks: list[tuple[dict[str, object], str, int | None]] = []
        if isinstance(smpl_rows, list):
            for row in smpl_rows:
                if isinstance(row, dict):
                    audio = row.get("audio", {})
                    expected_channels = (
                        int(audio.get("channels", 0))
                        if isinstance(audio, dict) and audio.get("channels") is not None
                        else None
                    )
                    checks.append((row, "wav_path", expected_channels))
        if isinstance(rendered_rows, list):
            for row in rendered_rows:
                if isinstance(row, dict):
                    channels = (
                        int(row.get("channels", 0)) if row.get("channels") is not None else None
                    )
                    checks.append((row, "wav_path", channels))
        for row, field_name, expected_channels in checks:
            object_key = str(row.get("source_ref", row.get("id", "")))
            path_text = str(row.get(field_name, ""))
            path_issue = _relative_export_path_issue(
                path_text,
                source_path=str(graph_path),
                object_key=object_key,
                field_name=field_name,
            )
            if path_issue is not None:
                issues.append(path_issue)
                continue
            wav_path = volume_root / path_text
            if not wav_path.exists():
                issues.append(
                    _issue(
                        severity=ValidationSeverity.ERROR,
                        code="EXPORT_GRAPH_WAV_MISSING",
                        message=f"Volume graph referenced WAV does not exist: {path_text}",
                        scope=ValidationScope.EXPORT,
                        source_path=str(graph_path),
                        object_key=object_key,
                    )
                )
                continue
            try:
                observed_channels = _wav_channel_count(wav_path)
            except Exception as exc:
                issues.append(
                    _issue(
                        severity=ValidationSeverity.ERROR,
                        code="EXPORT_GRAPH_WAV_BAD_HEADER",
                        message=f"Volume graph referenced WAV could not be opened: {exc}",
                        scope=ValidationScope.EXPORT,
                        source_path=str(wav_path),
                        object_key=object_key,
                    )
                )
                continue
            if expected_channels and observed_channels != expected_channels:
                issues.append(
                    _issue(
                        severity=ValidationSeverity.ERROR,
                        code="EXPORT_GRAPH_WAV_CHANNEL_MISMATCH",
                        message=f"channels graph={expected_channels} wav={observed_channels}",
                        scope=ValidationScope.EXPORT,
                        source_path=str(wav_path),
                        object_key=object_key,
                    )
                )
    return issues


def validate_export_sidecars(export_dir: Path, *, policy: str = "normal") -> ValidationReport:
    issues: list[ValidationIssue] = []
    if not export_dir.exists():
        issues.append(
            _issue(
                severity=ValidationSeverity.ERROR,
                code="EXPORT_DIR_NOT_FOUND",
                message="Export directory does not exist.",
                scope=ValidationScope.EXPORT,
                source_path=str(export_dir),
            )
        )
        return ValidationReport(tuple(issues), policy=policy)

    issues.extend(_volume_graph_wav_issues(export_dir))

    required = {
        "source_container",
        "object_key",
        "wav_path",
        "sample_rate",
        "channels",
        "sample_width_bytes",
        "frames",
        "stored_payload_size",
        "extraction_quality",
        "extraction_basis",
        "field_quality",
    }
    sidecars = sorted(
        path
        for path in export_dir.rglob("*.json")
        if "_schemas" not in path.parts and path.name not in {"schema_index.json"}
    )
    for sidecar in sidecars:
        try:
            record = json.loads(sidecar.read_text(encoding="utf-8"))
        except Exception as exc:
            issues.append(
                _issue(
                    severity=ValidationSeverity.ERROR,
                    code="EXPORT_SIDECAR_BAD_JSON",
                    message=f"Sidecar JSON could not be parsed: {exc}",
                    scope=ValidationScope.SIDECAR,
                    source_path=str(sidecar),
                )
            )
            continue
        if not isinstance(record, dict):
            continue
        if record.get("schema") == "axklib.volume_graph.v1":
            continue
        object_key = str(record.get("object_key", ""))
        missing: list[str] = []
        if record.get("schema") == "axklib.wave_sidecar.v2":
            identity = record.get("identity", {})
            audio = record.get("audio", {})
            if isinstance(identity, dict):
                object_key = str(identity.get("object_key", object_key))
            missing_sections = [
                key
                for key in (
                    "identity",
                    "audio",
                    "playback",
                    "relationships",
                    "parameters",
                    "conversion",
                    "origin",
                )
                if key not in record
            ]
            if missing_sections:
                issues.append(
                    _issue(
                        severity=ValidationSeverity.ERROR,
                        code="EXPORT_SIDECAR_MISSING_FIELD",
                        message="Sidecar missing required sections: " + ", ".join(missing_sections),
                        scope=ValidationScope.SIDECAR,
                        source_path=str(sidecar),
                        object_key=object_key,
                    )
                )
            if not isinstance(audio, dict):
                continue
            wav_path = Path(str(audio.get("wav_path", "")))
            if wav_path.is_absolute() or ".." in wav_path.parts:
                issues.append(
                    _issue(
                        severity=ValidationSeverity.ERROR,
                        code="EXPORT_SIDECAR_PATH_ESCAPE",
                        message="v2 sidecar audio.wav_path must be relative and stay inside the export root.",
                        scope=ValidationScope.SIDECAR,
                        source_path=str(sidecar),
                        object_key=object_key,
                    )
                )
                continue
            wav_path = export_dir / wav_path
            header_record = audio
        else:
            if "wav_path" not in record:
                continue
            header_record = record
            missing = sorted(key for key in required if key not in record)
        if missing:
            issues.append(
                _issue(
                    severity=ValidationSeverity.ERROR,
                    code="EXPORT_SIDECAR_MISSING_FIELD",
                    message="Sidecar missing required fields: " + ", ".join(missing),
                    scope=ValidationScope.SIDECAR,
                    source_path=str(sidecar),
                    object_key=object_key,
                )
            )
        if record.get("schema") != "axklib.wave_sidecar.v2":
            wav_path = Path(str(record.get("wav_path", "")))
            if not wav_path.is_absolute():
                cwd_relative = wav_path
                sidecar_relative = sidecar.parent / wav_path
                wav_path = cwd_relative if cwd_relative.exists() else sidecar_relative
        if not wav_path.exists():
            issues.append(
                _issue(
                    severity=ValidationSeverity.ERROR,
                    code="EXPORT_WAV_MISSING",
                    message=f"Referenced WAV does not exist: {wav_path}",
                    scope=ValidationScope.EXPORT,
                    source_path=str(sidecar),
                    object_key=object_key,
                )
            )
            continue
        try:
            with wave.open(str(wav_path), "rb") as wav:
                observed = {
                    "sample_rate": wav.getframerate(),
                    "channels": wav.getnchannels(),
                    "sample_width_bytes": wav.getsampwidth(),
                    "frames": wav.getnframes(),
                }
        except Exception as exc:
            issues.append(
                _issue(
                    severity=ValidationSeverity.ERROR,
                    code="EXPORT_WAV_BAD_HEADER",
                    message=f"Referenced WAV could not be opened: {exc}",
                    scope=ValidationScope.EXPORT,
                    source_path=str(wav_path),
                    object_key=object_key,
                )
            )
            continue
        for key, observed_value in observed.items():
            expected = header_record.get(key)
            if expected is None:
                continue
            try:
                expected_value = int(expected)
            except (TypeError, ValueError):
                continue
            if expected_value != observed_value:
                issues.append(
                    _issue(
                        severity=ValidationSeverity.ERROR,
                        code="EXPORT_WAV_HEADER_MISMATCH",
                        message=f"{key} sidecar={expected_value} wav={observed_value}",
                        scope=ValidationScope.EXPORT,
                        source_path=str(wav_path),
                        object_key=object_key,
                    )
                )
    issues.extend(_stereo_decision_export_issues(export_dir))
    return ValidationReport(
        tuple(
            sorted(
                issues,
                key=lambda issue: (issue.source_path, issue.code, issue.object_key, issue.message),
            )
        ),
        policy=policy,
    )
