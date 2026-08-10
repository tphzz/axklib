from __future__ import annotations

import json
import re
import tarfile
import zipfile
from pathlib import Path

import pytest

import generate_sbom
import inspect_package
import release_metadata
import version_metadata


def action_reference_count(workflow: str, action: str, version: str) -> int:
    return len(
        re.findall(
            rf"uses:\s+{re.escape(action)}@[0-9a-f]{{40}}\s+#\s+{re.escape(version)}(?:\s|$)",
            workflow,
        )
    )


def metadata(
    semantic_version: str = "0.0.0",
    project_version: str = "0.0.0",
    release_tag: str = "",
    *,
    is_prerelease: bool = False,
) -> version_metadata.VersionMetadata:
    major, minor, patch = (int(value) for value in project_version.split("."))
    return version_metadata.VersionMetadata(
        schema_version=1,
        semantic_version=semantic_version,
        project_version=project_version,
        major=major,
        minor=minor,
        patch=patch,
        release_tag=release_tag,
        is_release=bool(release_tag),
        is_prerelease=is_prerelease,
    )


def write_metadata(path: Path, value: version_metadata.VersionMetadata) -> None:
    path.write_text(json.dumps(value.__dict__) + "\n", encoding="utf-8")


def write_package_basename(path: Path, source_identity: str = "main-a1b2c3d") -> None:
    path.write_text(f"axklib-{source_identity}\n", encoding="utf-8")


def write_desktop_license_inputs(directory: Path) -> tuple[Path, Path]:
    crate = directory / "reqwest"
    crate.mkdir()
    (crate / "Cargo.toml").write_text("[package]\nname = \"reqwest\"\n", encoding="utf-8")
    (crate / "LICENSE").write_text("reqwest license text\n", encoding="utf-8")
    cargo = directory / "cargo-metadata.json"
    cargo.write_text(
        json.dumps(
            {
                "packages": [
                    {
                        "id": "path+file:///axkdeck#2.3.4",
                        "name": "axkdeck",
                        "version": "2.3.4",
                        "license": "MIT OR Apache-2.0",
                        "license_file": None,
                        "manifest_path": str(crate / "Cargo.toml"),
                    },
                    {
                        "id": "registry+https://github.com/rust-lang/crates.io-index#reqwest@0.12.0",
                        "name": "reqwest",
                        "version": "0.12.0",
                        "license": "MIT OR Apache-2.0",
                        "license_file": None,
                        "manifest_path": str(crate / "Cargo.toml"),
                    },
                ],
                "resolve": {
                    "nodes": [
                        {"id": "path+file:///axkdeck#2.3.4"},
                        {
                            "id": "registry+https://github.com/rust-lang/crates.io-index#reqwest@0.12.0"
                        },
                    ]
                },
            }
        ),
        encoding="utf-8",
    )
    package = directory / "tauri-api"
    package.mkdir()
    (package / "LICENSE").write_text("Tauri license text\n", encoding="utf-8")
    pnpm = directory / "pnpm-licenses.json"
    pnpm.write_text(
        json.dumps(
            {
                "Apache-2.0 OR MIT": [
                    {
                        "name": "@tauri-apps/api",
                        "versions": ["2.11.1"],
                        "paths": [str(package)],
                    }
                ]
            }
        ),
        encoding="utf-8",
    )
    return cargo, pnpm


def test_pnpm_packages_preserve_scopes_and_separate_peer_context(tmp_path: Path) -> None:
    lockfile = tmp_path / "pnpm-lock.yaml"
    lockfile.write_text(
        """lockfileVersion: '9.0'
packages:
  '@scope/plain@1.2.3': {}
  plain@2.0.0: {}
snapshots:
  '@scope/plain@1.2.3(peer@4.0.0)': {}
  plain@2.0.0(peer@4.0.0): {}
""",
        encoding="utf-8",
    )

    rows = generate_sbom.pnpm_packages(lockfile)

    assert [(row["name"], row["versionInfo"]) for row in rows] == [
        ("@scope/plain", "1.2.3"),
        ("plain", "2.0.0"),
    ]
    assert rows[0]["externalRefs"] == [
        {
            "referenceCategory": "PACKAGE-MANAGER",
            "referenceType": "purl",
            "referenceLocator": "pkg:npm/%40scope/plain@1.2.3",
        }
    ]
    assert rows[0]["comment"] == "pnpm peer contexts: (peer@4.0.0)"


@pytest.mark.parametrize(
    "key",
    ["'@scope/name@1.0.0'", "@scope/name", "plain", "@scope@1.0.0"],
)
def test_pnpm_identity_rejects_malformed_keys(key: str) -> None:
    with pytest.raises(ValueError, match="pnpm package identity"):
        generate_sbom.parse_pnpm_identity(key)


