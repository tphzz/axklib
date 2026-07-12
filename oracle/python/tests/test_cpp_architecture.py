from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CPP_ROOT = ROOT / "cpp"


def test_cpp_core_has_no_adapter_or_optional_dependency_includes() -> None:
    banned = ("CLI/", "Python.h", "cxx.h", "tauri", "sndfile.h", "soxr.h")
    files = [
        *sorted((CPP_ROOT / "include").rglob("*.hpp")),
        *sorted((CPP_ROOT / "src").rglob("*.cpp")),
    ]
    assert files
    violations = [
        f"{path.relative_to(ROOT)}: {token}"
        for path in files
        for token in banned
        if token in path.read_text(encoding="utf-8")
    ]
    assert violations == []


def test_cpp_core_target_excludes_audio_import_and_cli_dependencies() -> None:
    cmake = (CPP_ROOT / "CMakeLists.txt").read_text("utf-8")
    core_sources = cmake.split("add_library(\n  axk_core", 1)[1].split(")", 1)[0]
    assert "audio_import" not in core_sources
    assert "writer_image" not in core_sources
    core_links = cmake.split("target_link_libraries(axk_core", 1)[1].split(")", 1)[0]
    assert "SndFile" not in core_links
    assert "SOXR" not in core_links
    assert "CLI11" not in core_links
    assert "add_library(\n  axk_audio" in cmake
    assert "add_executable(axk_cli ALIAS axklib_cli)" in cmake


def test_cpp_transition_ledger_is_sequential_with_one_active_stage() -> None:
    ledger = json.loads((ROOT / "oracle" / "transition-ledger.json").read_text("utf-8"))
    assert set(ledger) == {"schema_version", "stages"}
    assert ledger["schema_version"] == "1.0"
    stages = ledger["stages"]
    assert [stage["id"] for stage in stages] == list(range(1, 18))
    assert len([stage for stage in stages if stage["status"] == "active"]) == 1
    seen_blocked = False
    for stage in stages:
        assert set(stage) == {"id", "name", "status"}
        assert stage["status"] in {"blocked", "active", "parity", "complete"}
        if stage["status"] == "blocked":
            seen_blocked = True
        elif seen_blocked:
            raise AssertionError("non-blocked stage appears after a blocked dependency")
