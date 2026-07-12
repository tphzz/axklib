#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import re
import tomllib
from pathlib import Path


def package(name: str, version: str, supplier: str) -> dict[str, object]:
    identifier = re.sub(r"[^A-Za-z0-9.-]", "-", f"SPDXRef-{supplier}-{name}-{version}")
    return {
        "SPDXID": identifier,
        "name": name,
        "versionInfo": version,
        "supplier": f"Organization: {supplier}",
        "downloadLocation": "NOASSERTION",
        "filesAnalyzed": False,
        "licenseConcluded": "NOASSERTION",
        "licenseDeclared": "NOASSERTION",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--axklib-root", type=Path, default=Path.cwd())
    parser.add_argument("--axkdeck-root", type=Path)
    args = parser.parse_args()
    packages = [package("axklib", "0.1.0", "axklib")]
    vcpkg = json.loads((args.axklib_root / "vcpkg.json").read_text())
    for dependency in vcpkg["dependencies"]:
        name = dependency if isinstance(dependency, str) else dependency["name"]
        packages.append(package(name, "locked-vcpkg-baseline", "vcpkg"))
    if args.axkdeck_root:
        cargo = tomllib.loads((args.axkdeck_root / "src-tauri/Cargo.lock").read_text())
        packages.extend(
            package(item["name"], item["version"], "crates.io") for item in cargo["package"]
        )
        lock_text = (args.axkdeck_root / "pnpm-lock.yaml").read_text()
        npm_rows = set(re.findall(r"^  ([^\s:]+)@([^\s:]+):$", lock_text, re.MULTILINE))
        packages.extend(package(name, version, "npm") for name, version in sorted(npm_rows))
    unique = {row["SPDXID"]: row for row in packages}
    namespace_hash = hashlib.sha256("\n".join(sorted(unique)).encode()).hexdigest()
    document = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": "axklib-native-release",
        "documentNamespace": f"https://axklib.invalid/spdx/{namespace_hash}",
        "creationInfo": {"creators": ["Tool: axklib-generate-sbom"], "created": "2026-07-11T00:00:00Z"},
        "packages": [unique[key] for key in sorted(unique)],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(document, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
