from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def test_sbom_generator_is_deterministic_and_spdx_shaped(tmp_path: Path) -> None:
    first = tmp_path / "first.json"
    second = tmp_path / "second.json"
    command = [
        sys.executable,
        str(ROOT / "oracle/generate_sbom.py"),
        "--axklib-root",
        str(ROOT),
    ]
    subprocess.run([*command, "--output", str(first)], check=True)
    subprocess.run([*command, "--output", str(second)], check=True)
    assert first.read_bytes() == second.read_bytes()
    document = json.loads(first.read_text())
    assert document["spdxVersion"] == "SPDX-2.3"
    assert len(document["packages"]) >= 6


def test_package_inspector_accepts_sdk_and_rejects_runtime_or_internal_content(
    tmp_path: Path,
) -> None:
    clean = tmp_path / "clean"
    clean.mkdir()
    (clean / "axklib").write_text("native cli")
    inspector = ROOT / "oracle/inspect_package.py"
    subprocess.run([sys.executable, str(inspector), str(clean)], check=True)
    (clean / "python3").write_text("runtime")
    rejected = subprocess.run(
        [sys.executable, str(inspector), str(clean)], check=False, capture_output=True, text=True
    )
    assert rejected.returncode == 1
    assert "scripting runtime content" in rejected.stdout
    (clean / "python3").unlink()
    (clean / "large-binary").write_bytes(b"x" * (2 * 1024 * 1024) + b"/home/user/build")
    rejected = subprocess.run(
        [sys.executable, str(inspector), str(clean)], check=False, capture_output=True, text=True
    )
    assert rejected.returncode == 1
    assert "machine-local path" in rejected.stdout


def test_production_targets_do_not_depend_on_the_python_oracle() -> None:
    subprocess.run(
        [sys.executable, str(ROOT / "oracle/check_production_boundary.py"), str(ROOT)],
        check=True,
    )
