from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

import native_configure

STALE_EXTRACTION_MANIFEST = """\
Installing openssl:x64-windows-axk@3.6.3...
CMake Error at scripts/cmake/vcpkg_extract_archive.cmake:19 (message):

  downloads/tools/perl/5.42.2.1
  was an extraction target, but it already exists.

error: building openssl:x64-windows-axk failed with: BUILD_FAILED
"""


def test_configure_retries_once_after_stale_tool_extraction(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    runner_temp = tmp_path / "runner-temp"
    downloads = runner_temp / "vcpkg" / "downloads"
    tools = downloads / "tools"
    tools.mkdir(parents=True)
    (tools / "stale.txt").write_text("partial extraction", encoding="utf-8")
    archives = downloads.parent / "archives"
    archives.mkdir()
    (archives / "cached.zip").write_bytes(b"cache")
    manifest = tmp_path / "build" / "vcpkg-manifest-install.log"
    manifest.parent.mkdir()
    diagnostics = tmp_path / "diagnostics"
    summary = tmp_path / "summary.md"
    calls: list[list[str]] = []

    def fake_run(command: list[str], *, check: bool) -> subprocess.CompletedProcess[str]:
        calls.append(command)
        assert check is False
        if len(calls) == 1:
            manifest.write_text(STALE_EXTRACTION_MANIFEST, encoding="utf-8")
            return subprocess.CompletedProcess(command, 1)
        assert not tools.exists()
        return subprocess.CompletedProcess(command, 0)

    monkeypatch.setattr(native_configure.subprocess, "run", fake_run)

    report = native_configure.run_native_configure(
        ["cmake", "--preset", "release"],
        runner_temp=runner_temp,
        downloads_root=downloads,
        manifest_path=manifest,
        diagnostics_directory=diagnostics,
        triplet="x64-windows-axk",
        github_step_summary=summary,
    )

    assert report == native_configure.ConfigureReport(
        returncode=0, attempts=2, recovery_attempted=True
    )
    assert calls == [
        ["cmake", "--preset", "release"],
        ["cmake", "--preset", "release"],
    ]
    assert (archives / "cached.zip").read_bytes() == b"cache"
    assert (diagnostics / "vcpkg-manifest-attempt-1.log").read_text(
        encoding="utf-8"
    ) == STALE_EXTRACTION_MANIFEST
    summary_text = summary.read_text(encoding="utf-8")
    assert "Recovered stale vcpkg tool extraction state" in summary_text
    assert "openssl:x64-windows-axk" in summary_text


def test_configure_does_not_retry_an_unrelated_package_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    runner_temp = tmp_path / "runner-temp"
    downloads = runner_temp / "vcpkg" / "downloads"
    tools = downloads / "tools"
    tools.mkdir(parents=True)
    sentinel = tools / "keep.txt"
    sentinel.write_text("keep", encoding="utf-8")
    manifest = tmp_path / "build" / "vcpkg-manifest-install.log"
    manifest.parent.mkdir()
    manifest_text = """\
Installing opus:x64-windows-axk@1.5.2...
error: building opus:x64-windows-axk failed with: BUILD_FAILED
See https://learn.microsoft.com/vcpkg/troubleshoot/build-failures
"""
    calls = 0

    def fake_run(command: list[str], *, check: bool) -> subprocess.CompletedProcess[str]:
        nonlocal calls
        calls += 1
        manifest.write_text(manifest_text, encoding="utf-8")
        return subprocess.CompletedProcess(command, 1)

    monkeypatch.setattr(native_configure.subprocess, "run", fake_run)
    summary = tmp_path / "summary.md"

    report = native_configure.run_native_configure(
        ["cmake", "--preset", "release"],
        runner_temp=runner_temp,
        downloads_root=downloads,
        manifest_path=manifest,
        diagnostics_directory=tmp_path / "diagnostics",
        triplet="x64-windows-axk",
        github_step_summary=summary,
    )

    assert report == native_configure.ConfigureReport(
        returncode=1, attempts=1, recovery_attempted=False
    )
    assert calls == 1
    assert sentinel.read_text(encoding="utf-8") == "keep"
    summary_text = summary.read_text(encoding="utf-8")
    assert "Native dependency configuration failed" in summary_text
    assert "opus:x64-windows-axk" in summary_text


def test_configure_stops_after_a_second_stale_extraction_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    runner_temp = tmp_path / "runner-temp"
    downloads = runner_temp / "vcpkg" / "downloads"
    (downloads / "tools").mkdir(parents=True)
    manifest = tmp_path / "build" / "vcpkg-manifest-install.log"
    manifest.parent.mkdir()
    calls = 0

    def fake_run(command: list[str], *, check: bool) -> subprocess.CompletedProcess[str]:
        nonlocal calls
        calls += 1
        manifest.write_text(STALE_EXTRACTION_MANIFEST, encoding="utf-8")
        return subprocess.CompletedProcess(command, calls)

    monkeypatch.setattr(native_configure.subprocess, "run", fake_run)

    report = native_configure.run_native_configure(
        ["cmake", "--preset", "release"],
        runner_temp=runner_temp,
        downloads_root=downloads,
        manifest_path=manifest,
        diagnostics_directory=tmp_path / "diagnostics",
        triplet="x64-windows-axk",
        github_step_summary=tmp_path / "summary.md",
    )

    assert report == native_configure.ConfigureReport(
        returncode=2, attempts=2, recovery_attempted=True
    )
    assert calls == 2
    assert (
        tmp_path / "diagnostics" / "vcpkg-manifest-attempt-2.log"
    ).read_text(encoding="utf-8") == STALE_EXTRACTION_MANIFEST


def test_configure_rejects_downloads_outside_runner_temp(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    runner_temp = tmp_path / "runner-temp"
    runner_temp.mkdir()
    downloads = tmp_path / "outside" / "downloads"
    tools = downloads / "tools"
    tools.mkdir(parents=True)
    sentinel = tools / "keep.txt"
    sentinel.write_text("keep", encoding="utf-8")
    called = False

    def fake_run(command: list[str], *, check: bool) -> subprocess.CompletedProcess[str]:
        nonlocal called
        called = True
        return subprocess.CompletedProcess(command, 0)

    monkeypatch.setattr(native_configure.subprocess, "run", fake_run)

    with pytest.raises(ValueError, match="runner temporary directory"):
        native_configure.run_native_configure(
            ["cmake", "--preset", "release"],
            runner_temp=runner_temp,
            downloads_root=downloads,
            manifest_path=tmp_path / "manifest.log",
            diagnostics_directory=tmp_path / "diagnostics",
            triplet="x64-windows-axk",
            github_step_summary=None,
        )

    assert called is False
    assert sentinel.read_text(encoding="utf-8") == "keep"
