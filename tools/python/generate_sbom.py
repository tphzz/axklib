#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
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


@dataclass(frozen=True)
class LicenseMaterial:
    component: str
    path: Path


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


def cargo_packages(metadata_file: Path) -> tuple[list[dict[str, object]], list[LicenseMaterial]]:
    loaded: object = json.loads(metadata_file.read_text(encoding="utf-8"))
    if (
        not isinstance(loaded, dict)
        or not isinstance(loaded.get("packages"), list)
        or not isinstance(loaded.get("resolve"), dict)
        or not isinstance(loaded["resolve"].get("nodes"), list)
    ):
        raise ValueError("Cargo metadata must contain a resolved package graph")
    resolved_ids = {
        value.get("id")
        for value in loaded["resolve"]["nodes"]
        if isinstance(value, dict) and isinstance(value.get("id"), str)
    }
    rows: list[dict[str, object]] = []
    materials: list[LicenseMaterial] = []
    for value in loaded["packages"]:
        if not isinstance(value, dict):
            raise ValueError("Cargo package metadata must be an object")
        name = value.get("name")
        version = value.get("version")
        package_id = value.get("id")
        license_expression = value.get("license")
        license_file = value.get("license_file")
        manifest_path = value.get("manifest_path")
        if package_id not in resolved_ids:
            continue
        if (
            not isinstance(name, str)
            or not name
            or not isinstance(version, str)
            or not version
            or not isinstance(manifest_path, str)
            or not manifest_path
        ):
            raise ValueError("Cargo package identity is incomplete")
        if name == "axkdeck":
            continue
        if not isinstance(license_expression, str) or not license_expression:
            if not isinstance(license_file, str) or not license_file:
                raise ValueError(f"Cargo package {name}@{version} has no declared license")
            license_expression = f"LicenseRef-{re.sub(r'[^A-Za-z0-9.-]', '-', name)}"
        row = package(name, version, "crates.io", license_expression=license_expression)
        row["externalRefs"] = [
            {
                "referenceCategory": "PACKAGE-MANAGER",
                "referenceType": "purl",
                "referenceLocator": f"pkg:cargo/{quote(name, safe='')}@{version}",
            }
        ]
        rows.append(row)
        directory = Path(manifest_path).parent
        candidates = [Path(license_file)] if isinstance(license_file, str) and license_file else []
        candidates.extend(
            path
            for path in directory.iterdir()
            if path.is_file()
            and path.name.upper().startswith(("LICENSE", "COPYING", "NOTICE", "COPYRIGHT"))
        )
        for candidate in candidates:
            path = candidate if candidate.is_absolute() else directory / candidate
            if path.is_file():
                materials.append(LicenseMaterial(f"Cargo {name} {version}", path))
    return rows, materials


def pnpm_runtime_packages(
    license_file: Path,
) -> tuple[list[dict[str, object]], list[LicenseMaterial]]:
    loaded: object = json.loads(license_file.read_text(encoding="utf-8"))
    if not isinstance(loaded, dict):
        raise ValueError("pnpm license inventory must be an object")
    rows: list[dict[str, object]] = []
    materials: list[LicenseMaterial] = []
    seen: set[tuple[str, str]] = set()
    for declared_license, packages in loaded.items():
        if not isinstance(declared_license, str) or not declared_license or declared_license == "UNKNOWN":
            raise ValueError("pnpm runtime package has no declared license")
        if not isinstance(packages, list):
            raise ValueError("pnpm license group must be an array")
        for value in packages:
            if not isinstance(value, dict):
                raise ValueError("pnpm package license entry must be an object")
            name = value.get("name")
            versions = value.get("versions")
            paths = value.get("paths")
            if (
                not isinstance(name, str)
                or not name
                or not isinstance(versions, list)
                or not versions
                or not all(isinstance(version, str) and version for version in versions)
                or not isinstance(paths, list)
                or not all(isinstance(path, str) and path for path in paths)
            ):
                raise ValueError("pnpm package license entry is incomplete")
            for version in versions:
                identity = (name, version)
                if identity in seen:
                    continue
                seen.add(identity)
                row = package(name, version, "npm", license_expression=declared_license)
                row["externalRefs"] = [
                    {
                        "referenceCategory": "PACKAGE-MANAGER",
                        "referenceType": "purl",
                        "referenceLocator": f"pkg:npm/{quote(name, safe='/')}@{version}",
                    }
                ]
                rows.append(row)
            for raw_path in paths:
                directory = Path(raw_path)
                for path in directory.iterdir():
                    if path.is_file() and path.name.upper().startswith(
                        ("LICENSE", "COPYING", "NOTICE", "COPYRIGHT")
                    ):
                        materials.append(
                            LicenseMaterial(f"pnpm {name} {', '.join(versions)}", path)
                        )
    return rows, materials


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