def test_sbom_includes_base_cli_and_test_dependencies(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    root = Path(__file__).resolve().parents[3]
    output = tmp_path / "sbom.json"
    version_file = tmp_path / "version.json"
    package_file = tmp_path / "package-basename.txt"
    write_metadata(version_file, metadata("1.2.3-rc.1", "1.2.3", "v1.2.3-rc.1", is_prerelease=True))
    write_package_basename(package_file, "v1.2.3-rc.1-a1b2c3d")
    monkeypatch.setattr(
        "sys.argv",
        [
            "generate_sbom.py",
            "--axklib-root",
            str(root),
            "--version-metadata-file",
            str(version_file),
            "--package-basename-file",
            str(package_file),
            "--output",
            str(output),
        ],
    )
    assert generate_sbom.main() == 0
    document = json.loads(output.read_text(encoding="utf-8"))
    names = {item["name"] for item in document["packages"]}
    assert {
        "axklib",
        "asio",
        "cli11",
        "crow",
        "gtest",
        "hash-library",
        "libsndfile",
        "soxr",
    } <= names
    sndfile = next(item for item in document["packages"] if item["name"] == "libsndfile")
    assert sndfile["versionInfo"].startswith("1.2.2")
    assert sndfile["licenseDeclared"] == "LGPL-2.1-or-later"
    axklib = next(item for item in document["packages"] if item["name"] == "axklib")
    assert axklib["versionInfo"] == "1.2.3-rc.1"
    assert document["name"] == "axklib-workspace-release"
    assert document["documentNamespace"].startswith("https://github.com/tphzz/axklib/spdx/")
    assert document["comment"] == "axklib source identity: v1.2.3-rc.1-a1b2c3d"


def test_sdk_sbom_excludes_cli_and_test_only_dependencies(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    root = Path(__file__).resolve().parents[3]
    output = tmp_path / "sdk.json"
    version_file = tmp_path / "version.json"
    package_file = tmp_path / "package-basename.txt"
    write_metadata(version_file, metadata())
    write_package_basename(package_file)
    monkeypatch.setattr(
        "sys.argv",
        [
            "generate_sbom.py",
            "--axklib-root",
            str(root),
            "--profile",
            "sdk",
            "--version-metadata-file",
            str(version_file),
            "--package-basename-file",
            str(package_file),
            "--output",
            str(output),
        ],
    )
    assert generate_sbom.main() == 0
    names = {item["name"] for item in json.loads(output.read_text())["packages"]}
    assert {"fatfs", "libsndfile", "soxr", "libflac", "libvorbis", "opus"} <= names
    assert names.isdisjoint({"cli11", "hash-library", "gtest"})


def test_server_sbom_includes_crow_without_cli_or_test_dependencies(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    root = Path(__file__).resolve().parents[3]
    output = tmp_path / "server.json"
    version_file = tmp_path / "version.json"
    package_file = tmp_path / "package-basename.txt"
    write_metadata(version_file, metadata())
    write_package_basename(package_file)
    monkeypatch.setattr(
        "sys.argv",
        [
            "generate_sbom.py",
            "--axklib-root",
            str(root),
            "--profile",
            "server",
            "--version-metadata-file",
            str(version_file),
            "--package-basename-file",
            str(package_file),
            "--output",
            str(output),
        ],
    )
    assert generate_sbom.main() == 0
    names = {item["name"] for item in json.loads(output.read_text())["packages"]}
    assert {"axklib", "asio", "crow", "hash-library", "libsndfile", "soxr"} <= names
    assert names.isdisjoint({"cli11", "gtest"})
    crow = next(
        item for item in json.loads(output.read_text())["packages"] if item["name"] == "crow"
    )
    asio = next(
        item for item in json.loads(output.read_text())["packages"] if item["name"] == "asio"
    )
    assert crow["versionInfo"] == "1.3.3"
    assert crow["licenseDeclared"] == "BSD-3-Clause"
    assert asio["versionInfo"] == "1.32.0"
    assert asio["licenseDeclared"] == "BSL-1.0"
    assert "source SHA512: c270425953d84c5f" in crow["comment"]
    assert "source SHA512: 9374ff97bd4af7b5" in asio["comment"]


def test_desktop_sbom_uses_the_shared_monorepo_version(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    root = Path(__file__).resolve().parents[3]
    output = tmp_path / "desktop.json"
    version_file = tmp_path / "version.json"
    package_file = tmp_path / "package-basename.txt"
    cargo_metadata, pnpm_licenses = write_desktop_license_inputs(tmp_path)
    write_metadata(version_file, metadata("2.3.4", "2.3.4", "v2.3.4"))
    write_package_basename(package_file, "v2.3.4-a1b2c3d")
    monkeypatch.setattr(
        "sys.argv",
        [
            "generate_sbom.py",
            "--axklib-root",
            str(root),
            "--axkdeck-root",
            str(root / "apps/axkdeck"),
            "--profile",
            "server",
            "--cargo-metadata-file",
            str(cargo_metadata),
            "--pnpm-license-file",
            str(pnpm_licenses),
            "--version-metadata-file",
            str(version_file),
            "--package-basename-file",
            str(package_file),
            "--output",
            str(output),
        ],
    )

    assert generate_sbom.main() == 0
    document = json.loads(output.read_text(encoding="utf-8"))
    desktop = [item for item in document["packages"] if item["name"] == "axkdeck"]
    assert document["name"] == "axkdeck-release"
    assert document["comment"] == "axklib source identity: v2.3.4-a1b2c3d"
    assert len(desktop) == 1
    assert desktop[0]["versionInfo"] == "2.3.4"
    assert {
        ("reqwest", "MIT OR Apache-2.0"),
        ("@tauri-apps/api", "Apache-2.0 OR MIT"),
    } <= {(item["name"], item["licenseDeclared"]) for item in document["packages"]}
    assert all(item["licenseDeclared"] != "NOASSERTION" for item in document["packages"])


def test_license_bundle_is_deterministic_and_rejects_unresolved_packages(
    tmp_path: Path,
) -> None:
    material = tmp_path / "LICENSE"
    material.write_text("license text\n", encoding="utf-8")
    packages = [generate_sbom.package("dependency", "1.0", "test", license_expression="MIT")]
    output = tmp_path / "THIRD-PARTY-LICENSES.txt"

    generate_sbom.write_license_bundle(
        output,
        packages,
        [
            generate_sbom.LicenseMaterial("dependency", material),
            generate_sbom.LicenseMaterial("duplicate", material),
        ],
    )

    text = output.read_text(encoding="utf-8")
    assert "- dependency 1.0: MIT" in text
    assert text.count("license text") == 1
    with pytest.raises(ValueError, match="unresolved licenses"):
        generate_sbom.write_license_bundle(
            output, [generate_sbom.package("unknown", "1.0", "test")], []
        )


def test_version_metadata_rejects_inconsistent_values(tmp_path: Path) -> None:
    path = tmp_path / "version.json"
    write_metadata(path, metadata("1.2.3", "1.2.4", "v1.2.3"))
    with pytest.raises(ValueError, match="numeric core"):
        version_metadata.read(path)


def test_sbom_timestamp_and_namespace_are_reproducible(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("SOURCE_DATE_EPOCH", "0")
    assert generate_sbom.creation_timestamp() == "1970-01-01T00:00:00Z"
    rows = [generate_sbom.package("axklib", "1.0.0", "axklib")]
    first = generate_sbom.sbom_document("sdk", "main-a1b2c3d", rows, "1970-01-01T00:00:00Z")
    repeated = generate_sbom.sbom_document("sdk", "main-a1b2c3d", rows, "1970-01-01T00:00:00Z")
    changed_version = generate_sbom.sbom_document(
        "sdk",
        "main-a1b2c3d",
        [generate_sbom.package("axklib", "1.0.1", "axklib")],
        "1970-01-01T00:00:00Z",
    )
    changed_profile = generate_sbom.sbom_document(
        "cli", "main-a1b2c3d", rows, "1970-01-01T00:00:00Z"
    )
    changed_source = generate_sbom.sbom_document(
        "sdk", "feature-a1b2c3d", rows, "1970-01-01T00:00:00Z"
    )
    changed_timestamp = generate_sbom.sbom_document(
        "sdk", "main-a1b2c3d", rows, "1970-01-01T00:00:01Z"
    )
    assert first == repeated
    assert (
        len(
            {
                first["documentNamespace"],
                changed_version["documentNamespace"],
                changed_profile["documentNamespace"],
                changed_source["documentNamespace"],
                changed_timestamp["documentNamespace"],
            }
        )
        == 5
    )


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


def test_release_metadata_uses_source_identity_and_debug_suffix(tmp_path: Path) -> None:
    package_file = tmp_path / "package_basename.txt"
    package_file.write_text("axklib-feature-audio-a1b2c3d\n", encoding="utf-8")
    package = release_metadata.read_package_basename(package_file)
    release = release_metadata.artifact_metadata(
        package, metadata(), "branch", "feature/audio", "linux-x64", "Release"
    )
    debug = release_metadata.artifact_metadata(
        package, metadata(), "branch", "feature/audio", "windows-arm64", "Debug"
    )
    assert release.artifact_stem == "axklib-feature-audio-a1b2c3d-linux-x64"
    assert release.sdk_artifact_stem == "axklib-sdk-feature-audio-a1b2c3d-linux-x64"
    assert release.cli_artifact_stem == "axklib-cli-feature-audio-a1b2c3d-linux-x64"
    assert debug.artifact_stem == "axklib-feature-audio-a1b2c3d-windows-arm64-debug"
    assert debug.sdk_artifact_stem.endswith("-windows-arm64-debug")
    assert debug.cli_artifact_stem.endswith("-windows-arm64-debug")


def test_release_metadata_shortens_only_exact_project_tag() -> None:
    version = metadata("1.2.3-rc.1+build.4", "1.2.3", "v1.2.3-rc.1+build.4", is_prerelease=True)
    artifact = release_metadata.artifact_metadata(
        "axklib-v1.2.3-rc.1-build.4-a1b2c3d",
        version,
        "tag",
        "v1.2.3-rc.1+build.4",
        "macos-universal",
        "Release",
    )
    assert artifact.artifact_stem == "axklib-1.2.3-rc.1+build.4-macos-universal"
    with pytest.raises(ValueError, match="release tag must be v1.2.3-rc.1[+]build.4"):
        release_metadata.artifact_metadata(
            "axklib-nightly-a1b2c3d",
            version,
            "tag",
            "nightly",
            "linux-arm64",
            "Release",
        )


def test_release_target_uses_preview_for_branches_and_preserves_tags() -> None:
    branch = release_metadata.draft_release_target("branch", "features/packages", metadata())
    assert branch == release_metadata.DraftReleaseTarget(
        tag_name="features/packages-preview",
        title="features/packages-preview",
        cleanup_tag=True,
        verify_tag=False,
        prerelease=True,
    )
    stable = metadata("2.0.0", "2.0.0", "v2.0.0")
    tag = release_metadata.draft_release_target("tag", "v2.0.0", stable)
    assert tag == release_metadata.DraftReleaseTarget(
        tag_name="v2.0.0",
        title="v2.0.0",
        cleanup_tag=False,
        verify_tag=True,
        prerelease=False,
    )
    prerelease = metadata("2.1.0-beta.1", "2.1.0", "v2.1.0-beta.1", is_prerelease=True)
    assert release_metadata.draft_release_target("tag", "v2.1.0-beta.1", prerelease).prerelease
    with pytest.raises(ValueError, match="single line"):
        release_metadata.draft_release_target("branch", "", metadata())
    with pytest.raises(ValueError, match="unsupported"):
        release_metadata.draft_release_target("pull_request", "123", metadata())


def write_native_archive(path: Path, component: str) -> None:
    files = {
        "LICENSE": b"project license",
        "licenses/dependency/copyright": b"dependency license",
    }
    if component == "sdk":
        files.update({"include/axklib/sdk.hpp": b"header", "lib/libaxklib.so": b"library"})
    else:
        files["bin/axklib"] = b"executable"

    if path.suffix == ".zip":
        with zipfile.ZipFile(path, "w") as archive:
            for name, contents in files.items():
                archive.writestr(name, contents)
    else:
        source = path.parent / f"{path.name}.contents"
        for name, contents in files.items():
            target = source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(contents)
        with tarfile.open(path, "w:gz") as archive:
            for item in sorted(source.rglob("*")):
                archive.add(item, arcname=item.relative_to(source))


def test_native_archives_are_component_specific_and_compact(tmp_path: Path) -> None:
    source = tmp_path / "install"
    (source / "include/axklib").mkdir(parents=True)
    (source / "lib").mkdir()
    (source / "bin").mkdir()
    (source / "share/licenses/axklib").mkdir(parents=True)
    (source / "share/licenses/dependency").mkdir()
    (source / "share/axklib").mkdir(parents=True)
    (source / "include/axklib/sdk.hpp").write_text("header", encoding="utf-8")
    (source / "lib/libaxklib.so").write_bytes(b"library")
    (source / "bin/axklib.dll").write_bytes(b"library")
    (source / "share/licenses/axklib/LICENSE").write_text("project", encoding="utf-8")
    (source / "share/licenses/dependency/copyright").write_text("third party", encoding="utf-8")
    (source / "share/axklib/axklib.spdx.json").write_text("{}", encoding="utf-8")

    archive = release_metadata.package_native_component(
        source_directory=source,
        output_directory=tmp_path / "output",
        artifact_stem="axklib-sdk-main-a1b2c3d-windows-x64",
        component="sdk",
        archive_format="zip",
    )

    with zipfile.ZipFile(archive) as package:
        names = set(package.namelist())
    assert "include/axklib/sdk.hpp" in names
    assert "lib/libaxklib.so" in names
    assert "bin/axklib.dll" in names
    assert "LICENSE" in names
    assert "licenses/dependency/copyright" in names
    assert not any(name.startswith("share/") for name in names)
    assert "licenses/axklib/LICENSE" not in names


def test_release_assets_require_exact_native_and_desktop_deliverables(tmp_path: Path) -> None:
    assets: list[Path] = []
    for platform, extension in release_metadata.RELEASE_ASSET_EXTENSIONS.items():
        for component in release_metadata.NATIVE_RELEASE_COMPONENTS:
            archive = tmp_path / f"axklib-{component}-main-a1b2c3d-{platform}{extension}"
            write_native_archive(archive, component)
            assets.append(archive)

    for platform, architecture, extension in release_metadata.DESKTOP_RELEASE_TARGETS:
        package = tmp_path / f"axkdeck-main-a1b2c3d-{platform}-{architecture}{extension}"
        package.write_bytes(f"{platform}-{architecture}".encode())
        assets.append(package)

    assert len(assets) == 17
    assert release_metadata.verify_release_assets(tmp_path) == sorted(assets)
    unexpected = tmp_path / "unexpected.txt"
    unexpected.write_text("unexpected", encoding="utf-8")
    with pytest.raises(ValueError, match="unexpected release assets"):
        release_metadata.verify_release_assets(tmp_path)


def write_complete_release_asset_set(directory: Path) -> list[Path]:
    for platform, extension in release_metadata.RELEASE_ASSET_EXTENSIONS.items():
        for component in release_metadata.NATIVE_RELEASE_COMPONENTS:
            archive = directory / f"axklib-{component}-main-a1b2c3d-{platform}{extension}"
            write_native_archive(archive, component)
    packages: list[Path] = []
    for platform, architecture, extension in release_metadata.DESKTOP_RELEASE_TARGETS:
        package = directory / f"axkdeck-main-a1b2c3d-{platform}-{architecture}{extension}"
        package.write_bytes(b"installer")
        packages.append(package)
    return packages


def test_release_assets_reject_missing_installer(tmp_path: Path) -> None:
    packages = write_complete_release_asset_set(tmp_path)
    packages[-1].unlink()

    with pytest.raises(ValueError, match="expected one macos-universal desktop package"):
        release_metadata.verify_release_assets(tmp_path)


def test_release_assets_reject_desktop_identity_that_differs_from_native(tmp_path: Path) -> None:
    packages = write_complete_release_asset_set(tmp_path)
    package = packages[0]
    package.rename(package.with_name(package.name.replace("main-a1b2c3d", "0.1.0")))

    with pytest.raises(ValueError, match="different identities"):
        release_metadata.verify_release_assets(tmp_path)


def test_release_assets_reject_share_content(tmp_path: Path) -> None:
    write_complete_release_asset_set(tmp_path)
    sdk = tmp_path / "axklib-sdk-main-a1b2c3d-windows-x64.zip"
    with zipfile.ZipFile(sdk, "a") as archive:
        archive.writestr("share/axklib/axklib.spdx.json", "{}")

    with pytest.raises(ValueError, match="contains share/"):
        release_metadata.verify_release_assets(tmp_path)


def test_native_workflow_is_manual_and_creates_only_release_drafts() -> None:
    root = Path(__file__).resolve().parents[3]
    workflow = (root / ".github/workflows/native.yml").read_text(encoding="utf-8")
    assert 'VCPKG_DEFAULT_BINARY_CACHE=$RUNNER_TEMP/vcpkg/archives' in workflow
    assert "path: ${{ runner.temp }}/vcpkg/archives" in workflow
    assert "VCPKG_DEFAULT_BINARY_CACHE: ${{ runner.temp }}" not in workflow
    assert "VCPKG_DEFAULT_BINARY_CACHE: ${{ github.workspace }}/.." not in workflow
    assert "  workflow_dispatch:" in workflow
    assert "  pull_request:" not in workflow
    assert "  push:" not in workflow
    assert "  schedule:" not in workflow
    assert "draft-release:" in workflow
    assert "if: ${{ github.event_name == 'workflow_dispatch' && !inputs.debug }}" in workflow
    assert "concurrency:" not in workflow.split("jobs:", 1)[0]
    assert "macos-slices:\n    name: macOS ARM64 and Intel slices" in workflow
    assert "macos-universal:\n    name: macOS universal" in workflow
    assert action_reference_count(workflow, "actions/download-artifact", "v8") == 3
    assert "gh release create" in workflow
    assert "--draft" in workflow
    assert "version_metadata.json" in workflow
    assert '"libaxklib.so.${project_version}"' in workflow
    assert '"libaxklib.${project_version}.dylib"' in workflow


def test_native_workflow_does_not_retain_legacy_windows_pfx_signing() -> None:
    root = Path(__file__).resolve().parents[3]
    workflow = (root / ".github/workflows/native.yml").read_text(encoding="utf-8")

    assert "sign_windows_desktop" not in workflow
    assert "WINDOWS_CERTIFICATE" not in workflow
    assert "certificateThumbprint" not in workflow
    assert "Import-PfxCertificate" not in workflow


def test_native_workflow_uses_official_dependency_and_incremental_build_caches() -> None:
    root = Path(__file__).resolve().parents[3]
    workflow = (root / ".github/workflows/native.yml").read_text(encoding="utf-8")

    assert action_reference_count(workflow, "actions/cache", "v6") >= 4
    assert "sccache" not in workflow.lower()
    assert "cold_build" not in workflow
    assert "git rev-parse HEAD:external/vcpkg" in workflow
    assert "CMakePresets.json" in workflow
    assert "library/cmake/triplets/**" in workflow
    assert "library/cmake/ports/**" in workflow
    assert "key: vcpkg-v2-${{ matrix.triplet }}-" in workflow
    assert "vcpkg-v2-${{ matrix.triplet }}-${{ steps.cache-inputs.outputs.vcpkg_revision }}-" in workflow
    assert "key: native-v3-${{ matrix.triplet }}-" in workflow
    assert "key: axkdeck-rust-v1-${{ matrix.artifact }}-" in workflow
    assert "key: axkdeck-rust-v2-macos-universal-" in workflow
    assert "${{ steps.native-toolchain.outputs.toolchain_fingerprint }}" in workflow
    assert "${{ steps.cache-inputs.outputs.dependency_fingerprint }}-${{ github.sha }}" in workflow
    assert "!build/native/${{ inputs.debug && 'debug' || 'release' }}/vcpkg_installed/**" not in workflow
    assert "!build/native/${{ inputs.debug && 'debug' || 'release' }}/Testing/**" in workflow
    assert "tools/python/native_build_cache.py fingerprint" in workflow
    assert "tools/python/native_build_cache.py prepare" in workflow
    assert "tools/python/native_build_cache.py finalize" in workflow
    assert """      - name: Build native targets
        shell: bash
        run: cmake --build --preset ${{ inputs.debug && 'debug' || 'release' }}

      - name: Verify source tree remains clean
        shell: bash
        run: |
          status="$(git status --porcelain --untracked-files=normal)"
          if [ -n "$status" ]; then
            printf 'CI source tree is modified:\\n%s\\n' "$status" >&2
            exit 1
          fi

      - name: Finalize native incremental build cache
""" in workflow
    assert workflow.index(
        "      - name: Save native incremental build cache after failure"
    ) > workflow.index("      - name: Upload Linux or Windows CLI archive")
    assert action_reference_count(workflow, "actions/cache/save", "v6") == 2
    assert "Save vcpkg binary cache after failure" in workflow
    assert "Save native incremental build cache after failure" in workflow
    assert workflow.count("steps.configure-native.outcome == 'success'") == 1
    assert workflow.count("steps.finalize-native-cache.outcome == 'success'") == 1
    assert (
        "if: ${{ failure() && steps.finalize-native-cache.outcome == 'success' "
        "&& steps.native-build-cache.outputs.cache-hit != 'true' }}"
    ) in workflow
    assert workflow.count("continue-on-error: true") == 2


def test_native_workflow_builds_monorepo_desktop_packages_from_tested_servers() -> None:
    root = Path(__file__).resolve().parents[3]
    workflow = (root / ".github/workflows/native.yml").read_text(encoding="utf-8")
    macos_helper = (root / "tools/release/build_macos_slices.sh").read_text(
        encoding="utf-8"
    )

    assert "desktop-static:" in workflow
    assert "needs:\n      - release-tools\n      - desktop-static" in workflow
    assert action_reference_count(workflow, "astral-sh/setup-uv", "v8.3.2") == 4
    assert (
        workflow.count("uv --project tools/python run python tools/python/generate_sbom.py") == 5
    )
    assert "AXKLIB_SERVER_BINARY=$server" in workflow
    assert "AXKLIB_VERSION_METADATA_FILE=$root/version_metadata.json" in workflow
    assert "AXKLIB_PACKAGE_BASENAME_FILE=$root/package_basename.txt" in workflow
    assert "pnpm version:test" in workflow
    assert 'pnpm desktop:build -- --target "$rust_target" --no-bundle' in macos_helper
    assert workflow.count("pnpm desktop:build") + macos_helper.count("pnpm desktop:build") >= 4
    assert "pnpm tauri build" not in workflow
    assert "--package-json" not in workflow
    assert workflow.count("--package-basename-file") >= 8
    assert workflow.count("--configuration") >= 5
    assert '--extension .deb --extension .rpm' in workflow
    assert '--extension .exe' in workflow
    assert '--extension .dmg' in workflow
    assert "name: release-axkdeck-${{ matrix.artifact }}" in workflow
    assert "name: release-axkdeck-macos-universal" in workflow
    assert "name: release-${{ steps.package.outputs.sdk_artifact_stem }}" in workflow
    assert "name: release-${{ steps.package.outputs.cli_artifact_stem }}" in workflow
    assert "pattern: release-*" in workflow
    assert "SHA256SUMS" not in workflow
    assert "combined Linux or Windows distribution" not in workflow
    assert "pnpm desktop:build -- --target universal-apple-darwin" in workflow
    assert "lipo \"$sidecar\" -verify_arch x86_64 arm64" in workflow
    assert action_reference_count(workflow, "pnpm/action-setup", "v6") == 4
    assert "# v4" not in "\n".join(
        line for line in workflow.splitlines() if "pnpm/action-setup@" in line
    )
    assert "if-no-files-found: error" in workflow


def test_clang_tidy_preset_uses_clang_compile_commands() -> None:
    root = Path(__file__).resolve().parents[3]
    presets = json.loads((root / "CMakePresets.json").read_text(encoding="utf-8"))
    clang_tidy = next(
        preset for preset in presets["configurePresets"] if preset["name"] == "clang-tidy"
    )

    assert clang_tidy["cacheVariables"]["CMAKE_C_COMPILER"] == "clang"
    assert clang_tidy["cacheVariables"]["CMAKE_CXX_COMPILER"] == "clang++"
    assert clang_tidy["cacheVariables"]["CMAKE_CXX_CLANG_TIDY"] == "clang-tidy"


def test_desktop_contract_and_rpm_inspection_are_cross_platform() -> None:
    root = Path(__file__).resolve().parents[3]
    attributes = (root / ".gitattributes").read_text(encoding="utf-8")
    prettier = json.loads((root / "apps/axkdeck/.prettierrc.json").read_text(encoding="utf-8"))
    workflow = (root / ".github/workflows/native.yml").read_text(encoding="utf-8")

    assert "/apps/axkdeck/src/lib/generated/axklibApiV1.ts text eol=lf" in attributes
    assert prettier["endOfLine"] == "lf"
    assert "libarchive-tools" in workflow
    assert 'rpm -Kv "$rpm"' in workflow
    assert 'bsdtar -xf "$GITHUB_WORKSPACE/$rpm" -C "$scan/rpm"' in workflow
    assert "rpm2cpio" not in workflow


def test_windows_desktop_bundle_uses_branded_gui_startup() -> None:
    root = Path(__file__).resolve().parents[3]
    desktop = root / "apps/axkdeck"
    configuration = json.loads(
        (desktop / "src-tauri/tauri.conf.json").read_text(encoding="utf-8")
    )
    nsis = configuration["bundle"]["windows"]["nsis"]

    assert nsis["installerIcon"] == "icons/icon.ico"
    assert nsis["uninstallerIcon"] == "icons/icon.ico"
    assert (desktop / "src-tauri" / nsis["installerIcon"]).is_file()

    main = (desktop / "src-tauri/src/main.rs").read_text(encoding="utf-8")
    assert '#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]' in main
    sidecar = (desktop / "src-tauri/src/server_sidecar.rs").read_text(encoding="utf-8")
    assert "command.creation_flags(CREATE_NO_WINDOW)" in sidecar

    index = (desktop / "index.html").read_text(encoding="utf-8")
    assert "#app:empty::before" in index
    assert 'content: "Loading axkdeck..."' in index
    assert "background: #111315" in index


def test_native_workflow_transfers_macos_slices_as_run_scoped_artifacts() -> None:
    root = Path(__file__).resolve().parents[3]
    workflow = (root / ".github/workflows/native.yml").read_text(encoding="utf-8")
    macos_helper = (root / "tools/release/build_macos_slices.sh").read_text(
        encoding="utf-8"
    )

    slices_job = workflow.split("  macos-slices:\n", 1)[1].split(
        "  macos-universal:\n", 1
    )[0]
    universal_job = workflow.split("  macos-universal:\n", 1)[1].split(
        "  draft-release:\n", 1
    )[0]
    assert "bash tools/release/build_macos_slices.sh" in slices_job
    assert "name: slice-macos-x64-${{ github.run_id }}" in slices_job
    assert "name: slice-macos-arm64-${{ github.run_id }}" in slices_job
    assert "retention-days: 2" in slices_job
    assert "name: slice-macos-x64-${{ github.run_id }}" in universal_job
    assert "name: slice-macos-arm64-${{ github.run_id }}" in universal_job
    assert "actions/cache/restore" not in universal_job
    assert "macos-x86_64-${{ github.run_id }}-${{ github.run_attempt }}" not in workflow
    assert "AXK_MACOS_CARGO_ROOT: ${{ runner.temp }}" in slices_job
    assert "src-tauri/resources/axkdeck.spdx.json" not in macos_helper
    assert 'local cargo_root="${AXK_MACOS_CARGO_ROOT:-' in macos_helper
    assert "-type f -o -type l" in macos_helper


def test_workflows_use_only_the_required_self_hosted_runner_labels() -> None:
    root = Path(__file__).resolve().parents[3]
    native_workflow = (root / ".github/workflows/native.yml").read_text(encoding="utf-8")
    docs_workflow = (root / ".github/workflows/publish-docs.yml").read_text(encoding="utf-8")

    for workflow in (native_workflow, docs_workflow):
        assert not re.search(r"(?m)^\s+(?:runner|runs-on): (?:ubuntu|windows|macos)-", workflow)
    assert "runs-on: [self-hosted, Linux, X64, jammy, docker]" in native_workflow
    assert "runs-on: ${{ fromJSON(matrix.runner_labels) }}" in native_workflow
    for labels in (
        '["self-hosted","Linux","X64","jammy","docker"]',
        '["self-hosted","Linux","ARM64","jammy","docker"]',
        '["self-hosted","Windows","X64"]',
        '["self-hosted","Windows","ARM64"]',
    ):
        assert native_workflow.count(f"runner_labels: '{labels}'") == 1
    assert native_workflow.count("runs-on: [self-hosted, macOS, ARM64]") == 2
    assert "runs-on: [self-hosted, Linux, X64, jammy, docker]" in docs_workflow


def test_workflows_cancel_builds_but_serialize_signing_and_publication() -> None:
    root = Path(__file__).resolve().parents[3]
    native_workflow = (root / ".github/workflows/native.yml").read_text(encoding="utf-8")
    docs_workflow = (root / ".github/workflows/publish-docs.yml").read_text(encoding="utf-8")

    architecture = native_workflow.split("  architecture:\n", 1)[1].split(
        "  release-tools:\n", 1
    )[0]
    release_tools = native_workflow.split("  release-tools:\n", 1)[1].split(
        "  desktop-static:\n", 1
    )[0]
    desktop_static = native_workflow.split("  desktop-static:\n", 1)[1].split(
        "  native:\n", 1
    )[0]
    native = native_workflow.split("  native:\n", 1)[1].split(
        "  macos-slices:\n", 1
    )[0]
    macos_slices = native_workflow.split("  macos-slices:\n", 1)[1].split(
        "  macos-universal:\n", 1
    )[0]
    macos_universal = native_workflow.split("  macos-universal:\n", 1)[1].split(
        "  draft-release:\n", 1
    )[0]
    draft_release = native_workflow.split("  draft-release:\n", 1)[1]

    for cancelable_job in (
        architecture,
        release_tools,
        desktop_static,
        native,
        macos_slices,
    ):
        assert "concurrency:" in cancelable_job
        assert "cancel-in-progress: true" in cancelable_job
    assert "native-${{ matrix.artifact }}-${{ github.ref }}" in native
    assert "group: native-macos-signing" in macos_universal
    assert "cancel-in-progress: false" in macos_universal
    assert "group: native-draft-release" in draft_release
    assert "cancel-in-progress: false" in draft_release
    assert "group: pages" in docs_workflow
    assert "cancel-in-progress: true" in docs_workflow


def test_windows_and_macos_self_hosted_preflights_are_explicit() -> None:
    root = Path(__file__).resolve().parents[3]
    workflow = (root / ".github/workflows/native.yml").read_text(encoding="utf-8")
    native_job = workflow.split("  native:\n", 1)[1].split("  macos-slices:\n", 1)[0]
    macos_slices = workflow.split("  macos-slices:\n", 1)[1].split(
        "  macos-universal:\n", 1
    )[0]

    assert "actions/setup-python@5fda3b95a4ea91299a34e894583c3862153e4b97 # v7.0.0" in workflow
    assert 'python-version: "3.13.14"' in workflow
    assert "name: Expose Git Bash to subsequent steps" in native_job
    assert "shell: powershell" in native_job
    assert "Get-Command git.exe" in native_job
    assert "'bin\\bash.exe'" in native_job
    assert ").FullName" in native_job
    assert "Split-Path $bash -Parent | Out-File" in native_job
    assert "& $bash --version" in native_job
    assert "$bash.Source" not in native_job
    assert "python -c \"import platform,sys;" in native_job
    assert "name: Install Linux C++23 toolchain" in native_job
    assert "https://apt.llvm.org/llvm-snapshot.gpg.key" in native_job
    assert "llvm-toolchain-jammy-18 main" in native_job
    assert "clang-18" in native_job
    assert "libc++-18-dev" in native_job
    assert "libc++abi-18-dev" in native_job
    assert "library/cmake/toolchains/LinuxClang18Libcxx.cmake" in native_job
    assert native_job.index("name: Install Linux C++23 toolchain") < native_job.index(
        "name: Fingerprint native toolchain"
    )
    assert "DEVELOPER_DIR: /Applications/Xcode.app/Contents/Developer" in macos_slices
    assert 'MACOSX_DEPLOYMENT_TARGET: "13.3"' in macos_slices
    assert 'MACOSX_DEPLOYMENT_TARGET: "10.15"' not in workflow
    assert 'xcrun --sdk macosx clang -arch x86_64' in macos_slices
    assert '/usr/bin/arch -x86_64 "$verification_binary"' in macos_slices

    desktop_configuration = json.loads(
        (root / "apps/axkdeck/src-tauri/tauri.conf.json").read_text(encoding="utf-8")
    )
    assert desktop_configuration["bundle"]["macOS"]["minimumSystemVersion"] == "13.3"
    assert 'Print :LSMinimumSystemVersion' in workflow
    assert 'xcrun vtool -show-build -arch "$architecture" "$binary"' in workflow

    linux_toolchain = (
        root / "library/cmake/toolchains/LinuxClang18Libcxx.cmake"
    ).read_text(encoding="utf-8")
    assert "find_program(AXK_CLANG_18 clang-18 REQUIRED)" in linux_toolchain
    assert "find_program(AXK_CLANGXX_18 clang++-18 REQUIRED)" in linux_toolchain
    assert 'set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")' in linux_toolchain
    assert "-nostdlib++" not in linux_toolchain


def test_self_hosted_tool_jobs_do_not_depend_on_runner_node_or_python() -> None:
    root = Path(__file__).resolve().parents[3]
    workflow = (root / ".github/workflows/native.yml").read_text(encoding="utf-8")
    release_tools = workflow.split("  release-tools:\n", 1)[1].split(
        "  desktop-static:\n", 1
    )[0]
    desktop_static = workflow.split("  desktop-static:\n", 1)[1].split(
        "  native:\n", 1
    )[0]
    native = workflow.split("  native:\n", 1)[1].split("  macos-slices:\n", 1)[0]
    macos_slices = workflow.split("  macos-slices:\n", 1)[1].split(
        "  macos-universal:\n", 1
    )[0]
    macos_universal = workflow.split("  macos-universal:\n", 1)[1].split(
        "  draft-release:\n", 1
    )[0]

    setup_python = "actions/setup-python@5fda3b95a4ea91299a34e894583c3862153e4b97"
    setup_uv = "astral-sh/setup-uv@11f9893b081a58869d3b5fccaea48c9e9e46f990"
    raw_python = "python tools/python/release_metadata.py release-target"
    assert setup_python in release_tools
    assert 'python-version: "3.13.14"' in release_tools
    assert release_tools.index(setup_python) < release_tools.index(setup_uv)
    assert release_tools.index(setup_python) < release_tools.index(raw_python)

    node_then_standalone_pnpm = re.compile(
        r"      - uses: actions/setup-node@249970729cb0ef3589644e2896645e5dc5ba9c38 # v6\n"
        r"        with:\n"
        r"          node-version: 22\n\n?"
        r"      - uses: pnpm/action-setup@0ebf47130e4866e96fce0953f49152a61190b271 # v6\n"
        r"        with:\n"
        r"          version: 10\.15\.1\n"
        r"          standalone: true\n"
        r"          cache: true\n"
        r"          cache_dependency_path: apps/axkdeck/pnpm-lock\.yaml"
    )
    for desktop_job in (desktop_static, native, macos_slices, macos_universal):
        assert len(node_then_standalone_pnpm.findall(desktop_job)) == 1
        assert "cache: pnpm" not in desktop_job


def test_macos_signing_uses_persistent_runner_state_without_credential_secrets() -> None:
    root = Path(__file__).resolve().parents[3]
    workflow = (root / ".github/workflows/native.yml").read_text(encoding="utf-8")
    universal_job = workflow.split("  macos-universal:\n", 1)[1].split(
        "  draft-release:\n", 1
    )[0]

    for obsolete in (
        "P12_BASE64",
        "P12_PASSWORD",
        "MACOS_DEV_ID_CERT_NAME",
        "APPLE_NOTARY_USER",
        "APPLE_NOTARY_APP_PASSWORD",
        "APPLE_TEAM_ID",
        "apple-actions/import-codesign-certs",
    ):
        assert obsolete not in workflow
    assert "$HOME/Library/Keychains/login.keychain-db" in universal_job
    assert "$HOME/.config/developer-id-signing/login-keychain-password" in universal_job
    assert "python tools/python/macos_signing.py" in universal_job
    assert "exactly one Developer ID Application" in universal_job
    assert "security list-keychains -d user -s" in universal_job
    assert "codesign --force --timestamp --options runtime" in universal_job
    assert "macos-signing-verification" in universal_job
    assert "APPLE_SIGNING_IDENTITY: ${{ steps.signing.outputs.fingerprint }}" in universal_job
    assert "--keychain-profile developer-id-notary" in universal_job


def test_native_workflow_notarizes_and_verifies_the_uploaded_macos_dmg() -> None:
    root = Path(__file__).resolve().parents[3]
    workflow = (root / ".github/workflows/native.yml").read_text(encoding="utf-8")
    universal_job = workflow.split("  macos-universal:\n", 1)[1].split(
        "  draft-release:\n", 1
    )[0]

    stage = universal_job.index("- name: Stage universal macOS desktop release package")
    notarize = universal_job.index("- name: Notarize and staple universal macOS desktop DMG")
    verify = universal_job.index("- name: Verify staged universal macOS desktop DMG")
    upload = universal_job.index("- name: Upload universal macOS desktop DMG")
    assert stage < notarize < verify < upload
    assert "- name: Verify universal macOS desktop package" not in universal_job

    notarization_step = universal_job[notarize:verify]
    assert "build/artifacts/axkdeck-macos-universal" in notarization_step
    assert "src-tauri/target/universal-apple-darwin/release/bundle/dmg" not in notarization_step
    verification_step = universal_job[verify:upload]
    assert 'codesign --verify --strict --verbose=2 "$dmg"' in verification_step
    assert 'xcrun stapler validate "$dmg"' in verification_step
    assert "spctl --assess --type open" in verification_step
    assert "spctl --assess --type execute" in verification_step
    assert '"$app/Contents/MacOS/axkdeck"' in verification_step
    assert '"$app/Contents/MacOS/axklib-server"' in verification_step
    assert 'lipo "$main" -verify_arch x86_64 arm64' in verification_step
    assert 'lipo "$sidecar" -verify_arch x86_64 arm64' in verification_step
    assert '"$app/Contents/Resources/licenses/axkdeck.spdx.json"' in verification_step
    assert '"$app/Contents/Resources/licenses/LGPL-2.1-or-later.txt"' in verification_step


def test_native_workflow_builds_tests_and_packages_server_on_every_release_target() -> None:
    root = Path(__file__).resolve().parents[3]
    workflow = (root / ".github/workflows/native.yml").read_text(encoding="utf-8")
    macos_helper = (root / "tools/release/build_macos_slices.sh").read_text(
        encoding="utf-8"
    )

    for target in ("Linux x64", "Linux ARM64", "Windows x64", "Windows ARM64"):
        assert f"name: {target}" in workflow
    assert "msvc_component: Microsoft.VisualStudio.Component.VC.Tools.ARM64" in workflow
    assert workflow.count('build_testing: "ON"') == 4
    assert workflow.count("run_tests: true") == 4
    assert "build_slice x86_64 x64-osx-axk macos-x64" in macos_helper
    assert "build_slice arm64 arm64-osx-axk macos-arm64" in macos_helper
    assert '/usr/bin/arch -x86_64 "$ctest_path"' in macos_helper
    assert "-DAXK_BUILD_SERVER=ON" in workflow
    assert 'cmake --install "$root" --prefix "$scan/server" --component server' in workflow
    assert 'python tools/python/inspect_package.py "$scan/server"' in workflow
    assert "release_server_smoke.py" not in workflow
    assert "release_server_smoke.py" not in macos_helper
    assert "build/tmp/universal/bin/axklib-server" in workflow
    for installed_file in (
        "share/axklib/axklib-server.spdx.json",
        "share/axklib/server/openapi-v1.json",
        "share/doc/axklib/server.md",
    ):
        assert installed_file in workflow
def test_native_workflow_checks_contract_and_generates_source_aware_server_sbom() -> None:
    root = Path(__file__).resolve().parents[3]
    workflow = (root / ".github/workflows/native.yml").read_text(encoding="utf-8")

    assert "tools/python/openapi_compat.py" not in workflow
    assert "openapi-v1.compatibility.json" not in workflow
    assert "pnpm contract:check" in workflow
    assert '--package-basename-file "$root/package_basename.txt" --profile server' in workflow
    assert '--output "$root/axklib-server.spdx.json"' in workflow


def test_docs_workflow_renders_mermaid_and_publishes_one_pages_artifact() -> None:
    root = Path(__file__).resolve().parents[3]
    workflow = (root / ".github/workflows/publish-docs.yml").read_text(encoding="utf-8")

    assert "actions/setup-node@820762786026740c76f36085b0efc47a31fe5020 # v7" in workflow
    assert "runs-on: [self-hosted, Linux, X64, jammy, docker]" in workflow
    assert "actions/setup-python@5fda3b95a4ea91299a34e894583c3862153e4b97 # v7.0.0" in workflow
    assert 'python-version: "3.13.14"' in workflow
    assert 'node-version: "24"' in workflow
    assert "npm ci" in workflow
    assert 'PATH="$PWD/node_modules/.bin:$PATH"' in workflow
    assert "mkdocs build --strict --config-file mkdocs.yml" in workflow
    assert workflow.count("actions/upload-pages-artifact@fc324d3547104276b827a68afc52ff2a11cc49c9 # v5") == 1
    assert workflow.count("actions/deploy-pages@cd2ce8fcbc39b97be8ca5fce6e763baed58fa128 # v5") == 1


def test_privileged_workflows_pin_every_action_to_a_full_commit() -> None:
    root = Path(__file__).resolve().parents[3]
    for relative in (".github/workflows/native.yml", ".github/workflows/publish-docs.yml"):
        workflow = (root / relative).read_text(encoding="utf-8")
        references = re.findall(r"uses:\s+[^\s@]+@([^\s#]+)", workflow)
        assert references
        assert all(re.fullmatch(r"[0-9a-f]{40}", reference) for reference in references)


@pytest.mark.parametrize(
    "contents",
    ["axklib-main-a1b2c3d", "axklib-main-a1b2c3d\nextra\n", "unsafe/name\n"],
)
def test_release_metadata_rejects_malformed_package_file(tmp_path: Path, contents: str) -> None:
    package_file = tmp_path / "package_basename.txt"
    package_file.write_text(contents, encoding="utf-8")
    with pytest.raises(ValueError):
        release_metadata.read_package_basename(package_file)


def test_release_metadata_parses_and_verifies_cli_report() -> None:
    report = release_metadata.parse_version_report(
        "axklib v1.2.3-a1b2c3d\n"
        "version: 1.2.3\n"
        "package: axklib-v1.2.3-a1b2c3d\n"
        "git: a1b2c3d\n"
        "ref: v1.2.3\n"
        "source: clean\n"
    )
    release_metadata.verify_version_report(
        report,
        version=metadata("1.2.3", "1.2.3", "v1.2.3"),
        package_basename="axklib-v1.2.3-a1b2c3d",
        git_sha_short="a1b2c3d",
        selected_ref="v1.2.3",
    )
    with pytest.raises(ValueError, match="version report mismatch"):
        release_metadata.verify_version_report(
            report,
            version=metadata("1.2.3", "1.2.3", "v1.2.3"),
            package_basename="axklib-v1.2.3-a1b2c3d",
            git_sha_short="fffffff",
            selected_ref="v1.2.3",
        )


def test_release_metadata_verifies_cross_compiled_binary_strings(tmp_path: Path) -> None:
    binary = tmp_path / "axklib.exe"
    binary.write_bytes(b"header\x000.0.0\0main-a1b2c3d\0axklib-main-a1b2c3d\0a1b2c3d\0main\0footer")
    release_metadata.verify_binary_strings(
        binary,
        version=metadata(),
        package_basename="axklib-main-a1b2c3d",
        git_sha_short="a1b2c3d",
        selected_ref="main",
    )
    with pytest.raises(ValueError, match="does not contain"):
        release_metadata.verify_binary_strings(
            binary,
            version=metadata(),
            package_basename="axklib-main-a1b2c3d",
            git_sha_short="fffffff",
            selected_ref="main",
        )
