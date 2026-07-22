from __future__ import annotations

import argparse
import json
import re
import shutil
from pathlib import Path
from typing import Any

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
    package_json: Path,
    axklib_version_metadata: Path,
    sbom: Path,
) -> list[Path]:
    expected_extensions = DESKTOP_PACKAGE_EXTENSIONS.get((platform, architecture))
    if expected_extensions is None:
        raise ValueError(f"unsupported desktop release target: {platform}-{architecture}")

    package_data: dict[str, Any] = json.loads(package_json.read_text(encoding="utf-8"))
    version = package_data.get("version")
    if not isinstance(version, str) or not re.fullmatch(
        r"[0-9]+[.][0-9]+[.][0-9]+(?:[-+][A-Za-z0-9.+-]+)?", version
    ):
        raise ValueError(f"invalid axkdeck version: {version!r}")
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

    native_data: dict[str, Any] = json.loads(axklib_version_metadata.read_text(encoding="utf-8"))
    sbom_data: dict[str, Any] = json.loads(sbom.read_text(encoding="utf-8"))
    sbom_comment = sbom_data.get("comment")
    if not isinstance(sbom_comment, str) or not sbom_comment.startswith(SOURCE_IDENTITY_PREFIX):
        raise ValueError("desktop SBOM does not declare the axklib source identity")
    source_identity = sbom_comment.removeprefix(SOURCE_IDENTITY_PREFIX)
    if not source_identity:
        raise ValueError("desktop SBOM contains an empty axklib source identity")
    native_version = native_data.get("semantic_version")
    if not isinstance(native_version, str) or not native_version:
        raise ValueError("axklib version metadata does not declare a semantic version")
    stem = f"axkdeck-{version}-{platform}-{architecture}"
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
    parser.add_argument("--package-json", type=Path, required=True)
    parser.add_argument("--axklib-version-metadata", type=Path, required=True)
    parser.add_argument("--sbom", type=Path, required=True)
    arguments = parser.parse_args()
    stage_release_packages(
        bundle_directory=arguments.bundle_directory,
        extensions={value.lower() for value in arguments.extension},
        output_directory=arguments.output_directory,
        platform=arguments.platform,
        architecture=arguments.architecture,
        package_json=arguments.package_json,
        axklib_version_metadata=arguments.axklib_version_metadata,
        sbom=arguments.sbom,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
