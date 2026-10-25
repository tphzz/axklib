from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pytest

ROOT = Path(__file__).resolve().parents[3]
HARNESS = ROOT / "devices/tests/run_macos_authorization_test.cmake"


@dataclass(frozen=True)
class AuthorizationFixture:
    cmake: str
    home: Path
    source: Path
    report: Path
    neighbor: Path
    emulator: str
    payload: bytes
    source_mode: int
    source_mtime_ns: int


@pytest.fixture
def authorization_fixture(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> AuthorizationFixture:
    cmake = shutil.which("cmake")
    assert cmake is not None, "CMake is required to test the native authorization harness"
    home = tmp_path / "native test home"
    home.mkdir()
    checkout = tmp_path / "external checkout"
    checkout.mkdir()
    source = checkout / "macos authorization fixture"
    payload = b"native authorization fixture\x00\xff\x01\n"
    source.write_bytes(payload)
    source.chmod(0o755)
    cache = home / "Library/Caches/axklib-authorization-tests"
    neighbor = cache / "unrelated-stage/keep"
    neighbor.parent.mkdir(parents=True)
    neighbor.write_text("not owned by this run", encoding="utf-8")
    report = tmp_path / "observed.json"
    inspector = tmp_path / "inspect_staged.py"
    inspector.write_text(
        "import hashlib, json, pathlib, stat, sys\n"
        "stage = pathlib.Path(sys.argv[1])\n"
        "report = pathlib.Path(sys.argv[2])\n"
        "report.write_text(json.dumps({\n"
        "    'path': str(stage),\n"
        "    'sha256': hashlib.sha256(stage.read_bytes()).hexdigest(),\n"
        "    'mode': stat.S_IMODE(stage.stat().st_mode),\n"
        "    'parent_mode': stat.S_IMODE(stage.parent.stat().st_mode),\n"
        "}), encoding='utf-8')\n",
        encoding="utf-8",
    )
    emulator_script = tmp_path / "fake emulator.cmake"
    emulator_script.write_text(
        "math(EXPR last \"${CMAKE_ARGC} - 1\")\n"
        "math(EXPR first_prefix \"${last} - 2\")\n"
        "math(EXPR second_prefix \"${last} - 1\")\n"
        "if(NOT \"${CMAKE_ARGV${first_prefix}}\" STREQUAL \"first prefix argument\"\n"
        "   OR NOT \"${CMAKE_ARGV${second_prefix}}\" STREQUAL \"second prefix argument\")\n"
        "  message(FATAL_ERROR \"emulator prefix arguments were not preserved\")\n"
        "endif()\n"
        "set(staged \"${CMAKE_ARGV${last}}\")\n"
        "execute_process(COMMAND \"$ENV{AXK_TEST_PYTHON}\" \"$ENV{AXK_TEST_INSPECTOR}\"\n"
        "  \"${staged}\" \"$ENV{AXK_TEST_REPORT}\" RESULT_VARIABLE inspected)\n"
        "if(NOT inspected EQUAL 0)\n"
        "  message(FATAL_ERROR \"staged-file inspection failed\")\n"
        "endif()\n"
        "if(\"$ENV{AXK_TEST_MODE}\" STREQUAL \"failure\")\n"
        "  message(FATAL_ERROR \"synthetic native failure\")\n"
        "elseif(\"$ENV{AXK_TEST_MODE}\" STREQUAL \"timeout\")\n"
        "  string(TIMESTAMP now \"%s\" UTC)\n"
        "  math(EXPR stop \"${now} + 5\")\n"
        "  while(now LESS stop)\n"
        "    string(TIMESTAMP now \"%s\" UTC)\n"
        "  endwhile()\n"
        "endif()\n",
        encoding="utf-8",
    )
    monkeypatch.setenv("HOME", str(home))
    monkeypatch.setenv("AXK_TEST_PYTHON", sys.executable)
    monkeypatch.setenv("AXK_TEST_INSPECTOR", str(inspector))
    monkeypatch.setenv("AXK_TEST_REPORT", str(report))
    for name in ("RUNNER_TEMP", "TMPDIR"):
        outside = tmp_path / name
        outside.mkdir()
        monkeypatch.setenv(name, str(outside))
    emulator = ";".join(
        [Path(cmake).as_posix(), "-P", emulator_script.as_posix(), "--", "first prefix argument", "second prefix argument"]
    )
    source_stat = source.stat()
    return AuthorizationFixture(
        cmake, home, source, report, neighbor, emulator, payload, source_stat.st_mode, source_stat.st_mtime_ns
    )


def run_harness(
    fixture: AuthorizationFixture, executable: Path | None = None
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            fixture.cmake,
            f"-DRAWDISK_AUTHORIZATION_TEST={(executable or fixture.source).as_posix()}",
            f"-DRAWDISK_AUTHORIZATION_EMULATOR={fixture.emulator}",
            "-DRAWDISK_AUTHORIZATION_TIMEOUT_SECONDS=1",
            "-P",
            str(HARNESS),
        ],
        text=True,
        cwd=fixture.source.parent,
        capture_output=True,
        check=False,
        timeout=10,
    )


def inspected(fixture: AuthorizationFixture) -> dict[str, Any]:
    value = json.loads(fixture.report.read_text(encoding="utf-8"))
    assert isinstance(value, dict)
    return value


def assert_owned_stage_cleaned(fixture: AuthorizationFixture, report: dict[str, Any]) -> None:
    staged = Path(report["path"])
    assert not staged.exists()
    assert not staged.parent.exists()
    assert fixture.neighbor.read_text(encoding="utf-8") == "not owned by this run"
    assert fixture.source.read_bytes() == fixture.payload
    assert fixture.source.stat().st_mode == fixture.source_mode
    assert fixture.source.stat().st_mtime_ns == fixture.source_mtime_ns
    assert sorted((fixture.home / "Library/Caches/axklib-authorization-tests").iterdir()) == [
        fixture.neighbor.parent
    ]


