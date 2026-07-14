#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import tomllib
from datetime import UTC, datetime
from pathlib import Path

LICENSE_OVERRIDES = {"soxr": "LGPL-2.1-or-later"}


def project_version(root: Path) -> str:
    cmake_text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(
        r"project\s*\(\s*axklib\b.*?\bVERSION\s+([^\s\)]+)",
        cmake_text,
        re.DOTALL | re.IGNORECASE,
    )
    if not match:
        raise ValueError("could not derive axklib version from CMakeLists.txt")
    cmake_version = match.group(1)
    manifest = json.loads((root / "vcpkg.json").read_text(encoding="utf-8"))
    manifest_version = manifest.get("version-semver")
    if manifest_version != cmake_version:
        raise ValueError(
            "axklib version mismatch: "
            f"CMakeLists.txt has {cmake_version}, vcpkg.json has {manifest_version}"
        )
    return cmake_version


def creation_timestamp() -> str:
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    instant = datetime.fromtimestamp(int(epoch), UTC) if epoch else datetime.now(UTC)
    return instant.replace(microsecond=0).strftime("%Y-%m-%dT%H:%M:%SZ")


def package(
    name: str,
    version: str,
    supplier: str,
    *,
    license_expression: str = "NOASSERTION",
    download_location: str = "NOASSERTION",
    comment: str | None = None,
) -> dict[str, object]:
    identifier = re.sub(r"[^A-Za-z0-9.-]", "-", f"SPDXRef-{supplier}-{name}-{version}")
    value: dict[str, object] = {
        "SPDXID": identifier,
        "name": name,
        "versionInfo": version,
        "supplier": f"Organization: {supplier}",
        "downloadLocation": download_location,
        "filesAnalyzed": False,
        "licenseConcluded": "NOASSERTION",
        "licenseDeclared": license_expression,
    }
    if comment:
        value["comment"] = comment
    return value


def dependency_name(value: object) -> str:
    if isinstance(value, str):
        return value
    if isinstance(value, dict):
        name = value.get("name")
        if isinstance(name, str):
            return name
    raise ValueError("vcpkg dependency has no name")


def dependency_features(value: object) -> list[str]:
    if isinstance(value, dict):
        features = value.get("features", [])
        if not isinstance(features, list) or not all(isinstance(item, str) for item in features):
            raise ValueError("vcpkg dependency features are invalid")
        return features
    return []


def port_version(value: dict[str, object]) -> str:
    for key in ("version-semver", "version", "version-date", "version-string"):
        version = value.get(key)
        if isinstance(version, str):
            revision = value.get("port-version", 0)
            return f"{version}#{revision}" if revision else version
    return "unknown"


def vcpkg_packages(root: Path, profile: str) -> list[dict[str, object]]:
    manifest = json.loads((root / "vcpkg.json").read_text(encoding="utf-8"))
    selected = list(manifest["dependencies"])
    if profile in {"cli", "workspace"}:
        selected.extend(manifest["features"]["cli"]["dependencies"])
    if profile == "workspace":
        selected.extend(manifest["features"]["tests"]["dependencies"])
    queue = [(item, dependency_features(item)) for item in selected]
    requested: dict[str, set[str]] = {}
    metadata: dict[str, dict[str, object]] = {}
    while queue:
        dependency, explicit_features = queue.pop(0)
        name = dependency_name(dependency)
        if name.startswith("vcpkg-"):
            continue
        known = requested.setdefault(name, set())
        new_features = set(explicit_features) - known
        first_visit = name not in metadata
        if not first_visit and not new_features:
            continue
        known.update(new_features)
        overlay_path = root / "library/cmake/ports" / name / "vcpkg.json"
        path = (
            overlay_path
            if overlay_path.is_file()
            else root / "external/vcpkg/ports" / name / "vcpkg.json"
        )
        value = json.loads(path.read_text(encoding="utf-8"))
        metadata[name] = value
        dependencies = list(value.get("dependencies", [])) if first_visit else []
        active_features = set(new_features)
        uses_defaults = not (
            isinstance(dependency, dict) and dependency.get("default-features") is False
        )
        if first_visit and uses_defaults:
            defaults = value.get("default-features", [])
            if isinstance(defaults, list):
                active_features.update(item for item in defaults if isinstance(item, str))
                known.update(active_features)
        feature_table = value.get("features", {})
        if isinstance(feature_table, dict):
            for feature in active_features:
                row = feature_table.get(feature, {})
                if isinstance(row, dict):
                    dependencies.extend(row.get("dependencies", []))
        queue.extend((item, dependency_features(item)) for item in dependencies)
    baseline = str(manifest["builtin-baseline"])
    rows = []
    for name in sorted(metadata):
        value = metadata[name]
        license_value = value.get("license") or LICENSE_OVERRIDES.get(name, "NOASSERTION")
        homepage = value.get("homepage", "NOASSERTION")
        features = sorted(requested[name])
        rows.append(
            package(
                name,
                port_version(value),
                "vcpkg",
                license_expression=str(license_value),
                download_location=str(homepage),
                comment=f"vcpkg baseline {baseline}; features: {','.join(features) or 'core'}",
            )
        )
    return rows


def sbom_document(
    profile: str, packages: list[dict[str, object]], created: str
) -> dict[str, object]:
    unique = {str(row["SPDXID"]): row for row in packages}
    ordered_packages = [unique[key] for key in sorted(unique)]
    name = f"axklib-{profile}-release"
    identity = {
        "name": name,
        "profile": profile,
        "created": created,
        "packages": ordered_packages,
    }
    namespace_hash = hashlib.sha256(
        json.dumps(identity, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    return {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": name,
        "documentNamespace": f"https://github.com/tphzz/axklib/spdx/{namespace_hash}",
        "creationInfo": {
            "creators": ["Tool: axklib-generate-sbom"],
            "created": created,
        },
        "packages": ordered_packages,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--axklib-root", type=Path, default=Path.cwd())
    parser.add_argument("--axkdeck-root", type=Path)
    parser.add_argument("--profile", choices=("sdk", "cli", "workspace"), default="workspace")
    args = parser.parse_args()
    version = project_version(args.axklib_root)
    packages = [package("axklib", version, "axklib", license_expression="MPL-2.0")]
    packages.extend(vcpkg_packages(args.axklib_root, args.profile))
    if args.axkdeck_root:
        cargo = tomllib.loads((args.axkdeck_root / "src-tauri/Cargo.lock").read_text())
        packages.extend(
            package(item["name"], item["version"], "crates.io") for item in cargo["package"]
        )
        lock_text = (args.axkdeck_root / "pnpm-lock.yaml").read_text()
        npm_rows = set(re.findall(r"^  ([^\s:]+)@([^\s:]+):$", lock_text, re.MULTILINE))
        packages.extend(package(name, version, "npm") for name, version in sorted(npm_rows))
    document = sbom_document(args.profile, packages, creation_timestamp())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(document, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
