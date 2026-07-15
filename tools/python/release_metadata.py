from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from dataclasses import asdict, dataclass
from pathlib import Path

import version_metadata

PLATFORMS = (
    "linux-x64",
    "linux-arm64",
    "windows-x64",
    "windows-arm64",
    "macos-x64",
    "macos-arm64",
    "macos-universal",
)

RELEASE_ASSET_EXTENSIONS = {
    "linux-x64": ".tar.gz",
    "linux-arm64": ".tar.gz",
    "windows-x64": ".zip",
    "windows-arm64": ".zip",
    "macos-universal": ".zip",
}


@dataclass(frozen=True)
class ArtifactMetadata:
    package_basename: str
    artifact_stem: str
    semantic_version: str
    project_version: str
    soversion: str
    is_prerelease: bool


@dataclass(frozen=True)
class DraftReleaseTarget:
    tag_name: str
    title: str
    cleanup_tag: bool
    verify_tag: bool
    prerelease: bool


@dataclass(frozen=True)
class VersionReport:
    source_identity: str
    semantic_version: str
    package_basename: str
    git_sha_short: str
    selected_ref: str
    source_state: str


def sanitize_ref(value: str) -> str:
    sanitized = re.sub(r"[^A-Za-z0-9._-]+", "-", value)
    sanitized = re.sub(r"-+", "-", sanitized).strip("-")
    return sanitized or "unknown"


def read_package_basename(path: Path) -> str:
    text = path.read_text(encoding="utf-8")
    if not text.endswith("\n") or text.count("\n") != 1:
        raise ValueError("package basename file must contain one newline-terminated line")
    basename = text[:-1]
    if not re.fullmatch(r"axklib-[A-Za-z0-9._-]+", basename):
        raise ValueError(f"invalid axklib package basename: {basename!r}")
    return basename


def artifact_metadata(
    package_basename: str,
    version: version_metadata.VersionMetadata,
    ref_type: str,
    ref_name: str,
    platform: str,
    configuration: str,
) -> ArtifactMetadata:
    if platform not in PLATFORMS:
        raise ValueError(f"unsupported artifact platform: {platform}")
    normalized_configuration = configuration.lower()
    if normalized_configuration not in {"debug", "release"}:
        raise ValueError(f"unsupported build configuration: {configuration}")
    if ref_type == "tag":
        expected_tag = version.release_tag
        if not version.is_release:
            raise ValueError("tag builds require release version metadata")
        if ref_name != expected_tag:
            raise ValueError(f"release tag must be {expected_tag}, found {ref_name or '<empty>'}")
        artifact_base = f"axklib-{version.semantic_version}"
    elif ref_type == "branch":
        artifact_base = package_basename
    else:
        raise ValueError(f"unsupported GitHub ref type: {ref_type or '<empty>'}")
    debug_suffix = "-debug" if normalized_configuration == "debug" else ""
    return ArtifactMetadata(
        package_basename,
        f"{artifact_base}-{platform}{debug_suffix}",
        version.semantic_version,
        version.project_version,
        str(version.major),
        version.is_prerelease,
    )


def draft_release_target(
    ref_type: str, ref_name: str, version: version_metadata.VersionMetadata
) -> DraftReleaseTarget:
    if not ref_name or "\n" in ref_name or "\r" in ref_name:
        raise ValueError("GitHub ref name must be a non-empty single line")
    if ref_type == "branch":
        tag_name = f"{ref_name}-preview"
        return DraftReleaseTarget(tag_name, tag_name, True, False, True)
    if ref_type == "tag":
        if not version.is_release or ref_name != version.release_tag:
            raise ValueError(
                f"release tag must be {version.release_tag or '<none>'}, found {ref_name}"
            )
        return DraftReleaseTarget(ref_name, ref_name, False, True, version.is_prerelease)
    raise ValueError(f"unsupported GitHub ref type: {ref_type or '<empty>'}")