@pytest.mark.parametrize("mode", ["success", "failure", "timeout"])
def test_private_home_stage_preserves_source_and_cleans_every_outcome(
    authorization_fixture: AuthorizationFixture,
    monkeypatch: pytest.MonkeyPatch,
    mode: str,
) -> None:
    import hashlib

    fixture = authorization_fixture
    monkeypatch.setenv("AXK_TEST_MODE", mode)

    result = run_harness(fixture)

    assert (result.returncode == 0) == (mode == "success"), result.stdout + result.stderr
    report = inspected(fixture)
    staged = Path(report["path"])
    assert staged.name == fixture.source.name
    assert staged.parent.parent == fixture.home / "Library/Caches/axklib-authorization-tests"
    assert staged != fixture.source
    assert report["sha256"] == hashlib.sha256(fixture.payload).hexdigest()
    if os.name != "nt":
        assert report["parent_mode"] == 0o700
        assert report["mode"] == 0o700
    assert_owned_stage_cleaned(fixture, report)


def test_repeated_runs_use_distinct_stage_directories(
    authorization_fixture: AuthorizationFixture, monkeypatch: pytest.MonkeyPatch
) -> None:
    fixture = authorization_fixture
    monkeypatch.setenv("AXK_TEST_MODE", "success")
    first = run_harness(fixture)
    assert first.returncode == 0, first.stdout + first.stderr
    first_report = inspected(fixture)
    assert_owned_stage_cleaned(fixture, first_report)

    second = run_harness(fixture)

    assert second.returncode == 0, second.stdout + second.stderr
    second_report = inspected(fixture)
    assert Path(first_report["path"]).parent != Path(second_report["path"]).parent
    assert_owned_stage_cleaned(fixture, second_report)


@pytest.mark.parametrize("input_kind", ["relative", "missing", "directory"])
def test_invalid_input_cannot_execute_or_change_a_stage(
    authorization_fixture: AuthorizationFixture, input_kind: str
) -> None:
    fixture = authorization_fixture
    executable = {
        "relative": Path("relative-test"),
        "missing": fixture.source.parent / "missing-test",
        "directory": fixture.source.parent,
    }[input_kind]

    result = run_harness(fixture, executable)

    assert result.returncode != 0
    assert not fixture.report.exists()
    assert fixture.source.read_bytes() == fixture.payload
    assert sorted((fixture.home / "Library/Caches/axklib-authorization-tests").iterdir()) == [
        fixture.neighbor.parent
    ]


def test_native_registration_routes_the_real_test_through_the_harness_and_emulator() -> None:
    cmake = (ROOT / "devices/CMakeLists.txt").read_text(encoding="utf-8")
    registration = cmake.split("add_test(NAME macos_authorization_tests", 1)[1].split(")", 1)[0]
    assert "${CMAKE_COMMAND}" in registration
    assert "-DRAWDISK_AUTHORIZATION_TEST=$<TARGET_FILE:macos_authorization_tests>" in registration
    assert "-DRAWDISK_AUTHORIZATION_EMULATOR=" in registration
    assert "$<TARGET_PROPERTY:macos_authorization_tests,CROSSCOMPILING_EMULATOR>" in registration
    assert "run_macos_authorization_test.cmake" in registration


def test_native_registration_preserves_a_target_level_emulator_override(
    authorization_fixture: AuthorizationFixture, monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    fixture = authorization_fixture
    monkeypatch.setenv("AXK_TEST_MODE", "success")
    cmake = (ROOT / "devices/CMakeLists.txt").read_text(encoding="utf-8")
    registration = "add_test(NAME macos_authorization_tests" + cmake.split(
        "add_test(NAME macos_authorization_tests", 1
    )[1].split("set_tests_properties(macos_authorization_tests", 1)[0]
    project = tmp_path / "registration source"
    project.mkdir()
    cmake_file = project / "CMakeLists.txt"
    cmake_file.write_text(
        "cmake_minimum_required(VERSION 3.21)\n"
        "project(AuthorizationRunner NONE)\n"
        "enable_testing()\n"
        'set(CMAKE_CROSSCOMPILING_EMULATOR "wrong-global-emulator")\n'
        "add_executable(macos_authorization_tests IMPORTED)\n"
        "set_target_properties(macos_authorization_tests PROPERTIES\n"
        f'  IMPORTED_LOCATION "{fixture.source.as_posix()}"\n'
        f'  CROSSCOMPILING_EMULATOR "{fixture.emulator}")\n'
        + registration.replace("${CMAKE_CURRENT_SOURCE_DIR}/tests/", (ROOT / "devices/tests").as_posix() + "/"),
        encoding="utf-8",
    )
    build = tmp_path / "registration build"
    configured = subprocess.run(
        [fixture.cmake, "-S", str(project), "-B", str(build)],
        text=True,
        capture_output=True,
        check=False,
        timeout=10,
    )
    assert configured.returncode == 0, configured.stdout + configured.stderr
    ctest = shutil.which("ctest")
    assert ctest is not None

    result = subprocess.run(
        [ctest, "--test-dir", str(build), "-C", "Release", "--output-on-failure"],
        text=True,
        capture_output=True,
        check=False,
        timeout=10,
    )

    assert result.returncode == 0, result.stdout + result.stderr
    assert_owned_stage_cleaned(fixture, inspected(fixture))
