"""Validate volume-level Yamaha SFS directory consistency from inventory reports.

This is a report layer over ``report_sfs_inventory.py`` output. It is
intended to separate volumes that are structurally valid but contain hidden or
unmapped objects from volumes that likely fail sampler-side loading because a
visible category directory contains malformed entries. It also warns when a visible tree contains alternating-byte compatibility artifact objects whose sampler loadability is not proven.
"""

from __future__ import annotations

import argparse
import csv
import json
from collections.abc import Sequence
from dataclasses import asdict, dataclass, fields
from pathlib import Path
from typing import Any

CATEGORY_CODES = {"SMPL", "SBNK", "SBAC", "SEQU", "PROG"}


@dataclass
class VolumeValidationRow:
    source_image: str
    partition_index: int
    partition_name: str
    volume_name: str
    volume_path: str
    directory_id: int
    category_count: int
    object_entry_count: int
    matched_object_count: int
    category_directory_count: int
    checked_category_entry_count: int
    valid_category_entry_count: int
    malformed_category_entry_count: int
    category_count_mismatch_count: int
    current_object_entry_count: int
    compatibility_artifact_object_entry_count: int
    compatibility_artifact_smpl_entry_count: int
    fatal_issue_count: int
    warning_issue_count: int
    allocation_status: str
    allocation_issue_count: int
    validation_status: str
    volume_classification: str
    quality_summary: str


@dataclass
class VolumeValidationIssueRow:
    source_image: str
    partition_index: int
    partition_name: str
    volume_name: str
    volume_path: str
    severity: str
    issue_type: str
    category_code: str
    category_name: str
    category_directory_id: int | None
    category_directory_path: str
    entry_offset: int | None
    entry_name: str
    link_id: int | None
    target_kind: str
    target_sfs_id: int | None
    target_payload_kind: str
    match_quality: str
    unmatched_reason: str
    details: str


@dataclass
class VolumeValidationSummaryRow:
    source_image: str
    volume_count: int
    pass_count: int
    warn_count: int
    fail_count: int
    fatal_issue_count: int
    warning_issue_count: int
    malformed_category_entry_count: int
    allocation_issue_count: int


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def int_field(row: dict[str, str], name: str, default: int = 0) -> int:
    value = row.get(name, "")
    if value is None or value == "":
        return default
    return int(value)


def optional_int(value: str | None) -> int | None:
    if value is None or value == "":
        return None
    return int(value)


def write_csv(path: Path, rows: Sequence[Any], row_type: type[Any]) -> None:
    field_names = [field.name for field in fields(row_type)]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=field_names)
        writer.writeheader()
        for row in rows:
            writer.writerow(asdict(row))


def write_json(path: Path, rows: Sequence[Any]) -> None:
    path.write_text(json.dumps([asdict(row) for row in rows], indent=2) + "\n", encoding="utf-8")


def clean_partition_key(row: dict[str, str]) -> tuple[str, int]:
    return row.get("source_image", ""), int_field(row, "partition_index")


def load_allocation_issues(allocation_dir: Path | None) -> dict[tuple[str, int], tuple[str, int, str]]:
    if allocation_dir is None:
        return {}
    path = allocation_dir / "allocation_summary.csv"
    if not path.exists():
        raise FileNotFoundError(f"missing allocation summary: {path}")

    issues: dict[tuple[str, int], tuple[str, int, str]] = {}
    for row in read_csv(path):
        issue_count = (
            int_field(row, "stored_used_not_reconstructed_count")
            + int_field(row, "reconstructed_used_not_stored_count")
            + int_field(row, "extent_total_mismatch_count")
            + int_field(row, "warning_count")
        )
        status = "Pass" if issue_count == 0 else "Fail"
        details = row.get("warnings", "")
        issues[clean_partition_key(row)] = (status, issue_count, details)
    return issues