def verify_release_assets(directory: Path) -> list[Path]:
    if not directory.is_dir():
        raise ValueError(f"release asset directory does not exist: {directory}")

    files = sorted(path for path in directory.iterdir() if path.is_file())
    expected: set[Path] = set()
    for platform, extension in RELEASE_ASSET_EXTENSIONS.items():
        archives = [path for path in files if path.name.endswith(f"-{platform}{extension}")]
        checksums = [path for path in files if path.name.endswith(f"-{platform}-SHA256SUMS")]
        if len(archives) != 1:
            raise ValueError(f"expected one {platform} release archive, found {len(archives)}")
        if len(checksums) != 1:
            raise ValueError(f"expected one {platform} checksum file, found {len(checksums)}")

        archive = archives[0]
        checksum = checksums[0]
        with archive.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        expected_checksum = f"{digest}  {archive.name}\n"
        if checksum.read_text(encoding="utf-8") != expected_checksum:
            raise ValueError(f"checksum does not match release archive: {archive.name}")
        expected.update((archive, checksum))

    unexpected = sorted(path.name for path in files if path not in expected)
    if unexpected:
        raise ValueError(f"unexpected release assets: {unexpected}")
    return sorted(expected)


def parse_version_report(text: str) -> VersionReport:
    lines = text.rstrip("\r\n").splitlines()
    if len(lines) != 6 or not lines[0].startswith("axklib "):
        raise ValueError("axklib --version did not emit the six-line build-information report")
    labels = ("version: ", "package: ", "git: ", "ref: ", "source: ")
    values: list[str] = []
    for line, label in zip(lines[1:], labels, strict=True):
        if not line.startswith(label):
            raise ValueError(f"axklib --version line must start with {label!r}")
        values.append(line[len(label) :])
    return VersionReport(lines[0][len("axklib ") :], *values)


def verify_version_report(
    report: VersionReport,
    *,
    version: version_metadata.VersionMetadata,
    package_basename: str,
    git_sha_short: str,
    selected_ref: str,
) -> None:
    expected = VersionReport(
        source_identity=package_basename.removeprefix("axklib-"),
        semantic_version=version.semantic_version,
        package_basename=package_basename,
        git_sha_short=git_sha_short,
        selected_ref=sanitize_ref(selected_ref),
        source_state="clean",
    )
    if report != expected:
        raise ValueError(f"version report mismatch: expected {expected}, found {report}")


def verify_binary_strings(
    binary: Path,
    *,
    version: version_metadata.VersionMetadata,
    package_basename: str,
    git_sha_short: str,
    selected_ref: str,
) -> None:
    source_identity = package_basename.removeprefix("axklib-")
    required = (
        version.semantic_version,
        source_identity,
        package_basename,
        git_sha_short,
        sanitize_ref(selected_ref),
    )
    contents = binary.read_bytes()
    missing = [value for value in required if value.encode("ascii") not in contents]
    if missing:
        raise ValueError(f"binary does not contain expected build metadata: {missing}")


def write_github_outputs(path: Path, metadata: ArtifactMetadata) -> None:
    with path.open("a", encoding="utf-8", newline="\n") as output:
        output.write(f"package_basename={metadata.package_basename}\n")
        output.write(f"artifact_stem={metadata.artifact_stem}\n")
        output.write(f"semantic_version={metadata.semantic_version}\n")
        output.write(f"project_version={metadata.project_version}\n")
        output.write(f"soversion={metadata.soversion}\n")
        output.write(f"is_prerelease={str(metadata.is_prerelease).lower()}\n")


def write_release_github_outputs(path: Path, target: DraftReleaseTarget) -> None:
    with path.open("a", encoding="utf-8", newline="\n") as output:
        output.write(f"tag_name={target.tag_name}\n")
        output.write(f"title={target.title}\n")
        output.write(f"cleanup_tag={str(target.cleanup_tag).lower()}\n")
        output.write(f"verify_tag={str(target.verify_tag).lower()}\n")
        output.write(f"prerelease={str(target.prerelease).lower()}\n")


