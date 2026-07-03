"""SFS allocation validation report service."""

from __future__ import annotations

from pathlib import Path

from axklib.containers.sfs_allocation import (
    AllocationExtent,
    AllocationMismatchRange,
    AllocationPartitionSummary,
    analyze_image,
)
from axklib.reports import write_csv, write_rows_json


def build_report(image: Path, output_dir: Path) -> list[AllocationPartitionSummary]:
    """Analyze allocation bitmap consistency and write parity CSV/JSON reports."""
    output_dir.mkdir(parents=True, exist_ok=True)
    report = analyze_image(image)
    summaries = list(report.summaries)
    extents = list(report.extents)
    mismatches = list(report.mismatches)

    write_csv(output_dir / "allocation_summary.csv", summaries, AllocationPartitionSummary)
    write_rows_json(output_dir / "allocation_summary.json", summaries)
    write_csv(output_dir / "allocation_extents.csv", extents, AllocationExtent)
    write_rows_json(output_dir / "allocation_extents.json", extents)
    write_csv(output_dir / "allocation_mismatches.csv", mismatches, AllocationMismatchRange)
    write_rows_json(output_dir / "allocation_mismatches.json", mismatches)
    return summaries