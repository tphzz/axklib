from __future__ import annotations

import argparse
import json
import re
import subprocess
from dataclasses import asdict, dataclass
from pathlib import Path

import generate_sbom

PLATFORMS = (
    "linux-x64",
    "linux-arm64",
    "windows-x64",
    "windows-arm64",
    "macos-x64",
    "macos-arm64",
    "macos-universal",
)


@dataclass(frozen=True)
class ArtifactMetadata:
    package_basename: str
    artifact_stem: str


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
    semantic_version: str,
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
        expected_tag = f"v{semantic_version}"
        if ref_name != expected_tag:
            raise ValueError(f"release tag must be {expected_tag}, found {ref_name or '<empty>'}")
        artifact_base = f"axklib-{semantic_version}"
    elif ref_type == "branch":
        artifact_base = package_basename
    else:
        raise ValueError(f"unsupported GitHub ref type: {ref_type or '<empty>'}")
    debug_suffix = "-debug" if normalized_configuration == "debug" else ""
    return ArtifactMetadata(package_basename, f"{artifact_base}-{platform}{debug_suffix}")


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
    semantic_version: str,
    package_basename: str,
    git_sha_short: str,
    selected_ref: str,
) -> None:
    expected = VersionReport(
        source_identity=package_basename.removeprefix("axklib-"),
        semantic_version=semantic_version,
        package_basename=package_basename,
        git_sha_short=git_sha_short,
        selected_ref=sanitize_ref(selected_ref),
        source_state="clean",
    )
    if report != expected:
        raise ValueError(f"version report mismatch: expected {expected}, found {report}")


def verify_binary_strings(
    binary: Path, *, package_basename: str, git_sha_short: str, selected_ref: str
) -> None:
    source_identity = package_basename.removeprefix("axklib-")
    required = (source_identity, package_basename, git_sha_short, sanitize_ref(selected_ref))
    contents = binary.read_bytes()
    missing = [value for value in required if value.encode("ascii") not in contents]
    if missing:
        raise ValueError(f"binary does not contain expected build metadata: {missing}")


def write_github_outputs(path: Path, metadata: ArtifactMetadata) -> None:
    with path.open("a", encoding="utf-8", newline="\n") as output:
        output.write(f"package_basename={metadata.package_basename}\n")
        output.write(f"artifact_stem={metadata.artifact_stem}\n")


def resolve_command(args: argparse.Namespace) -> int:
    metadata = artifact_metadata(
        read_package_basename(args.package_basename_file),
        generate_sbom.project_version(args.axklib_root.resolve()),
        args.ref_type,
        args.ref_name,
        args.platform,
        args.configuration,
    )
    if args.github_output:
        write_github_outputs(args.github_output, metadata)
    print(json.dumps(asdict(metadata), sort_keys=True))
    return 0


def verify_command(args: argparse.Namespace) -> int:
    root = args.axklib_root.resolve()
    package_basename = read_package_basename(args.package_basename_file)
    completed = subprocess.run(
        [str(args.cli), "--version"], check=True, capture_output=True, text=True
    )
    verify_version_report(
        parse_version_report(completed.stdout),
        semantic_version=generate_sbom.project_version(root),
        package_basename=package_basename,
        git_sha_short=args.git_sha_short,
        selected_ref=args.ref_name,
    )
    return 0


def verify_binary_command(args: argparse.Namespace) -> int:
    verify_binary_strings(
        args.binary,
        package_basename=read_package_basename(args.package_basename_file),
        git_sha_short=args.git_sha_short,
        selected_ref=args.ref_name,
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    resolve = subparsers.add_parser("resolve")
    resolve.add_argument("--axklib-root", type=Path, default=Path.cwd())
    resolve.add_argument("--package-basename-file", required=True, type=Path)
    resolve.add_argument("--ref-type", required=True)
    resolve.add_argument("--ref-name", required=True)
    resolve.add_argument("--platform", required=True, choices=PLATFORMS)
    resolve.add_argument("--configuration", required=True, choices=("Debug", "Release"))
    resolve.add_argument("--github-output", type=Path)
    resolve.set_defaults(handler=resolve_command)

    verify = subparsers.add_parser("verify-cli")
    verify.add_argument("--axklib-root", type=Path, default=Path.cwd())
    verify.add_argument("--package-basename-file", required=True, type=Path)
    verify.add_argument("--cli", required=True, type=Path)
    verify.add_argument("--git-sha-short", required=True)
    verify.add_argument("--ref-name", required=True)
    verify.set_defaults(handler=verify_command)

    verify_binary = subparsers.add_parser("verify-binary")
    verify_binary.add_argument("--package-basename-file", required=True, type=Path)
    verify_binary.add_argument("--binary", required=True, type=Path)
    verify_binary.add_argument("--git-sha-short", required=True)
    verify_binary.add_argument("--ref-name", required=True)
    verify_binary.set_defaults(handler=verify_binary_command)

    args = parser.parse_args()
    return int(args.handler(args))


if __name__ == "__main__":
    raise SystemExit(main())
