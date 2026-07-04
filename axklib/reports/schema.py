"""Machine-readable report schema manifests for axklib public reports."""

from __future__ import annotations

from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from axklib.reports import row_to_dict, to_plain, write_json

SCHEMA_VERSION = "1.0"


@dataclass(frozen=True)
class ReportColumnSchema:
    """Schema description for one column in a generated report.
    
    Use it to document column name, inferred type, nullability, semantic notes, and deprecation notes for CSV/JSON outputs."""
    name: str
    type: str
    required: bool
    nullable: bool
    semantic_notes: str = ""
    deprecation_notes: str = ""


@dataclass(frozen=True)
class ReportSchemaManifest:
    """Machine-readable schema manifest for one generated report.
    
    Use it to track row counts, column schemas, quality counts, issue-code counts, object-reference columns, source command, and compatibility notes."""
    report_name: str
    schema_version: str
    row_count: int
    columns: tuple[ReportColumnSchema, ...]
    quality_counts: dict[str, int] = field(default_factory=dict)
    issue_code_counts: dict[str, int] = field(default_factory=dict)
    object_type_counts: dict[str, int] = field(default_factory=dict)
    quality_columns: tuple[str, ...] = ()
    issue_code_columns: tuple[str, ...] = ()
    object_ref_columns: tuple[str, ...] = ()
    source_command: str = ""
    library_version: str = ""
    semantic_notes: str = ""
    replacement_notes: str = ""


def _value_type(value: Any) -> str:
    if value is None or value == "":
        return "null"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, int) and not isinstance(value, bool):
        return "integer"
    if isinstance(value, float):
        return "number"
    if isinstance(value, dict):
        return "object"
    if isinstance(value, (list, tuple)):
        return "array"
    return "string"


def _combined_type(types: set[str]) -> str:
    concrete = {item for item in types if item != "null"}
    if not concrete:
        return "null"
    if concrete == {"integer", "number"}:
        return "number"
    if len(concrete) == 1:
        return next(iter(concrete))
    return "mixed"


QUALITY_COLUMN_NAMES = {
    "quality",
    "extraction_quality",
    "match_quality",
    "organization_relationship_quality",
}
ISSUE_CODE_COLUMN_NAMES = {"code", "issue_code", "decode_issue_codes"}
OBJECT_REF_COLUMN_NAMES = {
    "object_key",
    "source_key",
    "target_key",
    "partition_index",
    "sfs_id",
    "payload_offset",
    "raw_offset",
    "object_offset",
    "object_offset_hex",
    "source_path",
    "source_container",
    "image",
}


def _field_semantic_notes(name: str) -> str:
    if name in QUALITY_COLUMN_NAMES:
        return "DataQuality marker; exact allowed values are part of the quality model, not a free-form status string."
    if name == "assignment_row_state":
        return "Program assignment row classification; decoded-row means a PROG row was decoded and reported separately from active assignment state."
    if name == "active_assignment_state":
        return "Conservative active Program assignment classification from decoded row bytes when available; unknown means the row does not match a supported active/off value."
    if name == "assignment_output1_byte_0x1d":
        return "Decoded PROG assignment row +0x1d byte retained as raw per-row output data; it is not the Rch Assign display selector by itself."
    if name == "assignment_rch_assign_gate_byte_0x28":
        return "Decoded PROG assignment row byte used for active/off classification where 0xff is active and 0x00 is visible/off."
    if name == "assignment_rch_assign_display":
        return "Conservative visible Rch Assign family: off, =SMP, 01 through 16, BasicRch, B01 through B16, or unknown."
    if name in {"basis", "extraction_basis", "notes", "match_notes"}:
        return "Quality/basis origin field. Do not treat as decoded raw storage by itself."
    if name.startswith("raw_") or name.endswith("_offset") or name.endswith("_offset_hex"):
        return "Raw image/object reference for auditability."
    if name in {"source_path", "source_container", "image"}:
        return "Input source path used to produce this row."
    if name in ISSUE_CODE_COLUMN_NAMES:
        return "Stable diagnostic/validation code where applicable."
    return ""


