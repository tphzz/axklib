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
    axklib = next(item for item in document["packages"] if item["name"] == "axklib")
    assert axklib["versionInfo"] == "0.1.0"
    assert document["name"] == "axklib-workspace-release"
    assert document["documentNamespace"].startswith(
        "https://github.com/tphzz/axklib/spdx/"
    )


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
    assert {"fatfs", "libsndfile", "soxr", "libflac", "libvorbis", "opus"} <= names
    assert names.isdisjoint({"cli11", "hash-library", "gtest"})


def test_project_version_rejects_manifest_drift(tmp_path: Path) -> None:
    (tmp_path / "CMakeLists.txt").write_text(
        "project(\n  axklib\n  VERSION 1.2.3\n  LANGUAGES CXX\n)\n",
        encoding="utf-8",
    )
    (tmp_path / "vcpkg.json").write_text(
        json.dumps({"version-semver": "1.2.4"}), encoding="utf-8"
    )
    with pytest.raises(ValueError, match="version mismatch"):
        generate_sbom.project_version(tmp_path)


def test_sbom_timestamp_and_namespace_are_reproducible(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("SOURCE_DATE_EPOCH", "0")
    assert generate_sbom.creation_timestamp() == "1970-01-01T00:00:00Z"
    rows = [generate_sbom.package("axklib", "1.0.0", "axklib")]
    first = generate_sbom.sbom_document("sdk", rows, "1970-01-01T00:00:00Z")
    repeated = generate_sbom.sbom_document("sdk", rows, "1970-01-01T00:00:00Z")
    changed_version = generate_sbom.sbom_document(
        "sdk",
        [generate_sbom.package("axklib", "1.0.1", "axklib")],
        "1970-01-01T00:00:00Z",
    )
    changed_profile = generate_sbom.sbom_document(
        "cli", rows, "1970-01-01T00:00:00Z"
    )
    changed_timestamp = generate_sbom.sbom_document(
        "sdk", rows, "1970-01-01T00:00:01Z"
    )
    assert first == repeated
    assert len(
        {
            first["documentNamespace"],
            changed_version["documentNamespace"],
            changed_profile["documentNamespace"],
            changed_timestamp["documentNamespace"],
        }
    ) == 4


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