def load_volume_object_counts(
    inventory_dir: Path,
) -> dict[tuple[str, int, str], dict[str, int]]:
    path = inventory_dir / "volume_objects.csv"
    if not path.exists():
        return {}

    counts_by_volume: dict[tuple[str, int, str], dict[str, int]] = {}
    for row in read_csv(path):
        key = (
            row.get("source_image", ""),
            int_field(row, "partition_index"),
            row.get("volume_path", ""),
        )
        counts = counts_by_volume.setdefault(
            key,
            {
                "current_object_entry_count": 0,
                "compatibility_artifact_object_entry_count": 0,
                "compatibility_artifact_smpl_entry_count": 0,
                "compatibility_artifact_sbnk_entry_count": 0,
                "compatibility_artifact_sbac_entry_count": 0,
                "compatibility_artifact_prog_entry_count": 0,
            },
        )
        match_method = row.get("match_method", "")
        if match_method == "link-id+type":
            counts["current_object_entry_count"] += 1
        elif match_method == "link-id+alternating-byte-type":
            counts["compatibility_artifact_object_entry_count"] += 1
            category_code = row.get("category_code", "").lower()
            key_name = f"compatibility_artifact_{category_code}_entry_count"
            if key_name in counts:
                counts[key_name] += 1
    return counts_by_volume


def is_category_object_entry(row: dict[str, str]) -> bool:
    name = row.get("name", "")
    if name in (".", ".."):
        return False
    if row.get("target_kind", "") == "directory":
        return False
    return True


def malformed_reasons(row: dict[str, str]) -> list[str]:
    reasons: list[str] = []
    if row.get("name", "") == "":
        reasons.append("blank name")
    if int_field(row, "link_id") == 0:
        reasons.append("link_id 0")
    if row.get("match_quality", "") != "Known":
        reasons.append(f"match quality {row.get('match_quality', '') or 'blank'}")
    if row.get("unmatched_reason", ""):
        reasons.append(f"unmatched reason {row['unmatched_reason']}")
    if row.get("target_payload_kind", "") == "unknown":
        reasons.append("unknown target payload kind")
    target_object_type = row.get("target_object_type", "")
    if target_object_type and target_object_type not in CATEGORY_CODES:
        reasons.append(f"unexpected object type {target_object_type}")
    return reasons


