from __future__ import annotations

import json
import tarfile
import zipfile
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
        package = tmp_path / f"axkdeck-0.1.0-{platform}-{architecture}{extension}"
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
        package = directory / f"axkdeck-0.1.0-{platform}-{architecture}{extension}"
        package.write_bytes(b"installer")
        packages.append(package)
    return packages


def test_release_assets_reject_missing_installer(tmp_path: Path) -> None:
    packages = write_complete_release_asset_set(tmp_path)
    packages[-1].unlink()

    with pytest.raises(ValueError, match="expected one macos-universal desktop package"):
        release_metadata.verify_release_assets(tmp_path)


def test_release_assets_reject_share_content(tmp_path: Path) -> None:
    write_complete_release_asset_set(tmp_path)
    sdk = tmp_path / "axklib-sdk-main-a1b2c3d-windows-x64.zip"
    with zipfile.ZipFile(sdk, "a") as archive:
        archive.writestr("share/axklib/axklib.spdx.json", "{}")

    with pytest.raises(ValueError, match="contains share/"):
        release_metadata.verify_release_assets(tmp_path)


def test_native_workflow_creates_only_release_drafts() -> None:
    root = Path(__file__).resolve().parents[3]
    workflow = (root / ".github/workflows/native.yml").read_text(encoding="utf-8")
    assert 'VCPKG_DEFAULT_BINARY_CACHE=$RUNNER_TEMP/vcpkg/archives' in workflow
    assert workflow.count("path: ${{ runner.temp }}/vcpkg/archives") == 2
    assert "VCPKG_DEFAULT_BINARY_CACHE: ${{ runner.temp }}" not in workflow
    assert "VCPKG_DEFAULT_BINARY_CACHE: ${{ github.workspace }}/.." not in workflow
    assert "draft-release:" in workflow
    assert "if: ${{ !inputs.debug }}" in workflow
    assert "uses: actions/download-artifact@v8" in workflow
    assert "gh release create" in workflow
    assert "--draft" in workflow
    assert "version_metadata.json" in workflow
    assert '"libaxklib.so.${project_version}"' in workflow
    assert '"libaxklib.${project_version}.dylib"' in workflow


def test_native_workflow_uses_official_dependency_and_incremental_build_caches() -> None:
    root = Path(__file__).resolve().parents[3]
    workflow = (root / ".github/workflows/native.yml").read_text(encoding="utf-8")

    assert workflow.count("uses: actions/cache@v6") == 4
    assert "sccache" not in workflow.lower()
    assert "cold_build" not in workflow
    assert "git rev-parse HEAD:external/vcpkg" in workflow
    assert "CMakePresets.json" in workflow
    assert "library/cmake/triplets/**" in workflow
    assert "library/cmake/ports/**" in workflow
    assert "key: vcpkg-v2-${{ matrix.triplet }}-" in workflow
    assert "vcpkg-v2-${{ matrix.triplet }}-${{ steps.cache-inputs.outputs.vcpkg_revision }}-" in workflow
    assert "key: native-v1-${{ matrix.triplet }}-" in workflow
    assert "key: axkdeck-rust-v1-${{ matrix.artifact }}-" in workflow
    assert "key: axkdeck-rust-v1-macos-universal-" in workflow
    assert "${{ steps.native-toolchain.outputs.toolchain_fingerprint }}" in workflow
    assert "${{ steps.cache-inputs.outputs.dependency_fingerprint }}-${{ github.sha }}" in workflow
    assert "!build/native/${{ inputs.debug && 'debug' || 'release' }}/vcpkg_installed/**" not in workflow
    assert "!build/native/${{ inputs.debug && 'debug' || 'release' }}/Testing/**" in workflow
    assert "tools/python/native_build_cache.py fingerprint" in workflow
    assert "tools/python/native_build_cache.py prepare" in workflow
    assert workflow.count("uses: actions/cache/save@v6") == 3
    assert "Save vcpkg binary cache after failure" in workflow
    assert "Save native incremental build cache after failure" in workflow
    assert workflow.count("steps.configure-native.outcome == 'success'") == 2
    assert workflow.count("continue-on-error: true") == 2


def test_native_workflow_builds_monorepo_desktop_packages_from_tested_servers() -> None:
    root = Path(__file__).resolve().parents[3]
    workflow = (root / ".github/workflows/native.yml").read_text(encoding="utf-8")

    assert "desktop-static:" in workflow
    assert "needs:\n      - release-tools\n      - desktop-static" in workflow
    assert workflow.count("uses: astral-sh/setup-uv@v8.3.2") == 3
    assert (
        workflow.count("uv --project tools/python run python tools/python/generate_sbom.py") == 5
    )
    assert "AXKLIB_SERVER_BINARY=$server" in workflow
    assert "pnpm tauri build --no-bundle" in workflow
    assert workflow.count("pnpm tauri build") >= 4
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
    assert "pnpm tauri build --target universal-apple-darwin" in workflow
    assert "lipo \"$sidecar\" -verify_arch x86_64 arm64" in workflow
    assert "if-no-files-found: error" in workflow


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


def test_native_workflow_restores_macos_slices_across_rerun_attempts() -> None:
    root = Path(__file__).resolve().parents[3]
    workflow = (root / ".github/workflows/native.yml").read_text(encoding="utf-8")

    universal_job = workflow.split("  macos-universal:\n", 1)[1].split(
        "  draft-release:\n", 1
    )[0]
    assert "submodules: recursive" in universal_job
    assert "key: macos-x86_64-${{ github.run_id }}-${{ github.run_attempt }}" in workflow
    assert "restore-keys: |\n            macos-x86_64-${{ github.run_id }}-" in workflow
    assert "key: macos-arm64-${{ github.run_id }}-${{ github.run_attempt }}" in workflow
    assert "restore-keys: |\n            macos-arm64-${{ github.run_id }}-" in workflow


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
    assert 'cmake --install "$root" --prefix "$scan/server" --component server' in workflow
    assert 'python tools/python/inspect_package.py "$scan/server"' in workflow
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
    assert '--package-basename-file "$root/package_basename.txt" --profile server' in workflow
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
