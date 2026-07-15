from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path

SEMVER_PATTERN = re.compile(
    r"^(0|[1-9][0-9]*)\."
    r"(0|[1-9][0-9]*)\."
    r"(0|[1-9][0-9]*)"
    r"(?:-((?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)"
    r"(?:\.(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*))*))?"
    r"(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$"
)


@dataclass(frozen=True)
class VersionMetadata:
    schema_version: int
    semantic_version: str
    project_version: str
    major: int
    minor: int
    patch: int
    release_tag: str
    is_release: bool
    is_prerelease: bool


def read(path: Path) -> VersionMetadata:
    raw = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise ValueError("version metadata must be a JSON object")
    expected_keys = set(VersionMetadata.__dataclass_fields__)
    if set(raw) != expected_keys:
        raise ValueError("version metadata fields do not match schema 1")
    if type(raw["schema_version"]) is not int or raw["schema_version"] != 1:
        raise ValueError("unsupported version metadata schema")
    for key in ("semantic_version", "project_version", "release_tag"):
        if not isinstance(raw[key], str):
            raise ValueError(f"version metadata {key} must be a string")
    for key in ("major", "minor", "patch"):
        if type(raw[key]) is not int or raw[key] < 0:
            raise ValueError(f"version metadata {key} must be a non-negative integer")
    for key in ("is_release", "is_prerelease"):
        if type(raw[key]) is not bool:
            raise ValueError(f"version metadata {key} must be a boolean")

    value = VersionMetadata(**raw)
    match = SEMVER_PATTERN.fullmatch(value.semantic_version)
    if not match:
        raise ValueError("semantic_version is not valid SemVer")
    core = ".".join(match.group(index) for index in range(1, 4))
    if value.project_version != core:
        raise ValueError("project_version does not match the semantic version numeric core")
    if (value.major, value.minor, value.patch) != tuple(
        int(match.group(index)) for index in range(1, 4)
    ):
        raise ValueError("numeric version components do not match project_version")
    if value.is_release != bool(value.release_tag):
        raise ValueError("release_tag and is_release disagree")
    if value.is_release and value.release_tag != f"v{value.semantic_version}":
        raise ValueError("release_tag does not match semantic_version")
    if not value.is_release and value.semantic_version != "0.0.0":
        raise ValueError("non-release version metadata must use 0.0.0")
    if value.is_prerelease != (match.group(4) is not None):
        raise ValueError("is_prerelease does not match semantic_version")
    return value
