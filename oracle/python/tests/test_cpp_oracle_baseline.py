from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BASELINE_PATH = ROOT / "oracle" / "baseline.json"


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def test_cpp_oracle_baseline_has_strict_shape_and_valid_files() -> None:
    value = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
    assert set(value) == {"commands", "fixtures", "oracle", "schema_version", "writer_profiles"}
    assert value["schema_version"] == "1.0"
    assert value["commands"] == sorted(set(value["commands"]))
    assert value["writer_profiles"] == sorted(set(value["writer_profiles"]))
    oracle = value["oracle"]
    assert set(oracle) == {
        "git_commit",
        "implementation",
        "lock_sha256",
        "python_version",
        "test_count",
    }
    assert oracle["implementation"] == "python"
    assert oracle["lock_sha256"] == _sha256(ROOT / "oracle/python/uv.lock")
    for fixture in value["fixtures"]:
        assert set(fixture) == {"path", "sha256", "size_bytes"}
        path = Path(fixture["path"])
        assert not path.is_absolute()
        resolved = ROOT / path
        assert fixture["sha256"] == _sha256(resolved)
        assert fixture["size_bytes"] == resolved.stat().st_size


def test_cpp_oracle_baseline_generation_is_deterministic(tmp_path: Path) -> None:
    value = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
    generated = tmp_path / "baseline.json"
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "oracle" / "generate_baseline.py"),
            "--oracle-commit",
            value["oracle"]["git_commit"],
            "--test-count",
            str(value["oracle"]["test_count"]),
            "--output",
            str(generated),
        ],
        check=True,
    )
    assert generated.read_bytes() == BASELINE_PATH.read_bytes()
