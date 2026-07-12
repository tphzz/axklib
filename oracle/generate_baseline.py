from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

COMMANDS = (
    "alter hds",
    "corpus audit",
    "coverage",
    "create hds",
    "extract sfz file",
    "extract wav file",
    "info",
    "inventory",
    "objects",
    "orphans",
    "relationships",
    "validate",
)
WRITER_PROFILES = (
    "alteration_full_chain_v1",
    "alteration_rename_v1",
    "hds_geometry_v1",
    "sbac_prog_v1",
    "sbnk_mono_stereo_v1",
    "smpl_audio_import_v1",
)
FIXTURE_PATHS = (
    Path("tests/fixtures/images/sampler-authored/HD00_512_multi_sbnk_authored.hds"),
    Path("tests/fixtures/images/sampler-authored/HD00_512_single_sbnk_authored.hds"),
    Path("tests/fixtures/images/sampler-authored/MANIFEST.json"),
)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_baseline(root: Path, *, oracle_commit: str, test_count: int) -> dict[str, object]:
    if len(oracle_commit) != 40 or any(char not in "0123456789abcdef" for char in oracle_commit):
        raise ValueError("oracle commit must be a lowercase 40-character Git hash")
    if test_count < 1:
        raise ValueError("test count must be positive")
    fixtures = [
        {
            "path": path.as_posix(),
            "sha256": _sha256(root / path),
            "size_bytes": (root / path).stat().st_size,
        }
        for path in FIXTURE_PATHS
    ]
    return {
        "commands": list(COMMANDS),
        "fixtures": fixtures,
        "oracle": {
            "git_commit": oracle_commit,
            "implementation": "python",
            "lock_sha256": _sha256(root / "oracle/python/uv.lock"),
            "python_version": "3.13",
            "test_count": test_count,
        },
        "schema_version": "1.0",
        "writer_profiles": list(WRITER_PROFILES),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate an immutable axklib oracle baseline")
    parser.add_argument("--oracle-commit", required=True)
    parser.add_argument("--test-count", required=True, type=int)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    value = build_baseline(
        root,
        oracle_commit=args.oracle_commit,
        test_count=args.test_count,
    )
    args.output.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
