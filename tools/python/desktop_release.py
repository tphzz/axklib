from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path
from typing import Any

import release_metadata
import version_metadata

SOURCE_IDENTITY_PREFIX = "axklib source identity: "
DESKTOP_PACKAGE_EXTENSIONS = {
    ("linux", "x64"): {".deb", ".rpm"},
    ("linux", "arm64"): {".deb", ".rpm"},
    ("windows", "x64"): {".exe"},
    ("windows", "arm64"): {".exe"},
    ("macos", "universal"): {".dmg"},
}


def stage_release_packages(
    *,
    bundle_directory: Path,
    extensions: set[str],
    output_directory: Path,
    platform: str,
    architecture: str,
    axklib_version_metadata: Path,
    package_basename_file: Path,
    sbom: Path,
    configuration: str,
) -> list[Path]:
    expected_extensions = DESKTOP_PACKAGE_EXTENSIONS.get((platform, architecture))
    if expected_extensions is None:
        raise ValueError(f"unsupported desktop release target: {platform}-{architecture}")

    if extensions != expected_extensions:
        raise ValueError(
            f"desktop release target {platform}-{architecture} requires extensions "
            f"{sorted(expected_extensions)}, found {sorted(extensions)}"
        )

    packages: list[Path] = []
    for extension in sorted(extensions):
        matching = sorted(
            path
            for path in bundle_directory.rglob("*")
            if path.is_file() and path.suffix.lower() == extension
        )
        if len(matching) != 1:
            raise ValueError(
                f"expected one {extension} desktop package in {bundle_directory}, "
                f"found {len(matching)}"
            )
        packages.append(matching[0])

    version = version_metadata.read(axklib_version_metadata)
    native_package_basename = release_metadata.read_package_basename(package_basename_file)
    source_identity = native_package_basename.removeprefix("axklib-")
    sbom_data: dict[str, Any] = json.loads(sbom.read_text(encoding="utf-8"))
    sbom_comment = sbom_data.get("comment")
    if not isinstance(sbom_comment, str) or not sbom_comment.startswith(SOURCE_IDENTITY_PREFIX):
        raise ValueError("desktop SBOM does not declare the axklib source identity")
    if sbom_comment.removeprefix(SOURCE_IDENTITY_PREFIX) != source_identity:
        raise ValueError("desktop SBOM source identity does not match the native build")
    normalized_configuration = configuration.lower()
    if normalized_configuration not in {"debug", "release"}:
        raise ValueError(f"unsupported desktop build configuration: {configuration}")
    identity = version.semantic_version if version.is_release else source_identity
    debug_suffix = "-debug" if normalized_configuration == "debug" else ""
    stem = f"axkdeck-{identity}-{platform}-{architecture}{debug_suffix}"
    output_directory.mkdir(parents=True, exist_ok=True)
    staged: list[Path] = []
    for package in packages:
        output = output_directory / f"{stem}{package.suffix.lower()}"
        shutil.copyfile(package, output)
        staged.append(output)
    return staged


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle-directory", type=Path, required=True)
    parser.add_argument("--extension", action="append", required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--architecture", required=True)
    parser.add_argument("--axklib-version-metadata", type=Path, required=True)
    parser.add_argument("--package-basename-file", type=Path, required=True)
    parser.add_argument("--sbom", type=Path, required=True)
    parser.add_argument("--configuration", choices=("Debug", "Release"), required=True)
    arguments = parser.parse_args()
    stage_release_packages(
        bundle_directory=arguments.bundle_directory,
        extensions={value.lower() for value in arguments.extension},
        output_directory=arguments.output_directory,
        platform=arguments.platform,
        architecture=arguments.architecture,
        axklib_version_metadata=arguments.axklib_version_metadata,
        package_basename_file=arguments.package_basename_file,
        sbom=arguments.sbom,
        configuration=arguments.configuration,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
