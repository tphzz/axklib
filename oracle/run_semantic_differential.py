"""Compare compact Python and native relationship/content semantics."""

from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
from collections import Counter
from pathlib import Path
from typing import Any

from python_semantic_summary import semantic_summary


def _run_json(cli: Path, command: str, image: Path) -> dict[str, Any]:
    process = subprocess.run(
        [str(cli), command, str(image)], check=False, capture_output=True, text=True
    )
    if process.returncode not in {0, 1} or not process.stdout:
        raise RuntimeError(
            f"native {command} failed with {process.returncode}: {process.stderr.strip()}"
        )
    value: dict[str, Any] = json.loads(process.stdout)
    return value


def _counter_rows(counter: Counter[tuple[str, ...]]) -> list[list[object]]:
    return [[*key, count] for key, count in sorted(counter.items())]


def _flatten(nodes: list[dict[str, Any]]) -> list[list[object]]:
    rows: list[list[object]] = []

    def visit(node: dict[str, Any], parent: str) -> None:
        display = str(node["display_name"])
        path = f"{parent}/{display}" if parent else display
        children = node["children"]
        rows.append([path, node["node_type"], node["object_type"], len(children)])
        for child in children:
            visit(child, path)

    for node in nodes:
        visit(node, "")
    return rows


def native_summary(cli: Path, image: Path) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="axklib-semantic-oracle-") as temporary:
        root = Path(temporary)
        completed: set[str] = set()

        def report(command: str, name: str) -> Any:
            output = root / command
            if command not in completed:
                process = subprocess.run(
                    [str(cli), command, str(image), "--output-dir", str(output)],
                    check=False,
                    capture_output=True,
                    text=True,
                )
                if process.returncode not in {0, 3}:
                    raise RuntimeError(
                        f"native {command} failed with {process.returncode}: {process.stderr.strip()}"
                    )
                completed.add(command)
            return json.loads((output / name).read_text())

        relationships = report("relationships", "relationships.json")
        bitmaps = report(
            "relationships", "current_sbnk_program_bitmap_crosscheck.json"
        )
        orphans = report("orphans", "waveform_orphans.json")
        validation_issues = report("validate", "validation_issues.json")
        validation_summary = report("validate", "validation_summary.json")
        info = subprocess.run(
            [str(cli), "info", str(image), "--format", "json"],
            check=True,
            capture_output=True,
            text=True,
        )
        tree = json.loads(info.stdout)["trees"][0]
    relationship_counts = Counter(
        (row["relationship_type"], row["quality"], row["basis"])
        for row in relationships
    )
    bitmap_counts = Counter(
        (row["match_status"], row["mismatch_class"])
        for row in bitmaps
    )
    waveform_counts = Counter(row["status"] for row in orphans)
    validation_counts = Counter(
        (row["code"], row["severity"]) for row in validation_issues
    )
    return {
        "schema_version": "1.0",
        "relationship_counts": _counter_rows(relationship_counts),
        "bitmap_counts": _counter_rows(bitmap_counts),
        "waveform_status_counts": [
            [key, count] for key, count in sorted(waveform_counts.items())
        ],
        "content_nodes": _flatten(tree["roots"]),
        "validation_failed": bool(validation_summary["failed"]),
        "validation_counts": _counter_rows(validation_counts),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp-cli", type=Path, required=True)
    parser.add_argument("images", nargs="+", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    rows: list[dict[str, object]] = []
    failed = False
    for image in args.images:
        python_value = semantic_summary(image)
        cpp_value = native_summary(args.cpp_cli, image)
        matches = cpp_value == python_value
        failed |= not matches
        rows.append(
            {
                "image": image.as_posix(),
                "matches": matches,
                "python": python_value if not matches else None,
                "cpp": cpp_value if not matches else None,
            }
        )
    report = {"schema_version": "1.0", "operation": "semantic-read", "results": rows}
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
