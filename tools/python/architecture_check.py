#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any

ALLOWED_EXCLUSION_CLASSIFICATIONS = {"generated", "machine-maintained", "vendored"}
TEST_DIRECTORY_NAMES = {"test", "tests", "testing"}
TEST_FILE_MARKERS = (".test.", "_test.")


@dataclass(frozen=True)
class ArchitectureIssue:
    code: str
    path: str
    message: str


def physical_line_count(path: Path) -> int:
    content = path.read_bytes()
    if not content:
        return 0
    return content.count(b"\n") + (0 if content.endswith(b"\n") else 1)


def _relative_path(value: Any, field: str) -> PurePosixPath:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{field} must be a non-empty relative path")
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or path.as_posix() != value:
        raise ValueError(f"{field} must be a normalized relative POSIX path: {value!r}")
    return path


def _is_test_source(path: PurePosixPath) -> bool:
    return any(part in TEST_DIRECTORY_NAMES for part in path.parts) or any(
        marker in path.name for marker in TEST_FILE_MARKERS
    )


def _is_under(path: PurePosixPath, directory: PurePosixPath) -> bool:
    return path == directory or directory in path.parents


def _load_policy(path: Path) -> dict[str, Any]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict) or document.get("version") != 1:
        raise ValueError("architecture policy must be an object with version 1")
    maximum = document.get("maximumLines")
    if not isinstance(maximum, int) or isinstance(maximum, bool) or maximum < 1:
        raise ValueError("maximumLines must be a positive integer")
    for field in ("sourceRoots", "classifiedExclusions"):
        if not isinstance(document.get(field), list):
            raise ValueError(f"{field} must be an array")
    if not isinstance(document.get("legacyFiles"), dict):
        raise ValueError("legacyFiles must be an object")
    return document


def _discover_production_files(
    root: Path,
    source_roots: list[Any],
    exclusions: list[PurePosixPath],
    issues: list[ArchitectureIssue],
) -> dict[str, Path]:
    discovered: dict[str, Path] = {}
    for index, entry in enumerate(source_roots):
        if not isinstance(entry, dict):
            raise ValueError(f"sourceRoots[{index}] must be an object")
        relative_root = _relative_path(entry.get("path"), f"sourceRoots[{index}].path")
        extensions = entry.get("extensions")
        if (
            not isinstance(extensions, list)
            or not extensions
            or any(not isinstance(value, str) or not value.startswith(".") for value in extensions)
        ):
            raise ValueError(f"sourceRoots[{index}].extensions must contain file extensions")
        source_root = root.joinpath(*relative_root.parts)
        if not source_root.exists():
            issues.append(
                ArchitectureIssue(
                    "source_root_missing",
                    relative_root.as_posix(),
                    "Configured production source root does not exist",
                )
            )
            continue
        candidates = [source_root] if source_root.is_file() else source_root.rglob("*")
        for candidate in candidates:
            if not candidate.is_file() or candidate.suffix not in extensions:
                continue
            relative = PurePosixPath(candidate.relative_to(root).as_posix())
            if _is_test_source(relative) or any(_is_under(relative, item) for item in exclusions):
                continue
            discovered[relative.as_posix()] = candidate
    return discovered


