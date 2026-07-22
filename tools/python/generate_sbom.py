#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import tomllib
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from urllib.parse import quote

import release_metadata
import version_metadata

LICENSE_OVERRIDES = {"soxr": "LGPL-2.1-or-later"}


@dataclass(frozen=True)
class PnpmIdentity:
    name: str
    version: str
    peer_context: str = ""


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


def parse_pnpm_identity(value: str) -> PnpmIdentity:
    if not value or value != value.strip() or "'" in value or '"' in value:
        raise ValueError(f"invalid pnpm package identity: {value!r}")
    if value.startswith("@"):
        slash = value.find("/")
        separator = value.find("@", slash + 1) if slash > 1 else -1
    else:
        separator = value.find("@")
    if separator <= 0 or separator + 1 >= len(value):
        raise ValueError(f"invalid pnpm package identity: {value!r}")
    name = value[:separator]
    qualified_version = value[separator + 1 :]
    context_at = qualified_version.find("(")
    version = qualified_version if context_at < 0 else qualified_version[:context_at]
    peer_context = "" if context_at < 0 else qualified_version[context_at:]
    if not version or any(character.isspace() for character in name + version):
        raise ValueError(f"invalid pnpm package identity: {value!r}")
    if peer_context and (
        not peer_context.startswith("(") or peer_context.count("(") != peer_context.count(")")
    ):
        raise ValueError(f"invalid pnpm package identity: {value!r}")
    return PnpmIdentity(name, version, peer_context)


def pnpm_packages(lockfile: Path) -> list[dict[str, object]]:
    import yaml

    loaded: object = yaml.safe_load(lockfile.read_text(encoding="utf-8"))
    if not isinstance(loaded, dict) or str(loaded.get("lockfileVersion")) != "9.0":
        raise ValueError("pnpm lockfile version 9 is required")
    identities: dict[tuple[str, str], set[str]] = {}
    for section_name in ("packages", "snapshots"):
        section = loaded.get(section_name, {})
        if not isinstance(section, dict):
            raise ValueError(f"pnpm {section_name} section must be a mapping")
        for raw_key in section:
            if not isinstance(raw_key, str):
                raise ValueError("pnpm package identity must be a string")
            identity = parse_pnpm_identity(raw_key)
            contexts = identities.setdefault((identity.name, identity.version), set())
            if identity.peer_context:
                contexts.add(identity.peer_context)
    rows: list[dict[str, object]] = []
    for (name, version), contexts in sorted(identities.items()):
        comment = f"pnpm peer contexts: {', '.join(sorted(contexts))}" if contexts else None
        row = package(name, version, "npm", comment=comment)
        row["externalRefs"] = [
            {
                "referenceCategory": "PACKAGE-MANAGER",
                "referenceType": "purl",
                "referenceLocator": f"pkg:npm/{quote(name, safe='/')}@{version}",
            }
        ]
        rows.append(row)
    return rows


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


def source_hashes(portfile: Path) -> list[str]:
    return sorted(set(re.findall(r"(?m)^\s*SHA512\s+([0-9a-fA-F]{128})\s*$", portfile.read_text())))


def vcpkg_packages(root: Path, profile: str) -> list[dict[str, object]]:
    manifest = json.loads((root / "vcpkg.json").read_text(encoding="utf-8"))
    selected = list(manifest["dependencies"])
    if profile in {"cli", "server", "workspace"}:
        selected.extend(manifest["features"]["application"]["dependencies"])
    if profile in {"cli", "workspace"}:
        selected.extend(manifest["features"]["cli"]["dependencies"])
    if profile in {"server", "workspace"}:
        selected.extend(manifest["features"]["server"]["dependencies"])
    if profile == "workspace":
        selected.extend(manifest["features"]["tests"]["dependencies"])
    queue = [(item, dependency_features(item)) for item in selected]
    requested: dict[str, set[str]] = {}
    metadata: dict[str, dict[str, object]] = {}
    metadata_paths: dict[str, Path] = {}
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
        metadata_paths[name] = path
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
        hashes = source_hashes(metadata_paths[name].parent / "portfile.cmake")
        hash_note = f"; source SHA512: {','.join(hashes)}" if hashes else ""
        rows.append(
            package(
                name,
                port_version(value),
                "vcpkg",
                license_expression=str(license_value),
                download_location=str(homepage),
                comment=f"vcpkg baseline {baseline}; features: {','.join(features) or 'core'}{hash_note}",
            )
        )
    return rows


def sbom_document(
    profile: str,
    source_identity: str,
    packages: list[dict[str, object]],
    created: str,
    *,
    product: str = "axklib",
) -> dict[str, object]:
    unique = {str(row["SPDXID"]): row for row in packages}
    ordered_packages = [unique[key] for key in sorted(unique)]
    name = f"{product}-{profile}-release" if product == "axklib" else f"{product}-release"
    identity = {
        "name": name,
        "profile": profile,
        "source_identity": source_identity,
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
        "comment": f"axklib source identity: {source_identity}",
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
    parser.add_argument("--version-metadata-file", required=True, type=Path)
    parser.add_argument("--package-basename-file", required=True, type=Path)
    parser.add_argument("--axkdeck-root", type=Path)
    parser.add_argument(
        "--profile", choices=("sdk", "cli", "server", "workspace"), default="workspace"
    )
    args = parser.parse_args()
    version = version_metadata.read(args.version_metadata_file).semantic_version
    source_identity = release_metadata.read_package_basename(
        args.package_basename_file
    ).removeprefix("axklib-")
    packages = [package("axklib", version, "axklib", license_expression="MPL-2.0")]
    packages.extend(vcpkg_packages(args.axklib_root, args.profile))
    product = "axklib"
    if args.axkdeck_root:
        product = "axkdeck"
        packages.append(
            package(
                "axkdeck",
                version,
                "axkdeck",
                license_expression="MIT OR Apache-2.0",
                comment=f"monorepo source identity: {source_identity}",
            )
        )
        cargo = tomllib.loads((args.axkdeck_root / "src-tauri/Cargo.lock").read_text())
        packages.extend(
            package(item["name"], item["version"], "crates.io")
            for item in cargo["package"]
            if item["name"] != "axkdeck"
        )
        packages.extend(pnpm_packages(args.axkdeck_root / "pnpm-lock.yaml"))
    document = sbom_document(
        args.profile, source_identity, packages, creation_timestamp(), product=product
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(document, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
