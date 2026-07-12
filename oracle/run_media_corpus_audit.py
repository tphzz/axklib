"""Compare native and Python media summaries over explicitly selected corpus files."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from run_media_differential import _case


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp-cli", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("images", nargs="+", type=Path)
    args = parser.parse_args()

    rows: list[dict[str, object]] = []
    if args.resume and args.report.is_file():
        rows = list(json.loads(args.report.read_text()).get("results", []))
    completed = {str(row["image"]) for row in rows}
    for image in args.images:
        if image.as_posix() in completed:
            continue
        kind = "iso" if image.suffix.lower() == ".iso" else "fat12_floppy"
        row = _case(args.cpp_cli, image, kind)
        row["image"] = image.as_posix()
        rows.append(row)
        partial = {
            "schema_version": "1.0",
            "operation": "external-media-corpus-audit",
            "success": False,
            "complete": False,
            "results": rows,
        }
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(partial, indent=2, sort_keys=True) + "\n")
    success = all(
        row["objects_match"] and row["relationships_match"] and row["tree_match"]
        for row in rows
    )
    report = {
        "schema_version": "1.0",
        "operation": "external-media-corpus-audit",
        "success": success,
        "complete": True,
        "results": rows,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
