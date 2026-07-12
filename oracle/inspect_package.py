#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

SCRIPT_SUFFIXES = {".py", ".pyc", ".pyo"}
FORBIDDEN_PARTS = {"oracle", "tests", "corpus", "sample-images", "__pycache__"}
SHARED_SUFFIXES = {".dll", ".dylib", ".so"}
PATH_PATTERNS = [
    re.compile(r"(?<![A-Za-z0-9])[A-Za-z]:[\\/]"),
    re.compile(r"/(?:Users|home|mnt|Volumes)/"),
]
BINARY_PATH_MARKERS = (b"/home/", b"/Users/", b"/mnt/", b"/Volumes/")


def contains_binary_path(path: Path) -> bool:
    overlap = b""
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            value = overlap + block
            if any(marker in value for marker in BINARY_PATH_MARKERS):
                return True
            overlap = value[-32:]
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--allow-library", action="append", default=[])
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    allowed = set(args.allow_library)
    issues: list[dict[str, str]] = []
    for path in sorted(args.root.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(args.root)
        lower_parts = {part.lower() for part in relative.parts}
        if path.suffix.lower() in SCRIPT_SUFFIXES or path.name.lower().startswith("python"):
            issues.append({"path": relative.as_posix(), "reason": "scripting runtime content"})
        if lower_parts & FORBIDDEN_PARTS:
            issues.append({"path": relative.as_posix(), "reason": "development-only content"})
        suffix = path.suffix.lower()
        is_versioned_so = ".so." in path.name.lower()
        if (suffix in SHARED_SUFFIXES or is_versioned_so) and path.name not in allowed:
            issues.append({"path": relative.as_posix(), "reason": "undeclared shared library"})
        if contains_binary_path(path):
            issues.append({"path": relative.as_posix(), "reason": "machine-local path"})
            continue
        if path.stat().st_size <= 2 * 1024 * 1024:
            text = path.read_text(encoding="utf-8", errors="ignore")
            if any(pattern.search(text) for pattern in PATH_PATTERNS):
                issues.append({"path": relative.as_posix(), "reason": "machine-local path"})
    report = {"schema_version": "1.0", "root": args.root.name, "valid": not issues, "issues": issues}
    rendered = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered)
    else:
        print(rendered, end="")
    return 0 if not issues else 1


if __name__ == "__main__":
    raise SystemExit(main())
