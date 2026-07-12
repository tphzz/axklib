from __future__ import annotations

import json
from pathlib import Path

import pytest

import generate_sbom
import inspect_package


def test_sbom_includes_base_cli_and_test_dependencies(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    root = Path(__file__).resolve().parents[3]
    output = tmp_path / "sbom.json"
    monkeypatch.setattr(
        "sys.argv",
        ["generate_sbom.py", "--axklib-root", str(root), "--output", str(output)],
    )
    assert generate_sbom.main() == 0
    document = json.loads(output.read_text(encoding="utf-8"))
    names = {item["name"] for item in document["packages"]}
    assert {"axklib", "cli11", "gtest", "hash-library", "libsndfile", "soxr"} <= names
    sndfile = next(item for item in document["packages"] if item["name"] == "libsndfile")
    assert sndfile["versionInfo"].startswith("1.2.2")
    assert sndfile["licenseDeclared"] == "LGPL-2.1-or-later"


def test_sdk_sbom_excludes_cli_and_test_only_dependencies(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    root = Path(__file__).resolve().parents[3]
    output = tmp_path / "sdk.json"
    monkeypatch.setattr(
        "sys.argv",
        [
            "generate_sbom.py",
            "--axklib-root",
            str(root),
            "--profile",
            "sdk",
            "--output",
            str(output),
        ],
    )
    assert generate_sbom.main() == 0
    names = {item["name"] for item in json.loads(output.read_text())["packages"]}
    assert {"libsndfile", "soxr", "libflac", "libvorbis", "opus"} <= names
    assert names.isdisjoint({"cli11", "hash-library", "gtest"})


def test_package_inspector_rejects_scripts_and_unlisted_shared_libraries(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    package = tmp_path / "package"
    package.mkdir()
    (package / "axklib").write_bytes(b"native")
    monkeypatch.setattr("sys.argv", ["inspect_package.py", str(package)])
    assert inspect_package.main() == 0

    (package / "python3").write_bytes(b"runtime")
    (package / "libsndfile.so").write_bytes(b"library")
    monkeypatch.setattr("sys.argv", ["inspect_package.py", str(package)])
    assert inspect_package.main() == 1

    (package / "python3").unlink()
    (package / "libsndfile.so").unlink()
    monkeypatch.setattr("sys.argv", ["inspect_package.py", str(package)])
    assert inspect_package.main() == 0
