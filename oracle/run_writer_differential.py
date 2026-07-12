"""Compare native fresh-image output byte-for-byte with the Python writer."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path

from axklib.build_manifest import build_hds_from_manifest, load_hds_build_manifest


def _digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def run(cpp_cli: Path, manifests: list[Path], report_path: Path) -> bool:
    results: list[dict[str, object]] = []
    with tempfile.TemporaryDirectory(prefix="axklib-writer-oracle-") as directory:
        root = Path(directory)
        for index, source in enumerate(manifests):
            manifest = load_hds_build_manifest(source)
            python_output = root / f"{index:02d}-python.hds"
            native_output = root / f"{index:02d}-native.hds"
            build_hds_from_manifest(manifest, python_output)
            process = subprocess.run(
                [
                    str(cpp_cli),
                    "create",
                    "hds",
                    str(source),
                    "--output",
                    str(native_output),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            python_hash = _digest(python_output)
            native_hash = _digest(native_output) if native_output.exists() else None
            results.append(
                {
                    "manifest": source.as_posix(),
                    "native_exit_code": process.returncode,
                    "native_stderr": process.stderr,
                    "python_sha256": python_hash,
                    "native_sha256": native_hash,
                    "byte_identical": process.returncode == 0 and python_hash == native_hash,
                }
            )
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(
        json.dumps(
            {"schema_version": "1.0", "operation": "fresh-writer", "results": results},
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    return all(bool(item["byte_identical"]) for item in results)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp-cli", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("manifests", nargs="+", type=Path)
    args = parser.parse_args()
    return 0 if run(args.cpp_cli, args.manifests, args.report) else 1


if __name__ == "__main__":
    raise SystemExit(main())