def vcpkg_license_materials(
    installed_root: Path, packages: list[dict[str, object]]
) -> list[LicenseMaterial]:
    names = {str(row["name"]) for row in packages if row["supplier"] == "Organization: vcpkg"}
    found: dict[str, Path] = {}
    for path in installed_root.rglob("copyright"):
        name = path.parent.name
        if name in names and name not in found:
            found[name] = path
    missing = sorted(names - found.keys())
    if missing:
        raise ValueError(f"vcpkg license material is missing for: {', '.join(missing)}")
    return [LicenseMaterial(f"vcpkg {name}", found[name]) for name in sorted(found)]


def validate_declared_licenses(packages: list[dict[str, object]]) -> None:
    unresolved = sorted(
        f"{row['name']}@{row['versionInfo']}"
        for row in packages
        if row.get("licenseDeclared") in {None, "", "NOASSERTION"}
    )
    if unresolved:
        raise ValueError(f"distributed packages have unresolved licenses: {', '.join(unresolved)}")


def write_license_bundle(
    output: Path, packages: list[dict[str, object]], materials: list[LicenseMaterial]
) -> None:
    validate_declared_licenses(packages)
    sections = [
        "axkdeck third-party license inventory",
        "========================================",
        "",
        "Distributed components and declared licenses:",
        "",
    ]
    for row in sorted(packages, key=lambda value: (str(value["name"]), str(value["versionInfo"]))):
        sections.append(
            f"- {row['name']} {row['versionInfo']}: {row['licenseDeclared']}"
        )
    sections.extend(["", "Bundled license and notice texts", "================================", ""])
    grouped: dict[str, tuple[list[str], str]] = {}
    for material in materials:
        raw = material.path.read_bytes()
        if len(raw) > 2 * 1024 * 1024 or b"\0" in raw:
            raise ValueError(f"license material is not bounded text: {material.path}")
        digest = hashlib.sha256(raw).hexdigest()
        text = raw.decode("utf-8", errors="replace").replace("\r\n", "\n").rstrip()
        if digest in grouped:
            grouped[digest][0].append(material.component)
        else:
            grouped[digest] = ([material.component], text)
    for digest in sorted(grouped):
        components, text = grouped[digest]
        sections.extend(
            [
                f"Components: {', '.join(sorted(set(components)))}",
                f"SHA-256: {digest}",
                "",
                text,
                "",
                "------------------------------------------------------------------------",
                "",
            ]
        )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(sections).rstrip() + "\n", encoding="utf-8", newline="\n")


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
    parser.add_argument("--cargo-metadata-file", type=Path, action="append")
    parser.add_argument("--pnpm-license-file", type=Path)
    parser.add_argument("--vcpkg-installed-root", type=Path)
    parser.add_argument("--license-bundle", type=Path)
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
    materials: list[LicenseMaterial] = []
    product = "axklib"
    if args.axkdeck_root:
        if not args.cargo_metadata_file or not args.pnpm_license_file:
            parser.error(
                "--axkdeck-root requires --cargo-metadata-file and --pnpm-license-file"
            )
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
        cargo_rows: list[dict[str, object]] = []
        cargo_materials: list[LicenseMaterial] = []
        for metadata_file in args.cargo_metadata_file:
            rows, rows_materials = cargo_packages(metadata_file)
            cargo_rows.extend(rows)
            cargo_materials.extend(rows_materials)
        pnpm_rows, pnpm_materials = pnpm_runtime_packages(args.pnpm_license_file)
        packages.extend(cargo_rows)
        packages.extend(pnpm_rows)
        materials.extend(cargo_materials)
        materials.extend(pnpm_materials)
    validate_declared_licenses(packages)
    if args.license_bundle:
        if not args.axkdeck_root or not args.vcpkg_installed_root:
            parser.error(
                "--license-bundle requires --axkdeck-root and --vcpkg-installed-root"
            )
        materials.extend(vcpkg_license_materials(args.vcpkg_installed_root, packages))
        write_license_bundle(args.license_bundle, packages, materials)
    document = sbom_document(
        args.profile, source_identity, packages, creation_timestamp(), product=product
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(document, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
