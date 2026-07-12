#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from pathlib import Path


def sha512(path: Path) -> str:
    digest = hashlib.sha512()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def collect(specification: Path, downloads: Path, output: Path) -> list[dict[str, str]]:
    value = json.loads(specification.read_text(encoding="utf-8"))
    packages = value.get("packages")
    if value.get("schema_version") != "1.0" or not isinstance(packages, list):
        raise ValueError("unsupported LGPL source specification")
    candidates = [path for path in downloads.iterdir() if path.is_file()]
    by_digest = {sha512(path): path for path in candidates}
    output.mkdir(parents=True, exist_ok=True)
    collected: list[dict[str, str]] = []
    for package in packages:
        expected = str(package["sha512"])
        source = by_digest.get(expected)
        if source is None:
            raise FileNotFoundError(
                f"exact source archive for {package['name']} {package['version']} is missing"
            )
        target = output / source.name
        shutil.copyfile(source, target)
        collected.append(
            {
                "name": str(package["name"]),
                "version": str(package["version"]),
                "file": target.name,
                "sha512": expected,
            }
        )
    (output / "sources.json").write_text(
        json.dumps({"schema_version": "1.0", "sources": collected}, indent=2) + "\n",
        encoding="utf-8",
    )
    return collected


def main() -> int:
    parser = argparse.ArgumentParser(description="Collect exact LGPL source archives by digest")
    parser.add_argument("--specification", required=True, type=Path)
    parser.add_argument("--downloads", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    collect(args.specification, args.downloads, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
