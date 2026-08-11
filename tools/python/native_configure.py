"""Run native CMake configuration with narrow vcpkg state recovery."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

STALE_EXTRACTION_MARKER = "was an extraction target, but it already exists."
FAILED_PACKAGE_PATTERN = re.compile(r"^error: building (.+) failed with:", re.MULTILINE)


@dataclass(frozen=True)
class ConfigureReport:
    returncode: int
    attempts: int
    recovery_attempted: bool


@dataclass(frozen=True)
class ManifestFailure:
    package: str | None
    primary_error: str


def inspect_manifest(contents: str) -> ManifestFailure:
    package_match = FAILED_PACKAGE_PATTERN.search(contents)
    package = package_match.group(1).strip() if package_match else None
    lines = contents.splitlines()
    for index, line in enumerate(lines):
        if STALE_EXTRACTION_MARKER not in line:
            continue
        previous = ""
        for candidate in reversed(lines[:index]):
            candidate = candidate.strip()
            if candidate and not candidate.startswith("CMake Error at "):
                previous = candidate
                break
        primary = line.strip()
        if previous:
            primary = f"{previous} {primary}"
        return ManifestFailure(package, primary)

    for line in lines:
        if line.startswith("CMake Error at "):
            return ManifestFailure(package, line.strip())
    if package is not None:
        return ManifestFailure(package, f"vcpkg failed to build {package}")
    return ManifestFailure(None, "CMake configuration failed without a vcpkg diagnostic")


def _validated_downloads_root(runner_temp: Path, downloads_root: Path) -> Path:
    runner_lexical = Path(os.path.abspath(runner_temp))
    downloads_lexical = Path(os.path.abspath(downloads_root))
    if not runner_lexical.is_dir():
        raise ValueError("runner temporary directory does not exist")
    if downloads_lexical == runner_lexical or not downloads_lexical.is_relative_to(
        runner_lexical
    ):
        raise ValueError(
            "vcpkg downloads root must be a strict descendant of the runner temporary directory"
        )

    current = runner_lexical
    for component in downloads_lexical.relative_to(runner_lexical).parts:
        current /= component
        if current.is_symlink():
            raise ValueError("vcpkg downloads root must not contain a symbolic link")
        if current.exists() and not current.is_dir():
            raise ValueError("vcpkg downloads root contains a non-directory component")
    if not downloads_lexical.is_dir():
        raise ValueError("vcpkg downloads root does not exist")

    runner_resolved = runner_lexical.resolve(strict=True)
    downloads_resolved = downloads_lexical.resolve(strict=True)
    if not downloads_resolved.is_relative_to(runner_resolved):
        raise ValueError("vcpkg downloads root resolves outside the runner temporary directory")
    return downloads_lexical


def _remove_auxiliary_tools(downloads_root: Path) -> None:
    tools = downloads_root / "tools"
    if tools.is_symlink() or (tools.exists() and not tools.is_dir()):
        tools.unlink()
    elif tools.is_dir():
        shutil.rmtree(tools)


def _read_manifest(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def _snapshot_manifest(path: Path, diagnostics_directory: Path, attempt: int) -> Path | None:
    if not path.is_file():
        return None
    diagnostics_directory.mkdir(parents=True, exist_ok=True)
    destination = diagnostics_directory / f"vcpkg-manifest-attempt-{attempt}.log"
    shutil.copyfile(path, destination)
    return destination


def _clear_manifest(path: Path) -> None:
    if path.is_symlink() or path.is_file():
        path.unlink()


def _annotation_value(value: str) -> str:
    return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def _emit_annotation(level: str, title: str, message: str) -> None:
    print(f"::{level} title={_annotation_value(title)}::{_annotation_value(message)}")


def _append_summary(
    path: Path | None,
    *,
    heading: str,
    triplet: str,
    failure: ManifestFailure,
    manifest_snapshot: Path | None,
) -> None:
    if path is None:
        return
    package = failure.package or "not identified"
    snapshot = manifest_snapshot.name if manifest_snapshot is not None else "not produced"
    with path.open("a", encoding="utf-8") as stream:
        stream.write(f"### {heading}\n\n")
        stream.write(f"- Triplet: `{triplet}`\n")
        stream.write(f"- Package: `{package}`\n")
        stream.write(f"- Primary error: {failure.primary_error}\n")
        stream.write(f"- Preserved manifest: `{snapshot}`\n\n")


def run_native_configure(
    command: Sequence[str],
    *,
    runner_temp: Path,
    downloads_root: Path,
    manifest_path: Path,
    diagnostics_directory: Path,
    triplet: str,
    github_step_summary: Path | None,
) -> ConfigureReport:
    if not command:
        raise ValueError("native configure command must not be empty")
    downloads_root = _validated_downloads_root(runner_temp, downloads_root)
    configure_command = list(command)

    _clear_manifest(manifest_path)
    first = subprocess.run(configure_command, check=False)
    if first.returncode == 0:
        return ConfigureReport(0, 1, False)

    first_contents = _read_manifest(manifest_path)
    first_snapshot = _snapshot_manifest(manifest_path, diagnostics_directory, 1)
    first_failure = inspect_manifest(first_contents)
    if STALE_EXTRACTION_MARKER not in first_contents:
        _emit_annotation("error", "Native dependency configuration failed", first_failure.primary_error)
        _append_summary(
            github_step_summary,
            heading="Native dependency configuration failed",
            triplet=triplet,
            failure=first_failure,
            manifest_snapshot=first_snapshot,
        )
        return ConfigureReport(first.returncode, 1, False)

    _emit_annotation(
        "warning",
        "Recovered stale vcpkg tool extraction state",
        "Removed the isolated vcpkg downloads/tools directory and will retry configuration once.",
    )
    _remove_auxiliary_tools(downloads_root)
    _clear_manifest(manifest_path)
    second = subprocess.run(configure_command, check=False)
    if second.returncode == 0:
        _append_summary(
            github_step_summary,
            heading="Recovered stale vcpkg tool extraction state",
            triplet=triplet,
            failure=first_failure,
            manifest_snapshot=first_snapshot,
        )
        return ConfigureReport(0, 2, True)

    second_contents = _read_manifest(manifest_path)
    second_snapshot = _snapshot_manifest(manifest_path, diagnostics_directory, 2)
    second_failure = inspect_manifest(second_contents)
    _emit_annotation("error", "Native dependency configuration failed", second_failure.primary_error)
    _append_summary(
        github_step_summary,
        heading="Native dependency configuration failed after one recovery attempt",
        triplet=triplet,
        failure=second_failure,
        manifest_snapshot=second_snapshot,
    )
    return ConfigureReport(second.returncode, 2, True)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner-temp", type=Path, required=True)
    parser.add_argument("--downloads-root", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--diagnostics-directory", type=Path, required=True)
    parser.add_argument("--triplet", required=True)
    parser.add_argument("--github-step-summary", type=Path)
    parser.add_argument("configure_command", nargs=argparse.REMAINDER)
    return parser


def main() -> int:
    arguments = _parser().parse_args()
    command = arguments.configure_command
    if command[:1] == ["--"]:
        command = command[1:]
    report = run_native_configure(
        command,
        runner_temp=arguments.runner_temp,
        downloads_root=arguments.downloads_root,
        manifest_path=arguments.manifest,
        diagnostics_directory=arguments.diagnostics_directory,
        triplet=arguments.triplet,
        github_step_summary=arguments.github_step_summary,
    )
    return report.returncode


if __name__ == "__main__":
    raise SystemExit(main())
