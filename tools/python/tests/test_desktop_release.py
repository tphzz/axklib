from __future__ import annotations

import json
from pathlib import Path

import pytest

from desktop_release import stage_release_packages


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value), encoding="utf-8")


def write_version_metadata(
    path: Path,
    *,
    semantic_version: str = "0.0.0",
    project_version: str = "0.0.0",
    release_tag: str = "",
    is_release: bool = False,
    is_prerelease: bool = False,
) -> None:
    major, minor, patch = (int(value) for value in project_version.split("."))
    write_json(
        path,
        {
            "schema_version": 1,
            "semantic_version": semantic_version,
            "project_version": project_version,
            "major": major,
            "minor": minor,
            "patch": patch,
            "release_tag": release_tag,
            "is_release": is_release,
            "is_prerelease": is_prerelease,
        },
    )


def write_package_basename(path: Path, source_identity: str) -> None:
    path.write_text(f"axklib-{source_identity}\n", encoding="utf-8")


def test_release_staging_normalizes_only_selected_package_formats(tmp_path: Path) -> None:
    bundle = tmp_path / "bundle"
    bundle.mkdir()
    (bundle / "axkdeck.deb").write_bytes(b"deb")
    (bundle / "axkdeck.rpm").write_bytes(b"rpm")
    (bundle / "not-a-package.txt").write_text("ignored", encoding="utf-8")
    native_metadata = tmp_path / "version.json"
    package_basename = tmp_path / "package.txt"
    sbom = tmp_path / "sbom.json"
    write_version_metadata(native_metadata)
    write_package_basename(package_basename, "feature-abc1234")
    write_json(sbom, {"comment": "axklib source identity: feature-abc1234"})

    packages = stage_release_packages(
        bundle_directory=bundle,
        extensions={".deb", ".rpm"},
        output_directory=tmp_path / "output",
        platform="linux",
        architecture="arm64",
        axklib_version_metadata=native_metadata,
        package_basename_file=package_basename,
        sbom=sbom,
        configuration="Release",
    )

    assert [package.name for package in packages] == [
        "axkdeck-feature-abc1234-linux-arm64.deb",
        "axkdeck-feature-abc1234-linux-arm64.rpm",
    ]
    assert sorted(path.name for path in (tmp_path / "output").iterdir()) == [
        "axkdeck-feature-abc1234-linux-arm64.deb",
        "axkdeck-feature-abc1234-linux-arm64.rpm",
    ]


def test_tagged_release_staging_uses_the_complete_semantic_version(tmp_path: Path) -> None:
    bundle = tmp_path / "bundle"
    bundle.mkdir()
    (bundle / "axkdeck.exe").write_bytes(b"installer")
    native_metadata = tmp_path / "version.json"
    package_basename = tmp_path / "package.txt"
    sbom = tmp_path / "sbom.json"
    write_version_metadata(
        native_metadata,
        semantic_version="1.2.3-rc.4+build.9",
        project_version="1.2.3",
        release_tag="v1.2.3-rc.4+build.9",
        is_release=True,
        is_prerelease=True,
    )
    write_package_basename(package_basename, "v1.2.3-rc.4-build.9-a1b2c3d")
    write_json(
        sbom,
        {"comment": "axklib source identity: v1.2.3-rc.4-build.9-a1b2c3d"},
    )

    packages = stage_release_packages(
        bundle_directory=bundle,
        extensions={".exe"},
        output_directory=tmp_path / "output",
        platform="windows",
        architecture="x64",
        axklib_version_metadata=native_metadata,
        package_basename_file=package_basename,
        sbom=sbom,
        configuration="Release",
    )

    assert [package.name for package in packages] == [
        "axkdeck-1.2.3-rc.4+build.9-windows-x64.exe"
    ]


def test_release_metadata_requires_a_requested_package(tmp_path: Path) -> None:
    bundle = tmp_path / "bundle"
    bundle.mkdir()
    native_metadata = tmp_path / "version.json"
    package_basename = tmp_path / "package.txt"
    sbom = tmp_path / "sbom.json"
    write_version_metadata(native_metadata)
    write_package_basename(package_basename, "main-a1b2c3d")
    write_json(sbom, {})

    with pytest.raises(ValueError, match="expected one .dmg desktop package"):
        stage_release_packages(
            bundle_directory=bundle,
            extensions={".dmg"},
            output_directory=tmp_path / "output",
            platform="macos",
            architecture="universal",
            axklib_version_metadata=native_metadata,
            package_basename_file=package_basename,
            sbom=sbom,
            configuration="Release",
        )


