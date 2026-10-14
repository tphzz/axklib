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
    exclusions: list[dict[str, str | bool]] | None = None,
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


def test_native_contract_cannot_depend_on_a_sampler_or_conversion(tmp_path: Path) -> None:
    write_lines(tmp_path / "src/unit.cpp", 1)
    source = tmp_path / "library/src/native/document.cpp"
    source.parent.mkdir(parents=True)
    source.write_text('#include "axklib/formats/tx16w/codec.hpp"\n', encoding="utf-8")
    issues = architecture_check.check_repository(tmp_path, write_policy(tmp_path))
    assert any(issue.code == "module_dependency_forbidden" for issue in issues)


def test_native_format_cannot_include_writer_transport_or_another_format(tmp_path: Path) -> None:
    write_lines(tmp_path / "src/unit.cpp", 1)
    source = tmp_path / "library/include/axklib/formats/tx16w/document.hpp"
    source.parent.mkdir(parents=True)
    source.write_text(
        '#include "axklib/writer.hpp"\n'
        '#include "axklib/formats/a_series/document.hpp"\n'
        '#include <nlohmann/json.hpp>\n'
        '#include "../../writer.hpp"\n',
        encoding="utf-8",
    )
    issues = architecture_check.check_repository(tmp_path, write_policy(tmp_path))
    assert sum(issue.code == "module_dependency_forbidden" for issue in issues) == 4


def test_native_format_can_include_its_contract_and_shared_sources(tmp_path: Path) -> None:
    write_lines(tmp_path / "src/unit.cpp", 1)
    source = tmp_path / "library/src/formats/tx16w/document.cpp"
    source.parent.mkdir(parents=True)
    source.write_text(
        '#include "axklib/formats/tx16w/document.hpp"\n'
        '#include "axklib/native/source.hpp"\n#include <vector>\n',
        encoding="utf-8",
    )
    assert architecture_check.check_repository(tmp_path, write_policy(tmp_path)) == []


def test_a_series_owner_cannot_depend_on_conversion_or_transport(tmp_path: Path) -> None:
    write_lines(tmp_path / "src/unit.cpp", 1)
    source = tmp_path / "library/src/formats/a_series/document.cpp"
    source.parent.mkdir(parents=True)
    source.write_text(
        '#include "axklib/media.hpp"\n'
        '#include "axklib/conversion/tx16w_to_a_series/converter.hpp"\n'
        '#include <nlohmann/json.hpp>\n',
        encoding="utf-8",
    )
    issues = architecture_check.check_repository(tmp_path, write_policy(tmp_path))
    assert sum(issue.code == "module_dependency_forbidden" for issue in issues) == 2


def test_shared_session_infrastructure_cannot_include_format_state(tmp_path: Path) -> None:
    write_lines(tmp_path / "src/unit.cpp", 1)
    source = tmp_path / "apps/application/src/image_sessions_internal.hpp"
    source.parent.mkdir(parents=True)
    source.write_text(
        '#include "native_document_state.hpp"\n'
        '#include "formats/a_series/session_state.hpp"\n'
        '#include "axklib/formats/tx16w/document.hpp"\n',
        encoding="utf-8",
    )
    issues = architecture_check.check_repository(tmp_path, write_policy(tmp_path))
    assert sum(issue.code == "module_dependency_forbidden" for issue in issues) == 2


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


def test_explicit_vendored_exclusion_may_be_absent(tmp_path: Path) -> None:
    policy = write_policy(
        tmp_path,
        exclusions=[
            {
                "path": "src/.venv",
                "classification": "vendored",
                "reason": "Local dependency environment",
                "mayBeAbsent": True,
            }
        ],
    )
    write_lines(tmp_path / "src/tool.py", 1)

    assert architecture_check.check_repository(tmp_path, policy) == []


def test_rejects_invalid_optional_exclusion_flag(tmp_path: Path) -> None:
    policy = write_policy(
        tmp_path,
        exclusions=[
            {
                "path": "src/generated",
                "classification": "generated",
                "reason": "Generated output",
                "mayBeAbsent": "yes",
            }
        ],
    )
    write_lines(tmp_path / "src/tool.py", 1)

    assert issue_codes(tmp_path, policy) == [
        "invalid_exclusion_presence",
        "exclusion_path_missing",
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
