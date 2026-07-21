from __future__ import annotations

import json
from pathlib import Path

import pytest

import native_build_cache


def entry(object_id: str, mode: str = "100644") -> native_build_cache.IndexEntry:
    return native_build_cache.IndexEntry(mode, object_id)


def test_prepare_reuses_unchanged_inputs_and_marks_changed_inputs_newer(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    source = tmp_path / "source"
    build = tmp_path / "build"
    source.mkdir()
    build.mkdir()
    unchanged = source / "unchanged.cpp"
    changed = source / "changed.cpp"
    added = source / "added.hpp"
    for path in (unchanged, changed, added):
        path.write_text(path.name, encoding="utf-8")
    (build / "cached.o").write_bytes(b"cached")
    native_build_cache.write_state(
        build / native_build_cache.STATE_FILENAME,
        {
            "deleted.cpp": entry("deleted"),
            "changed.cpp": entry("old"),
            "unchanged.cpp": entry("same"),
        },
    )
    current = {
        "added.hpp": entry("added"),
        "changed.cpp": entry("new"),
        "unchanged.cpp": entry("same"),
    }
    monkeypatch.setattr(native_build_cache, "read_git_index", lambda _: current)

    report = native_build_cache.prepare_build_cache(source, build)

    assert report == native_build_cache.PreparationReport(True, 1, 3)
    assert unchanged.stat().st_mtime_ns == native_build_cache.NORMALIZED_MTIME_NS
    assert changed.stat().st_mtime_ns > native_build_cache.NORMALIZED_MTIME_NS
    assert added.stat().st_mtime_ns > native_build_cache.NORMALIZED_MTIME_NS
    assert (build / "cached.o").read_bytes() == b"cached"
    assert native_build_cache.read_state(build / native_build_cache.STATE_FILENAME) == current


@pytest.mark.parametrize("state_contents", [None, "not-json", '{"schema_version": 999}'])
def test_prepare_discards_build_tree_without_valid_state(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    state_contents: str | None,
) -> None:
    source = tmp_path / "source"
    build = tmp_path / "build"
    source.mkdir()
    build.mkdir()
    tracked = source / "tracked.cpp"
    tracked.write_text("source", encoding="utf-8")
    sentinel = build / "stale.o"
    sentinel.write_bytes(b"stale")
    if state_contents is not None:
        (build / native_build_cache.STATE_FILENAME).write_text(state_contents, encoding="utf-8")
    current = {"tracked.cpp": entry("current")}
    monkeypatch.setattr(native_build_cache, "read_git_index", lambda _: current)
    original_mtime = tracked.stat().st_mtime_ns

    report = native_build_cache.prepare_build_cache(source, build)

    assert report == native_build_cache.PreparationReport(False, 0, 1)
    assert not sentinel.exists()
    assert tracked.stat().st_mtime_ns == original_mtime
    assert native_build_cache.read_state(build / native_build_cache.STATE_FILENAME) == current


def test_read_state_rejects_malformed_entries(tmp_path: Path) -> None:
    state = tmp_path / "state.json"
    state.write_text(
        json.dumps({"schema_version": 1, "entries": {"source.cpp": {"mode": 100644}}}),
        encoding="utf-8",
    )
    assert native_build_cache.read_state(state) is None


def test_toolchain_fingerprint_is_stable_and_covers_runner_image(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(
        native_build_cache,
        "_command_identity",
        lambda command: {"command": list(command), "output": "stable"},
    )
    monkeypatch.setattr(native_build_cache.platform, "platform", lambda: "test-platform")
    monkeypatch.setattr(native_build_cache.platform, "machine", lambda: "test-machine")
    monkeypatch.setattr(native_build_cache.platform, "system", lambda: "Linux")
    first_environment = {"CXX": "c++", "ImageVersion": "20260701.1"}
    second_environment = {"CXX": "c++", "ImageVersion": "20260708.1"}
    moved_environment = {
        "CXX": "c++",
        "GITHUB_WORKSPACE": "/different/workspace",
        "ImageVersion": "20260701.1",
    }

    first = native_build_cache.toolchain_fingerprint("x64-linux-axk", first_environment)

    assert first == native_build_cache.toolchain_fingerprint(
        "x64-linux-axk", first_environment
    )
    assert first != native_build_cache.toolchain_fingerprint(
        "x64-linux-axk", second_environment
    )
    assert first != native_build_cache.toolchain_fingerprint(
        "arm64-linux-axk", first_environment
    )
    assert first != native_build_cache.toolchain_fingerprint(
        "x64-linux-axk", moved_environment
    )


def test_append_github_output_appends_values(tmp_path: Path) -> None:
    output = tmp_path / "github-output"
    output.write_text("existing=value\n", encoding="utf-8")

    native_build_cache.append_github_output(output, {"cache": "ready", "count": "2"})

    assert output.read_text(encoding="utf-8") == "existing=value\ncache=ready\ncount=2\n"
