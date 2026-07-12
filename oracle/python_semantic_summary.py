"""Emit a compact maintained semantic summary for native parity checks."""

from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path
from typing import Any

from axklib.containers import open as open_container
from axklib.content_tree import ContentNode, build_content_tree_for_container
from axklib.relationships import build_relationship_graph_for_paths
from axklib.validation import validate_container
from axklib.waveform_orphans import analyze_hds_waveform_orphans


def _counter_rows(counter: Counter[tuple[str, ...]]) -> list[list[object]]:
    return [[*key, count] for key, count in sorted(counter.items())]


def _flatten(nodes: tuple[ContentNode, ...]) -> list[list[object]]:
    rows: list[list[object]] = []

    def visit(node: ContentNode, parent: str) -> None:
        path = f"{parent}/{node.display_name}" if parent else node.display_name
        rows.append([path, node.node_type, node.object_type, len(node.children)])
        for child in node.children:
            visit(child, path)

    for node in nodes:
        visit(node, "")
    return rows


def semantic_summary(path: Path) -> dict[str, Any]:
    graph = build_relationship_graph_for_paths([path], container="sfs")
    relationship_counts = Counter(
        (row.relationship_type, row.quality, row.basis) for row in graph.relationships
    )
    bitmap_counts = Counter(
        (row.match_status, row.mismatch_class) for row in graph.sbnk_bitmap_rows
    )
    orphan_report = analyze_hds_waveform_orphans(path)
    orphan_counts = Counter(row.status for row in orphan_report.rows)
    container = open_container(path)
    tree = build_content_tree_for_container(container, include_validation=False)
    validation = validate_container(container)
    validation_counts = Counter((row.code, row.severity.value) for row in validation.issues)
    return {
        "schema_version": "1.0",
        "relationship_counts": _counter_rows(relationship_counts),
        "bitmap_counts": _counter_rows(bitmap_counts),
        "waveform_status_counts": [[key, count] for key, count in sorted(orphan_counts.items())],
        "content_nodes": _flatten(tree.roots),
        "validation_failed": validation.failed,
        "validation_counts": _counter_rows(validation_counts),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    print(json.dumps(semantic_summary(args.image), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