def make_schema_manifest(
    report_name: str,
    rows: Sequence[object],
    *,
    semantic_notes: str = "",
    replacement_notes: str = "",
    schema_version: str = SCHEMA_VERSION,
    source_command: str = "",
    library_version: str = "",
) -> ReportSchemaManifest:
    plain_rows = [row_to_dict(row) for row in rows]
    all_keys = sorted({key for row in plain_rows for key in row})
    row_count = len(plain_rows)
    columns: list[ReportColumnSchema] = []
    for key in all_keys:
        present = sum(1 for row in plain_rows if key in row)
        types = {_value_type(row.get(key)) for row in plain_rows if key in row}
        nullable = "null" in types or present < row_count
        columns.append(
            ReportColumnSchema(
                name=key,
                type=_combined_type(types),
                required=bool(row_count and present == row_count),
                nullable=nullable,
                semantic_notes=_field_semantic_notes(key),
            )
        )

    quality_counts: Counter[str] = Counter()
    issue_code_counts: Counter[str] = Counter()
    object_type_counts: Counter[str] = Counter()
    for row in plain_rows:
        for key in ("quality", "extraction_quality", "match_quality", "organization_relationship_quality"):
            value = row.get(key)
            if value not in (None, ""):
                quality_counts[str(value)] += 1
        for key in ("code", "issue_code"):
            value = row.get(key)
            if value not in (None, ""):
                issue_code_counts[str(value)] += 1
        value = row.get("object_type") or row.get("type") or row.get("matched_target_type")
        if value not in (None, ""):
            object_type_counts[str(value)] += 1

    return ReportSchemaManifest(
        report_name=report_name,
        schema_version=schema_version,
        row_count=row_count,
        columns=tuple(columns),
        quality_counts=dict(sorted(quality_counts.items())),
        issue_code_counts=dict(sorted(issue_code_counts.items())),
        object_type_counts=dict(sorted(object_type_counts.items())),
        quality_columns=tuple(key for key in all_keys if key in QUALITY_COLUMN_NAMES),
        issue_code_columns=tuple(key for key in all_keys if key in ISSUE_CODE_COLUMN_NAMES),
        object_ref_columns=tuple(key for key in all_keys if key in OBJECT_REF_COLUMN_NAMES),
        source_command=source_command,
        library_version=library_version,
        semantic_notes=semantic_notes,
        replacement_notes=replacement_notes,
    )


def write_schema_manifest(
    path: Path,
    report_name: str,
    rows: Sequence[object],
    *,
    semantic_notes: str = "",
    replacement_notes: str = "",
    schema_version: str = SCHEMA_VERSION,
    source_command: str = "",
    library_version: str = "",
) -> ReportSchemaManifest:
    manifest = make_schema_manifest(
        report_name,
        rows,
        semantic_notes=semantic_notes,
        replacement_notes=replacement_notes,
        schema_version=schema_version,
        source_command=source_command,
        library_version=library_version,
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    write_json(path, to_plain(manifest))
    return manifest


def write_schema_index(output_dir: Path, manifests: list[ReportSchemaManifest]) -> None:
    write_json(
        output_dir / "schema_index.json",
        {
            "schema_version": SCHEMA_VERSION,
            "report_count": len(manifests),
            "reports": [
                {
                    "report_name": manifest.report_name,
                    "row_count": manifest.row_count,
                    "column_count": len(manifest.columns),
                    "quality_counts": manifest.quality_counts,
                    "issue_code_counts": manifest.issue_code_counts,
                    "object_type_counts": manifest.object_type_counts,
                    "quality_columns": manifest.quality_columns,
                    "issue_code_columns": manifest.issue_code_columns,
                    "object_ref_columns": manifest.object_ref_columns,
                    "source_command": manifest.source_command,
                    "library_version": manifest.library_version,
                }
                for manifest in manifests
            ],
        },
    )