def resolve_command(args: argparse.Namespace) -> int:
    version = version_metadata.read(args.version_metadata_file)
    metadata = artifact_metadata(
        read_package_basename(args.package_basename_file),
        version,
        args.ref_type,
        args.ref_name,
        args.platform,
        args.configuration,
    )
    if args.github_output:
        write_github_outputs(args.github_output, metadata)
    print(json.dumps(asdict(metadata), sort_keys=True))
    return 0


def release_target_command(args: argparse.Namespace) -> int:
    target = draft_release_target(
        args.ref_type, args.ref_name, version_metadata.read(args.version_metadata_file)
    )
    if args.github_output:
        write_release_github_outputs(args.github_output, target)
    print(json.dumps(asdict(target), sort_keys=True))
    return 0


def verify_release_assets_command(args: argparse.Namespace) -> int:
    assets = verify_release_assets(args.directory)
    print(json.dumps([path.name for path in assets]))
    return 0


def verify_command(args: argparse.Namespace) -> int:
    version = version_metadata.read(args.version_metadata_file)
    package_basename = read_package_basename(args.package_basename_file)
    completed = subprocess.run(
        [str(args.cli), "--version"], check=True, capture_output=True, text=True
    )
    verify_version_report(
        parse_version_report(completed.stdout),
        version=version,
        package_basename=package_basename,
        git_sha_short=args.git_sha_short,
        selected_ref=args.ref_name,
    )
    return 0


def verify_binary_command(args: argparse.Namespace) -> int:
    verify_binary_strings(
        args.binary,
        version=version_metadata.read(args.version_metadata_file),
        package_basename=read_package_basename(args.package_basename_file),
        git_sha_short=args.git_sha_short,
        selected_ref=args.ref_name,
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    resolve = subparsers.add_parser("resolve")
    resolve.add_argument("--version-metadata-file", required=True, type=Path)
    resolve.add_argument("--package-basename-file", required=True, type=Path)
    resolve.add_argument("--ref-type", required=True)
    resolve.add_argument("--ref-name", required=True)
    resolve.add_argument("--platform", required=True, choices=PLATFORMS)
    resolve.add_argument("--configuration", required=True, choices=("Debug", "Release"))
    resolve.add_argument("--github-output", type=Path)
    resolve.set_defaults(handler=resolve_command)

    release_target = subparsers.add_parser("release-target")
    release_target.add_argument("--version-metadata-file", required=True, type=Path)
    release_target.add_argument("--ref-type", required=True)
    release_target.add_argument("--ref-name", required=True)
    release_target.add_argument("--github-output", type=Path)
    release_target.set_defaults(handler=release_target_command)

    verify_release_assets_parser = subparsers.add_parser("verify-release-assets")
    verify_release_assets_parser.add_argument("--directory", required=True, type=Path)
    verify_release_assets_parser.set_defaults(handler=verify_release_assets_command)

    verify = subparsers.add_parser("verify-cli")
    verify.add_argument("--version-metadata-file", required=True, type=Path)
    verify.add_argument("--package-basename-file", required=True, type=Path)
    verify.add_argument("--cli", required=True, type=Path)
    verify.add_argument("--git-sha-short", required=True)
    verify.add_argument("--ref-name", required=True)
    verify.set_defaults(handler=verify_command)

    verify_binary = subparsers.add_parser("verify-binary")
    verify_binary.add_argument("--version-metadata-file", required=True, type=Path)
    verify_binary.add_argument("--package-basename-file", required=True, type=Path)
    verify_binary.add_argument("--binary", required=True, type=Path)
    verify_binary.add_argument("--git-sha-short", required=True)
    verify_binary.add_argument("--ref-name", required=True)
    verify_binary.set_defaults(handler=verify_binary_command)

    args = parser.parse_args()
    return int(args.handler(args))


if __name__ == "__main__":
    raise SystemExit(main())
