from __future__ import annotations

import json
from pathlib import Path

import pytest

from desktop_release import write_release_metadata


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value), encoding="utf-8")


def test_release_metadata_hashes_only_selected_package_formats(tmp_path: Path) -> None:
    bundle = tmp_path / "bundle"
    bundle.mkdir()
    (bundle / "axkdeck.deb").write_bytes(b"deb")
    (bundle / "axkdeck.rpm").write_bytes(b"rpm")
    (bundle / "not-a-package.txt").write_text("ignored", encoding="utf-8")
    package_json = tmp_path / "package.json"
    native_metadata = tmp_path / "version.json"
    sbom = tmp_path / "sbom.json"
    write_json(package_json, {"version": "1.2.3"})
    write_json(native_metadata, {"semantic_version": "4.5.6"})
    write_json(sbom, {"comment": "axklib source identity: feature-abc1234"})

    manifest_path, checksums_path = write_release_metadata(
        bundle_directory=bundle,
        extensions={".deb", ".rpm"},
        output_directory=tmp_path / "output",
        commit="0123456789abcdef",
        platform="linux",
        architecture="arm64",
        package_json=package_json,
        axklib_version_metadata=native_metadata,
        sbom=sbom,
    )

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert manifest["version"] == "1.2.3"
    assert manifest["axklib_version"] == "4.5.6"
    assert manifest["axklib_source_identity"] == "feature-abc1234"
    assert manifest["architecture"] == "arm64"
    assert [package["file"] for package in manifest["packages"]] == [
        "axkdeck.deb",
        "axkdeck.rpm",
    ]
    assert "not-a-package.txt" not in checksums_path.read_text(encoding="utf-8")


def test_release_metadata_requires_a_requested_package(tmp_path: Path) -> None:
    bundle = tmp_path / "bundle"
    bundle.mkdir()
    package_json = tmp_path / "package.json"
    native_metadata = tmp_path / "version.json"
    sbom = tmp_path / "sbom.json"
    write_json(package_json, {"version": "1.0.0"})
    write_json(native_metadata, {"semantic_version": "1.0.0"})
    write_json(sbom, {})

    with pytest.raises(ValueError, match="no desktop packages"):
        write_release_metadata(
            bundle_directory=bundle,
            extensions={".dmg"},
            output_directory=tmp_path / "output",
            commit="abc",
            platform="macos",
            architecture="universal",
            package_json=package_json,
            axklib_version_metadata=native_metadata,
            sbom=sbom,
        )


def test_release_metadata_requires_sbom_source_identity(tmp_path: Path) -> None:
    bundle = tmp_path / "bundle"
    bundle.mkdir()
    (bundle / "axkdeck.dmg").write_bytes(b"dmg")
    package_json = tmp_path / "package.json"
    native_metadata = tmp_path / "version.json"
    sbom = tmp_path / "sbom.json"
    write_json(package_json, {"version": "1.0.0"})
    write_json(native_metadata, {"semantic_version": "1.0.0"})
    write_json(sbom, {"comment": "unrelated comment"})

    with pytest.raises(ValueError, match="does not declare"):
        write_release_metadata(
            bundle_directory=bundle,
            extensions={".dmg"},
            output_directory=tmp_path / "output",
            commit="abc",
            platform="macos",
            architecture="universal",
            package_json=package_json,
            axklib_version_metadata=native_metadata,
            sbom=sbom,
        )
