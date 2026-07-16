from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

import generate_sbom
import inspect_package
import release_metadata
import version_metadata


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
    assert document["documentNamespace"].startswith(
        "https://github.com/tphzz/axklib/spdx/"
    )
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
    crow = next(item for item in json.loads(output.read_text())["packages"] if item["name"] == "crow")
    asio = next(item for item in json.loads(output.read_text())["packages"] if item["name"] == "asio")
    assert crow["versionInfo"] == "1.3.3"
    assert crow["licenseDeclared"] == "BSD-3-Clause"
    assert asio["versionInfo"] == "1.32.0"
    assert asio["licenseDeclared"] == "BSL-1.0"
    assert "source SHA512: c270425953d84c5f" in crow["comment"]
    assert "source SHA512: 9374ff97bd4af7b5" in asio["comment"]


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
        "sdk", "main-a1b2c3d",
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
    assert len(
        {
            first["documentNamespace"],
            changed_version["documentNamespace"],
            changed_profile["documentNamespace"],
            changed_source["documentNamespace"],
            changed_timestamp["documentNamespace"],
        }
    ) == 5


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
    assert debug.artifact_stem == "axklib-feature-audio-a1b2c3d-windows-arm64-debug"


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


def test_release_assets_require_every_distribution_and_exact_checksums(tmp_path: Path) -> None:
    assets: list[Path] = []
    for platform, extension in release_metadata.RELEASE_ASSET_EXTENSIONS.items():
        archive = tmp_path / f"axklib-main-a1b2c3d-{platform}{extension}"
        archive.write_bytes(platform.encode())
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        checksum = tmp_path / f"axklib-main-a1b2c3d-{platform}-SHA256SUMS"
        checksum.write_text(f"{digest}  {archive.name}\n", encoding="utf-8")
        assets.extend((archive, checksum))

    assert release_metadata.verify_release_assets(tmp_path) == sorted(assets)
    assets[1].write_text("0" * 64 + f"  {assets[0].name}\n", encoding="utf-8")
    with pytest.raises(ValueError, match="checksum does not match"):
        release_metadata.verify_release_assets(tmp_path)
    assets[1].write_text(
        f"{hashlib.sha256(assets[0].read_bytes()).hexdigest()}  {assets[0].name}\n",
        encoding="utf-8",
    )
    unexpected = tmp_path / "unexpected.txt"
    unexpected.write_text("unexpected", encoding="utf-8")
    with pytest.raises(ValueError, match="unexpected release assets"):
        release_metadata.verify_release_assets(tmp_path)


def test_native_workflow_creates_only_release_drafts() -> None:
    root = Path(__file__).resolve().parents[3]
    workflow = (root / ".github/workflows/native.yml").read_text(encoding="utf-8")
    assert "VCPKG_DEFAULT_BINARY_CACHE: ${{ github.workspace }}/../.cache/vcpkg/archives" in workflow
    assert "VCPKG_DEFAULT_BINARY_CACHE: ${{ github.workspace }}/.cache" not in workflow
    assert "draft-release:" in workflow
    assert "if: ${{ !inputs.debug }}" in workflow
    assert "uses: actions/download-artifact@v8" in workflow
    assert "gh release create" in workflow
    assert "--draft" in workflow
    assert "version_metadata.json" in workflow
    assert '"libaxklib.so.${project_version}"' in workflow
    assert '"libaxklib.${project_version}.dylib"' in workflow


def test_native_workflow_builds_tests_and_packages_server_on_every_release_target() -> None:
    root = Path(__file__).resolve().parents[3]
    workflow = (root / ".github/workflows/native.yml").read_text(encoding="utf-8")

    for target in (
        "Linux x64",
        "Linux ARM64",
        "Windows x64",
        "Windows ARM64",
        "macOS x64 slice",
        "macOS ARM64 slice",
    ):
        assert f"name: {target}" in workflow
    assert "runner: windows-11-vs2026-arm" in workflow
    assert "msvc_component: Microsoft.VisualStudio.Component.VC.Tools.ARM64" in workflow
    assert workflow.count('build_testing: "ON"') == 6
    assert workflow.count("run_tests: true") == 6
    assert "-DAXK_BUILD_SERVER=ON" in workflow
    assert "cmake --install \"$root\" --prefix \"$scan/server\" --component server" in workflow
    assert "python tools/python/inspect_package.py \"$scan/server\"" in workflow
    assert "python tools/python/release_server_smoke.py" in workflow
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

    assert "tools/python/openapi_compat.py check" in workflow
    assert "apps/server/contracts/openapi-v1.compatibility.json" in workflow
    assert "apps/server/contracts/openapi-v1.json" in workflow
    assert "--package-basename-file \"$root/package_basename.txt\" --profile server" in workflow
    assert '--output "$root/axklib-server.spdx.json"' in workflow


def test_docs_workflow_renders_mermaid_and_publishes_one_pages_artifact() -> None:
    root = Path(__file__).resolve().parents[3]
    workflow = (root / ".github/workflows/publish-docs.yml").read_text(encoding="utf-8")

    assert "uses: actions/setup-node@v7" in workflow
    assert 'node-version: "24"' in workflow
    assert "npm ci" in workflow
    assert 'PATH="$PWD/node_modules/.bin:$PATH"' in workflow
    assert "mkdocs build --strict --config-file mkdocs.yml" in workflow
    assert workflow.count("actions/upload-pages-artifact@v5") == 1
    assert workflow.count("actions/deploy-pages@v5") == 1


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
    binary.write_bytes(
        b"header\0" b"0.0.0\0main-a1b2c3d\0axklib-main-a1b2c3d\0a1b2c3d\0main\0footer"
    )
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
