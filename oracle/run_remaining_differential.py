"""Run maintained differential lanes not covered by the focused reader matrix."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from axklib.build_manifest import build_hds_from_manifest, load_hds_build_manifest


def _run(script: Path, arguments: list[str]) -> bool:
    process = subprocess.run(
        [sys.executable, str(script), *arguments], check=False
    )
    return process.returncode == 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp-cli", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    oracle = root / "oracle"
    manifests = root / "tests" / "fixtures" / "manifests"
    build_manifest = manifests / "build" / "empty-two-volume.json"
    transaction = manifests / "alteration" / "delete-volume.json"
    rejection = manifests / "alteration" / "reject-missing-volume.json"
    args.output_dir.mkdir(parents=True, exist_ok=True)
    source = args.output_dir / "alteration-source.hds"
    build_hds_from_manifest(load_hds_build_manifest(build_manifest), source)

    executable_suffix = args.cpp_cli.suffix
    effect_dump = args.cpp_cli.with_name(f"axk_effect_dump{executable_suffix}")
    lanes = {
        "writer": _run(
            oracle / "run_writer_differential.py",
            [
                "--cpp-cli", str(args.cpp_cli),
                "--report", str(args.output_dir / "writer.json"),
                str(build_manifest),
            ],
        ),
        "alteration": _run(
            oracle / "run_alteration_differential.py",
            [
                "--cpp-cli", str(args.cpp_cli),
                "--report", str(args.output_dir / "alteration.json"),
                "--case", str(source), str(transaction),
                "--reject-case", str(source), str(rejection),
            ],
        ),
        "cli": _run(
            oracle / "run_cli_differential.py",
            [
                "--cpp-cli", str(args.cpp_cli),
                "--fixture", str(args.fixture),
                "--report", str(args.output_dir / "cli.json"),
            ],
        ),
        "media": _run(
            oracle / "run_media_differential.py",
            [
                "--cpp-cli", str(args.cpp_cli),
                "--fixture", str(args.fixture),
                "--report", str(args.output_dir / "media.json"),
            ],
        ),
        "effects": _run(
            oracle / "run_effect_differential.py",
            [
                "--cpp-dump", str(effect_dump),
                "--report", str(args.output_dir / "effects.json"),
            ],
        ),
    }
    summary = {"schema_version": "1.0", "operation": "remaining-differential", "lanes": lanes}
    (args.output_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return 0 if all(lanes.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
