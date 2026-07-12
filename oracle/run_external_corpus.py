"""Run bounded SFS differential checks against an optional external corpus."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def corpus_images(root: Path, limit: int) -> list[Path]:
    if not root.is_dir():
        return []
    suffixes = {".hda", ".hds"}
    return [
        path
        for path in sorted(root.rglob("*"))
        if path.is_file() and path.suffix.lower() in suffixes
    ][:limit]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--cpp-cli", type=Path, required=True)
    parser.add_argument("--limit", type=int, default=16)
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args(argv)
    images = corpus_images(args.root, args.limit)
    if not images:
        message = f"external SFS corpus unavailable or empty: {args.root}"
        print(message, file=sys.stderr)
        return 2 if args.strict else 0
    command = [
        sys.executable,
        str(Path(__file__).with_name("run_sfs_differential.py")),
        "--cpp-cli",
        str(args.cpp_cli),
    ]
    if args.report:
        command.extend(["--report", str(args.report)])
    command.extend(str(path) for path in images)
    return subprocess.run(command, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