def build_report(
    *,
    inventory_dir: Path,
    output_dir: Path,
    allocation_dir: Path | None = None,
) -> tuple[list[VolumeValidationRow], list[VolumeValidationIssueRow], VolumeValidationSummaryRow]:
    output_dir.mkdir(parents=True, exist_ok=True)

    volumes = read_csv(inventory_dir / "volumes.csv")
    categories = read_csv(inventory_dir / "volume_categories.csv")
    directory_entries = read_csv(inventory_dir / "directory_entries.csv")
    allocation_by_partition = load_allocation_issues(allocation_dir)
    object_counts_by_volume = load_volume_object_counts(inventory_dir)

    category_by_key: dict[tuple[str, int, int], dict[str, str]] = {}
    categories_by_volume: dict[tuple[str, int, str], list[dict[str, str]]] = {}
    for row in categories:
        source = row.get("source_image", "")
        partition_index = int_field(row, "partition_index")
        directory_id = int_field(row, "directory_id")
        category_by_key[(source, partition_index, directory_id)] = row
        volume_key = (source, partition_index, row.get("volume_path", ""))
        categories_by_volume.setdefault(volume_key, []).append(row)

    entries_by_category: dict[tuple[str, int, int], list[dict[str, str]]] = {}
    for row in directory_entries:
        key = (row.get("source_image", ""), int_field(row, "partition_index"), int_field(row, "directory_id"))
        if key in category_by_key:
            entries_by_category.setdefault(key, []).append(row)

    report_rows: list[VolumeValidationRow] = []
    issue_rows: list[VolumeValidationIssueRow] = []

    for volume in volumes:
        source = volume.get("source_image", "")
        partition_index = int_field(volume, "partition_index")
        partition_name = volume.get("partition_name", "")
        volume_name = volume.get("volume_name", "")
        volume_path = volume.get("volume_path", "")
        volume_key = (source, partition_index, volume_path)
        volume_categories = categories_by_volume.get(volume_key, [])

        checked_entries = 0
        valid_entries = 0
        malformed_entries = 0
        category_count_mismatches = 0
        fatal_count = 0
        warning_count = 0
        quality: list[str] = []
        object_counts = object_counts_by_volume.get(volume_key, {})
        current_object_entries = object_counts.get("current_object_entry_count", 0)
        compatibility_artifact_entries = object_counts.get("compatibility_artifact_object_entry_count", 0)
        compatibility_artifact_smpl_entries = object_counts.get("compatibility_artifact_smpl_entry_count", 0)

        for category in volume_categories:
            category_key = (source, partition_index, int_field(category, "directory_id"))
            category_entries = [entry for entry in entries_by_category.get(category_key, []) if is_category_object_entry(entry)]
            matched_object_count = int_field(category, "matched_object_count")
            object_entry_count = int_field(category, "object_entry_count")
            if len(category_entries) != object_entry_count or matched_object_count != object_entry_count:
                category_count_mismatches += 1
                fatal_count += 1
                issue_rows.append(
                    VolumeValidationIssueRow(
                        source_image=source,
                        partition_index=partition_index,
                        partition_name=partition_name,
                        volume_name=volume_name,
                        volume_path=volume_path,
                        severity="fatal",
                        issue_type="category-object-count-mismatch",
                        category_code=category.get("category_code", ""),
                        category_name=category.get("category_name", ""),
                        category_directory_id=int_field(category, "directory_id"),
                        category_directory_path=f"{volume_path}/{category.get('category_code', '')}",
                        entry_offset=None,
                        entry_name="",
                        link_id=None,
                        target_kind="",
                        target_sfs_id=None,
                        target_payload_kind="",
                        match_quality="",
                        unmatched_reason="",
                        details=(
                            f"category reports object_entry_count={object_entry_count}, "
                            f"matched_object_count={matched_object_count}, checked_entries={len(category_entries)}"
                        ),
                    )
                )

            for entry in category_entries:
                checked_entries += 1
                reasons = malformed_reasons(entry)
                if reasons:
                    malformed_entries += 1
                    fatal_count += 1
                    issue_rows.append(
                        VolumeValidationIssueRow(
                            source_image=source,
                            partition_index=partition_index,
                            partition_name=partition_name,
                            volume_name=volume_name,
                            volume_path=volume_path,
                            severity="fatal",
                            issue_type="malformed-category-entry",
                            category_code=category.get("category_code", ""),
                            category_name=category.get("category_name", ""),
                            category_directory_id=int_field(category, "directory_id"),
                            category_directory_path=entry.get("directory_path", ""),
                            entry_offset=optional_int(entry.get("entry_offset")),
                            entry_name=entry.get("name", ""),
                            link_id=optional_int(entry.get("link_id")),
                            target_kind=entry.get("target_kind", ""),
                            target_sfs_id=optional_int(entry.get("target_sfs_id")),
                            target_payload_kind=entry.get("target_payload_kind", ""),
                            match_quality=entry.get("match_quality", ""),
                            unmatched_reason=entry.get("unmatched_reason", ""),
                            details="; ".join(reasons),
                        )
                    )
                else:
                    valid_entries += 1

        if compatibility_artifact_entries:
            warning_count += 1
            legacy_details = (
                f"visible alternating-byte compatibility artifact object entries: total={compatibility_artifact_entries}, "
                f"SMPL={compatibility_artifact_smpl_entries}, "
                f"SBNK={object_counts.get('compatibility_artifact_sbnk_entry_count', 0)}, "
                f"SBAC={object_counts.get('compatibility_artifact_sbac_entry_count', 0)}, "
                f"PROG={object_counts.get('compatibility_artifact_prog_entry_count', 0)}; "
                "filesystem tree/allocation validation does not prove sampler loadability "
                "for this physical alternating-byte artifact family"
            )
            quality.append(legacy_details)
            issue_rows.append(
                VolumeValidationIssueRow(
                    source_image=source,
                    partition_index=partition_index,
                    partition_name=partition_name,
                    volume_name=volume_name,
                    volume_path=volume_path,
                    severity="warning",
                    issue_type="visible-alternating-byte-compatibility-artifact-objects",
                    category_code="",
                    category_name="",
                    category_directory_id=None,
                    category_directory_path="",
                    entry_offset=None,
                    entry_name="",
                    link_id=None,
                    target_kind="object",
                    target_sfs_id=None,
                    target_payload_kind="alternating-byte-compatibility-object",
                    match_quality="Likely",
                    unmatched_reason="",
                    details=legacy_details,
                )
            )

        allocation_status, allocation_issue_count, allocation_details = allocation_by_partition.get(
            (source, partition_index), ("Not checked", 0, "")
        )
        if allocation_issue_count:
            fatal_count += allocation_issue_count
            issue_rows.append(
                VolumeValidationIssueRow(
                    source_image=source,
                    partition_index=partition_index,
                    partition_name=partition_name,
                    volume_name=volume_name,
                    volume_path=volume_path,
                    severity="fatal",
                    issue_type="partition-allocation-consistency",
                    category_code="",
                    category_name="",
                    category_directory_id=None,
                    category_directory_path="",
                    entry_offset=None,
                    entry_name="",
                    link_id=None,
                    target_kind="",
                    target_sfs_id=None,
                    target_payload_kind="",
                    match_quality="",
                    unmatched_reason="",
                    details=allocation_details or f"partition allocation issue count {allocation_issue_count}",
                )
            )

        if malformed_entries:
            quality.append(f"{malformed_entries} malformed category entries")
        if category_count_mismatches:
            quality.append(f"{category_count_mismatches} category count mismatches")
        if allocation_issue_count:
            quality.append(f"{allocation_issue_count} allocation issues")
        if not quality:
            quality.append("category directory entries and optional allocation check passed")

        status = "Fail" if fatal_count else ("Warn" if warning_count else "Pass")
        if status == "Fail":
            volume_classification = "volume-likely-corrupt"
        elif status == "Warn":
            volume_classification = "valid-visible-tree-with-warnings"
        else:
            volume_classification = "valid-visible-tree-hidden-unreferenced-not-an-error"
        report_rows.append(
            VolumeValidationRow(
                source_image=source,
                partition_index=partition_index,
                partition_name=partition_name,
                volume_name=volume_name,
                volume_path=volume_path,
                directory_id=int_field(volume, "directory_id"),
                category_count=int_field(volume, "category_count"),
                object_entry_count=int_field(volume, "object_entry_count"),
                matched_object_count=int_field(volume, "matched_object_count"),
                category_directory_count=len(volume_categories),
                checked_category_entry_count=checked_entries,
                valid_category_entry_count=valid_entries,
                malformed_category_entry_count=malformed_entries,
                category_count_mismatch_count=category_count_mismatches,
                current_object_entry_count=current_object_entries,
                compatibility_artifact_object_entry_count=compatibility_artifact_entries,
                compatibility_artifact_smpl_entry_count=compatibility_artifact_smpl_entries,
                fatal_issue_count=fatal_count,
                warning_issue_count=warning_count,
                allocation_status=allocation_status,
                allocation_issue_count=allocation_issue_count,
                validation_status=status,
                volume_classification=volume_classification,
                quality_summary="; ".join(quality),
            )
        )

    source_image = report_rows[0].source_image if report_rows else ""
    summary = VolumeValidationSummaryRow(
        source_image=source_image,
        volume_count=len(report_rows),
        pass_count=sum(1 for row in report_rows if row.validation_status == "Pass"),
        warn_count=sum(1 for row in report_rows if row.validation_status == "Warn"),
        fail_count=sum(1 for row in report_rows if row.validation_status == "Fail"),
        fatal_issue_count=sum(row.fatal_issue_count for row in report_rows),
        warning_issue_count=sum(row.warning_issue_count for row in report_rows),
        malformed_category_entry_count=sum(row.malformed_category_entry_count for row in report_rows),
        allocation_issue_count=sum(row.allocation_issue_count for row in report_rows),
    )

    write_csv(output_dir / "volume_validation.csv", report_rows, VolumeValidationRow)
    write_json(output_dir / "volume_validation.json", report_rows)
    write_csv(output_dir / "volume_validation_issues.csv", issue_rows, VolumeValidationIssueRow)
    write_json(output_dir / "volume_validation_issues.json", issue_rows)
    write_csv(output_dir / "volume_validation_summary.csv", [summary], VolumeValidationSummaryRow)
    write_json(output_dir / "volume_validation_summary.json", [summary])
    return report_rows, issue_rows, summary


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory-dir", type=Path, required=True)
    parser.add_argument("--allocation-dir", type=Path)
    parser.add_argument("-o", "--output-dir", type=Path, required=True)
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    _rows, _issues, summary = build_report(
        inventory_dir=args.inventory_dir,
        allocation_dir=args.allocation_dir,
        output_dir=args.output_dir,
    )
    print(f"volumes: {summary.volume_count}")
    print(f"pass: {summary.pass_count}")
    print(f"warn: {summary.warn_count}")
    print(f"fail: {summary.fail_count}")
    print(f"malformed category entries: {summary.malformed_category_entry_count}")
    print(f"allocation issues: {summary.allocation_issue_count}")
    print(f"wrote: {args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


