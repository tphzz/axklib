#!/usr/bin/env python3
"""Check documented TX16W floppy sets, every encoded detail and continuation order."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import time
from pathlib import Path
from typing import Any

from image_open_smoke import result_code, run_case


def load_sets(path: Path) -> list[dict[str, Any]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    cases = document["sets"]
    if not isinstance(cases, list) or not 1 <= len(cases) <= 160:
        raise ValueError("expected 1 to 160 documented sets")
    names: set[str] = set()
    sources: set[str] = set()
    for case in cases:
        if case["id"] in names or not 1 <= len(case["sources"]) <= 32:
            raise ValueError("duplicate set or invalid member count")
        names.add(case["id"])
        parents: set[Path] = set()
        for name in case["sources"]:
            member = Path(name)
            if member.is_absolute() or ".." in member.parts or name in sources:
                raise ValueError("source escapes root or belongs to multiple expected sets")
            parents.add(member.parent)
            sources.add(name)
        if len(parents) != 1 or not case["catalog"]:
            raise ValueError("a documented set must have sibling sources and a catalog reference")
        if not {"completeness", "requirementStates"} <= case["expected"].keys():
            raise ValueError("each set needs explicit source-state expectations")
    return cases


def variants(sources: list[str]) -> list[tuple[str, list[str], bool]]:
    if len(sources) == 1:
        return [("single", sources, False)]
    # A late wave-only disk has no references to other wave-only disks. Attach the
    # catalog's Setup-bearing first disk next, then the remaining companions.
    late_start = [sources[-1], sources[0], *reversed(sources[1:-1])]
    return [("batch", sources, False), ("reverse", list(reversed(sources)), False),
            ("incremental", sources, True), ("last-first", late_start, True)]


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    server = args.server.resolve(strict=True)
    corpus = args.corpus.resolve(strict=True)
    sets = load_sets(args.manifest)
    args.output.mkdir(parents=True, exist_ok=False)
    cases = [(case, variant) for case in sets for variant in variants(case["sources"])]
    results: list[dict[str, Any]] = [
        {"id": f"{case['id']}-{variant[0]}", "status": "not_run"} for case, variant in cases
    ]
    report: dict[str, Any] = {"server": str(server), "serverSha256": digest(server),
                            "manifestSha256": digest(args.manifest), "releaseReady": False,
                            "results": results}
    for index, (case, (_mode, names, incremental)) in enumerate(cases):
        row = results[index]
        started = time.monotonic()
        paths = [corpus / name for name in names]
        row.update(sources=[str(path) for path in paths], catalog=case["catalog"])
        try:
            if not all(path.is_file() and path.resolve().is_relative_to(corpus) for path in paths):
                row.update(status="missing", reason="A documented member is unavailable")
            else:
                before = [digest(path) for path in paths]
                expected = {"format": "yamaha.tx16w", "filesystemName": "FAT12" if len(paths) == 1 else "sources",
                            "deviceView": "yamaha-native", "minimumFiles": 1,
                            "allObjectDetails": True, "wavePreviews": True, **case["expected"]}
                try:
                    row.update(run_case(
                        server, paths[0], expected, args.output / f"{row['id']}.log",
                        maximum_requests=4096, traversal_seconds=60,
                        companion_steps=[{"path": path.name} for path in paths[1:]] if incremental else None,
                        additional_sources=None if incremental else [path.name for path in paths[1:]],
                    ), status="passed")
                finally:
                    row["sourceSha256"] = before
                    if before != [digest(path) for path in paths]:
                        raise RuntimeError("read-only corpus bytes changed")
        except (RuntimeError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
            row.update(status="failed", reason=str(error))
        row["seconds"] = round(time.monotonic() - started, 3)
        report["exitCode"] = result_code(results)
        (args.output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"{row['status'].upper()} {row['id']}: {row.get('reason', row['seconds'])}", flush=True)
    return result_code(results)


if __name__ == "__main__":
    raise SystemExit(main())
