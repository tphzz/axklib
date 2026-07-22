from __future__ import annotations

import json
from pathlib import Path

import pytest

from desktop_release import stage_release_packages


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value), encoding="utf-8")


def test_release_staging_normalizes_only_selected_package_formats(tmp_path: Path) -> None:
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

    packages = stage_release_packages(
        bundle_directory=bundle,
        extensions={".deb", ".rpm"},
        output_directory=tmp_path / "output",
        platform="linux",
        architecture="arm64",
        package_json=package_json,
        axklib_version_metadata=native_metadata,
        sbom=sbom,
    )

    assert [package.name for package in packages] == [
        "axkdeck-1.2.3-linux-arm64.deb",
        "axkdeck-1.2.3-linux-arm64.rpm",
    ]
    assert sorted(path.name for path in (tmp_path / "output").iterdir()) == [
        "axkdeck-1.2.3-linux-arm64.deb",
        "axkdeck-1.2.3-linux-arm64.rpm",
    ]


def test_release_metadata_requires_a_requested_package(tmp_path: Path) -> None:
    bundle = tmp_path / "bundle"
    bundle.mkdir()
    package_json = tmp_path / "package.json"
    native_metadata = tmp_path / "version.json"
    sbom = tmp_path / "sbom.json"
    write_json(package_json, {"version": "1.0.0"})
    write_json(native_metadata, {"semantic_version": "1.0.0"})
    write_json(sbom, {})

    with pytest.raises(ValueError, match="expected one .dmg desktop package"):
        stage_release_packages(
            bundle_directory=bundle,
            extensions={".dmg"},
            output_directory=tmp_path / "output",
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
        stage_release_packages(
            bundle_directory=bundle,
            extensions={".dmg"},
            output_directory=tmp_path / "output",
            platform="macos",
            architecture="universal",
            package_json=package_json,
            axklib_version_metadata=native_metadata,
            sbom=sbom,
        )


def test_release_staging_rejects_duplicate_requested_format(tmp_path: Path) -> None:
    bundle = tmp_path / "bundle"
    bundle.mkdir()
    (bundle / "first.deb").write_bytes(b"first")
    (bundle / "second.deb").write_bytes(b"second")
    (bundle / "axkdeck.rpm").write_bytes(b"rpm")
    package_json = tmp_path / "package.json"
    native_metadata = tmp_path / "version.json"
    sbom = tmp_path / "sbom.json"
    write_json(package_json, {"version": "1.0.0"})
    write_json(native_metadata, {"semantic_version": "1.0.0"})
    write_json(sbom, {"comment": "axklib source identity: main-a1b2c3d"})

    with pytest.raises(ValueError, match="expected one .deb desktop package"):
        stage_release_packages(
            bundle_directory=bundle,
            extensions={".deb", ".rpm"},
            output_directory=tmp_path / "output",
            platform="linux",
            architecture="x64",
            package_json=package_json,
            axklib_version_metadata=native_metadata,
            sbom=sbom,
        )


@pytest.mark.parametrize(
    ("version", "platform", "architecture", "message"),
    [
        ("../../escape", "linux", "x64", "invalid axkdeck version"),
        ("1.0.0", "linux", "universal", "unsupported desktop release target"),
    ],
)
def test_release_staging_rejects_unsafe_identity_fields(
    tmp_path: Path,
    version: str,
    platform: str,
    architecture: str,
    message: str,
) -> None:
    bundle = tmp_path / "bundle"
    bundle.mkdir()
    (bundle / "axkdeck.deb").write_bytes(b"deb")
    package_json = tmp_path / "package.json"
    native_metadata = tmp_path / "version.json"
    sbom = tmp_path / "sbom.json"
    write_json(package_json, {"version": version})
    write_json(native_metadata, {"semantic_version": "1.0.0"})
    write_json(sbom, {"comment": "axklib source identity: main-a1b2c3d"})

    with pytest.raises(ValueError, match=message):
        stage_release_packages(
            bundle_directory=bundle,
            extensions={".deb"},
            output_directory=tmp_path / "output",
            platform=platform,
            architecture=architecture,
            package_json=package_json,
            axklib_version_metadata=native_metadata,
            sbom=sbom,
        )
