from __future__ import annotations

import json
from pathlib import Path

import architecture_check


def write_lines(path: Path, count: int, newline: bytes = b"\n") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(newline.join([b"line"] * count) + newline)


def write_policy(
    root: Path,
    *,
    legacy_files: dict[str, int] | None = None,
    exclusions: list[dict[str, str]] | None = None,
) -> Path:
    policy = {
        "version": 1,
        "maximumLines": 700,
        "sourceRoots": [{"path": "src", "extensions": [".cpp", ".ts"]}],
        "classifiedExclusions": exclusions or [],
        "legacyFiles": legacy_files or {},
    }
    path = root / "policy.json"
    path.write_text(json.dumps(policy), encoding="utf-8")
    return path


def issue_codes(root: Path, policy: Path) -> list[str]:
    return [issue.code for issue in architecture_check.check_repository(root, policy)]


def test_accepts_file_at_limit_and_matching_legacy_file(tmp_path: Path) -> None:
    write_lines(tmp_path / "src/at-limit.cpp", 700)
    write_lines(tmp_path / "src/legacy.cpp", 701)
    policy = write_policy(tmp_path, legacy_files={"src/legacy.cpp": 701})

    assert architecture_check.check_repository(tmp_path, policy) == []


def test_rejects_new_oversized_file_and_legacy_growth(tmp_path: Path) -> None:
    write_lines(tmp_path / "src/new.cpp", 701)
    write_lines(tmp_path / "src/legacy.cpp", 702)
    policy = write_policy(tmp_path, legacy_files={"src/legacy.cpp": 701})

    assert issue_codes(tmp_path, policy) == ["new_file_over_budget", "legacy_count_increased"]


def test_requires_reduced_legacy_budget_to_be_recorded(tmp_path: Path) -> None:
    write_lines(tmp_path / "src/legacy.cpp", 701)
    policy = write_policy(tmp_path, legacy_files={"src/legacy.cpp": 702})

    assert issue_codes(tmp_path, policy) == ["legacy_baseline_stale"]


def test_requires_legacy_entry_removal_at_or_below_limit(tmp_path: Path) -> None:
    write_lines(tmp_path / "src/legacy.cpp", 700)
    policy = write_policy(tmp_path, legacy_files={"src/legacy.cpp": 701})

    assert issue_codes(tmp_path, policy) == ["legacy_entry_unnecessary"]


def test_rejects_missing_and_nonproduction_legacy_entries(tmp_path: Path) -> None:
    write_lines(tmp_path / "src/ordinary.cpp", 1)
    write_lines(tmp_path / "outside/file.cpp", 701)
    policy = write_policy(
        tmp_path,
        legacy_files={"src/missing.cpp": 701, "outside/file.cpp": 701},
    )

    assert issue_codes(tmp_path, policy) == [
        "legacy_file_missing",
        "legacy_file_not_production",
    ]


def test_explicit_generated_exclusion_is_not_counted(tmp_path: Path) -> None:
    write_lines(tmp_path / "src/generated/api.ts", 900)
    policy = write_policy(
        tmp_path,
        exclusions=[
            {
                "path": "src/generated",
                "classification": "generated",
                "reason": "Generated API contract",
            }
        ],
    )

    assert architecture_check.check_repository(tmp_path, policy) == []


def test_rejects_unknown_or_unexplained_exclusion(tmp_path: Path) -> None:
    write_lines(tmp_path / "src/generated/api.ts", 900)
    policy = write_policy(
        tmp_path,
        exclusions=[
            {
                "path": "src/generated",
                "classification": "convenient",
                "reason": "",
            }
        ],
    )

    assert issue_codes(tmp_path, policy) == [
        "invalid_exclusion_classification",
        "missing_exclusion_reason",
    ]


def test_ignores_test_sources_without_classifying_them_as_production(tmp_path: Path) -> None:
    write_lines(tmp_path / "src/large.test.ts", 900)
    write_lines(tmp_path / "src/testing/helper.ts", 900)
    policy = write_policy(tmp_path)

    assert architecture_check.check_repository(tmp_path, policy) == []


def test_counts_crlf_and_unterminated_final_lines(tmp_path: Path) -> None:
    path = tmp_path / "source.cpp"
    path.write_bytes(b"one\r\ntwo\r\nthree")

    assert architecture_check.physical_line_count(path) == 3
