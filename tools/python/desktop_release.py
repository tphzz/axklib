from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any

SOURCE_IDENTITY_PREFIX = "axklib source identity: "


def _sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_release_metadata(
    *,
    bundle_directory: Path,
    extensions: set[str],
    output_directory: Path,
    commit: str,
    platform: str,
    architecture: str,
    package_json: Path,
    axklib_version_metadata: Path,
    sbom: Path,
) -> tuple[Path, Path]:
    packages = sorted(
        path
        for path in bundle_directory.rglob("*")
        if path.is_file() and path.suffix.lower() in extensions
    )
    if not packages:
        rendered = ", ".join(sorted(extensions))
        raise ValueError(f"no desktop packages with extensions {rendered} in {bundle_directory}")

    package_data: dict[str, Any] = json.loads(package_json.read_text(encoding="utf-8"))
    native_data: dict[str, Any] = json.loads(axklib_version_metadata.read_text(encoding="utf-8"))
    sbom_data: dict[str, Any] = json.loads(sbom.read_text(encoding="utf-8"))
    sbom_comment = sbom_data.get("comment")
    if not isinstance(sbom_comment, str) or not sbom_comment.startswith(SOURCE_IDENTITY_PREFIX):
        raise ValueError("desktop SBOM does not declare the axklib source identity")
    source_identity = sbom_comment.removeprefix(SOURCE_IDENTITY_PREFIX)
    if not source_identity:
        raise ValueError("desktop SBOM contains an empty axklib source identity")
    version = str(package_data["version"])
    stem = f"axkdeck-{version}-{platform}-{architecture}"
    entries = [
        {
            "file": path.name,
            "size_bytes": path.stat().st_size,
            "sha256": _sha256(path),
        }
        for path in packages
    ]
    manifest = {
        "schema_version": "1.0",
        "product": "axkdeck",
        "version": version,
        "commit": commit,
        "platform": platform,
        "architecture": architecture,
        "axklib_version": native_data["semantic_version"],
        "axklib_source_identity": source_identity,
        "sbom_sha256": _sha256(sbom),
        "packages": entries,
    }

    output_directory.mkdir(parents=True, exist_ok=True)
    manifest_path = output_directory / f"{stem}-manifest.json"
    checksums_path = output_directory / f"{stem}-SHA256SUMS"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    checksums_path.write_text(
        "".join(f"{entry['sha256']}  {entry['file']}\n" for entry in entries),
        encoding="utf-8",
    )
    return manifest_path, checksums_path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle-directory", type=Path, required=True)
    parser.add_argument("--extension", action="append", required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--architecture", required=True)
    parser.add_argument("--package-json", type=Path, required=True)
    parser.add_argument("--axklib-version-metadata", type=Path, required=True)
    parser.add_argument("--sbom", type=Path, required=True)
    arguments = parser.parse_args()
    write_release_metadata(
        bundle_directory=arguments.bundle_directory,
        extensions={value.lower() for value in arguments.extension},
        output_directory=arguments.output_directory,
        commit=arguments.commit,
        platform=arguments.platform,
        architecture=arguments.architecture,
        package_json=arguments.package_json,
        axklib_version_metadata=arguments.axklib_version_metadata,
        sbom=arguments.sbom,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