def test_release_metadata_requires_sbom_source_identity(tmp_path: Path) -> None:
    bundle = tmp_path / "bundle"
    bundle.mkdir()
    (bundle / "axkdeck.dmg").write_bytes(b"dmg")
    native_metadata = tmp_path / "version.json"
    package_basename = tmp_path / "package.txt"
    sbom = tmp_path / "sbom.json"
    write_version_metadata(native_metadata)
    write_package_basename(package_basename, "main-a1b2c3d")
    write_json(sbom, {"comment": "unrelated comment"})

    with pytest.raises(ValueError, match="does not declare"):
        stage_release_packages(
            bundle_directory=bundle,
            extensions={".dmg"},
            output_directory=tmp_path / "output",
            platform="macos",
            architecture="universal",
            axklib_version_metadata=native_metadata,
            package_basename_file=package_basename,
            sbom=sbom,
            configuration="Release",
        )


def test_release_staging_rejects_duplicate_requested_format(tmp_path: Path) -> None:
    bundle = tmp_path / "bundle"
    bundle.mkdir()
    (bundle / "first.deb").write_bytes(b"first")
    (bundle / "second.deb").write_bytes(b"second")
    (bundle / "axkdeck.rpm").write_bytes(b"rpm")
    native_metadata = tmp_path / "version.json"
    package_basename = tmp_path / "package.txt"
    sbom = tmp_path / "sbom.json"
    write_version_metadata(native_metadata)
    write_package_basename(package_basename, "main-a1b2c3d")
    write_json(sbom, {"comment": "axklib source identity: main-a1b2c3d"})

    with pytest.raises(ValueError, match="expected one .deb desktop package"):
        stage_release_packages(
            bundle_directory=bundle,
            extensions={".deb", ".rpm"},
            output_directory=tmp_path / "output",
            platform="linux",
            architecture="x64",
            axklib_version_metadata=native_metadata,
            package_basename_file=package_basename,
            sbom=sbom,
            configuration="Release",
        )


@pytest.mark.parametrize(
    ("platform", "architecture", "message"),
    [
        ("linux", "universal", "unsupported desktop release target"),
        ("unknown", "x64", "unsupported desktop release target"),
    ],
)
def test_release_staging_rejects_unsafe_identity_fields(
    tmp_path: Path,
    platform: str,
    architecture: str,
    message: str,
) -> None:
    bundle = tmp_path / "bundle"
    bundle.mkdir()
    (bundle / "axkdeck.deb").write_bytes(b"deb")
    native_metadata = tmp_path / "version.json"
    package_basename = tmp_path / "package.txt"
    sbom = tmp_path / "sbom.json"
    write_version_metadata(native_metadata)
    write_package_basename(package_basename, "main-a1b2c3d")
    write_json(sbom, {"comment": "axklib source identity: main-a1b2c3d"})

    with pytest.raises(ValueError, match=message):
        stage_release_packages(
            bundle_directory=bundle,
            extensions={".deb"},
            output_directory=tmp_path / "output",
            platform=platform,
            architecture=architecture,
            axklib_version_metadata=native_metadata,
            package_basename_file=package_basename,
            sbom=sbom,
            configuration="Release",
        )


def test_debug_staging_adds_a_debug_suffix(tmp_path: Path) -> None:
    bundle = tmp_path / "bundle"
    bundle.mkdir()
    (bundle / "axkdeck.exe").write_bytes(b"installer")
    native_metadata = tmp_path / "version.json"
    package_basename = tmp_path / "package.txt"
    sbom = tmp_path / "sbom.json"
    write_version_metadata(native_metadata)
    write_package_basename(package_basename, "main-a1b2c3d")
    write_json(sbom, {"comment": "axklib source identity: main-a1b2c3d"})

    packages = stage_release_packages(
        bundle_directory=bundle,
        extensions={".exe"},
        output_directory=tmp_path / "output",
        platform="windows",
        architecture="arm64",
        axklib_version_metadata=native_metadata,
        package_basename_file=package_basename,
        sbom=sbom,
        configuration="Debug",
    )

    assert packages[0].name == "axkdeck-main-a1b2c3d-windows-arm64-debug.exe"
