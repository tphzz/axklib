#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path

TEXT_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".cmake", ".json", ".toml", ".rs"}
FORBIDDEN = ("oracle/python", "python3", "python.exe", "py -", "pyo3", "pybind11")
SKIP_PARTS = {"target", "build", "node_modules", "native-runtime"}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--desktop-root", type=Path)
    args = parser.parse_args()

    roots = [
        args.root / "CMakeLists.txt",
        args.root / "library/src",
        args.root / "apps/cli",
    ]
    if args.desktop_root:
        roots.extend([args.desktop_root / "src", args.desktop_root / "src-tauri"])
    issues: list[dict[str, str]] = []
    for root in roots:
        paths = [root] if root.is_file() else root.rglob("*")
        for path in paths:
            if (
                not path.is_file()
                or path.suffix.lower() not in TEXT_SUFFIXES
                or any(part in SKIP_PARTS for part in path.parts)
            ):
                continue
            text = path.read_text(encoding="utf-8", errors="ignore").lower()
            for token in FORBIDDEN:
                if token in text:
                    issues.append({"path": path.as_posix(), "token": token})
    print(json.dumps({"valid": not issues, "issues": issues}, indent=2))
    return 0 if not issues else 1


if __name__ == "__main__":
    raise SystemExit(main())