def check_repository(root: Path, policy_path: Path) -> list[ArchitectureIssue]:
    root = root.resolve()
    policy = _load_policy(policy_path)
    issues: list[ArchitectureIssue] = []
    exclusions: list[PurePosixPath] = []
    for index, entry in enumerate(policy["classifiedExclusions"]):
        if not isinstance(entry, dict):
            raise ValueError(f"classifiedExclusions[{index}] must be an object")
        relative = _relative_path(entry.get("path"), f"classifiedExclusions[{index}].path")
        exclusions.append(relative)
        classification = entry.get("classification")
        reason = entry.get("reason")
        may_be_absent = entry.get("mayBeAbsent", False)
        if classification not in ALLOWED_EXCLUSION_CLASSIFICATIONS:
            issues.append(
                ArchitectureIssue(
                    "invalid_exclusion_classification",
                    relative.as_posix(),
                    "Exclusions must be generated, machine-maintained, or vendored",
                )
            )
        if not isinstance(reason, str) or not reason.strip():
            issues.append(
                ArchitectureIssue(
                    "missing_exclusion_reason",
                    relative.as_posix(),
                    "Classified exclusions require a non-empty reason",
                )
            )
        if not isinstance(may_be_absent, bool):
            issues.append(
                ArchitectureIssue(
                    "invalid_exclusion_presence",
                    relative.as_posix(),
                    "mayBeAbsent must be a boolean when specified",
                )
            )
        if not root.joinpath(*relative.parts).exists() and may_be_absent is not True:
            issues.append(
                ArchitectureIssue(
                    "exclusion_path_missing",
                    relative.as_posix(),
                    "Classified exclusion path does not exist",
                )
            )

    production = _discover_production_files(
        root, policy["sourceRoots"], exclusions, issues
    )
    maximum = policy["maximumLines"]
    raw_legacy = policy["legacyFiles"]
    legacy: dict[str, int] = {}
    for raw_path, raw_count in raw_legacy.items():
        relative = _relative_path(raw_path, "legacyFiles key")
        if not isinstance(raw_count, int) or isinstance(raw_count, bool) or raw_count <= maximum:
            raise ValueError(
                f"legacyFiles[{relative.as_posix()!r}] must exceed maximumLines"
            )
        legacy[relative.as_posix()] = raw_count

    counts = {path: physical_line_count(file) for path, file in production.items()}
    for path in sorted(counts):
        if counts[path] > maximum and path not in legacy:
            issues.append(
                ArchitectureIssue(
                    "new_file_over_budget",
                    path,
                    f"{counts[path]} physical lines exceeds the {maximum}-line limit",
                )
            )

    missing_entries: list[tuple[str, int]] = []
    nonproduction_entries: list[tuple[str, int]] = []
    active_entries: list[tuple[str, int]] = []
    for path, recorded in sorted(legacy.items()):
        absolute = root.joinpath(*PurePosixPath(path).parts)
        if not absolute.exists():
            missing_entries.append((path, recorded))
        elif path not in production:
            nonproduction_entries.append((path, recorded))
        else:
            active_entries.append((path, recorded))

    for path, _ in missing_entries:
        issues.append(
            ArchitectureIssue("legacy_file_missing", path, "Legacy file no longer exists")
        )
    for path, _ in nonproduction_entries:
        issues.append(
            ArchitectureIssue(
                "legacy_file_not_production",
                path,
                "Legacy entry does not identify a scanned production file",
            )
        )
    for path, recorded in active_entries:
        current = counts[path]
        if current <= maximum:
            issues.append(
                ArchitectureIssue(
                    "legacy_entry_unnecessary",
                    path,
                    f"{current} physical lines is within the {maximum}-line limit",
                )
            )
        elif current > recorded:
            issues.append(
                ArchitectureIssue(
                    "legacy_count_increased",
                    path,
                    f"{current} physical lines exceeds the recorded legacy count {recorded}",
                )
            )
        elif current < recorded:
            issues.append(
                ArchitectureIssue(
                    "legacy_baseline_stale",
                    path,
                    f"Lower the recorded count from {recorded} to the current {current}",
                )
            )
    return issues


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--policy", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    policy = args.policy or root / "cmake" / "architecture-line-budget.json"
    try:
        issues = check_repository(root, policy)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(json.dumps({"valid": False, "error": str(error)}, indent=2))
        return 2
    print(
        json.dumps(
            {
                "valid": not issues,
                "issues": [issue.__dict__ for issue in issues],
            },
            indent=2,
        )
    )
    return 0 if not issues else 1


if __name__ == "__main__":
    raise SystemExit(main())
