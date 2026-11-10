from __future__ import annotations

import json
import os
import re
import shutil
import signal
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]


def run_linux_command(command: list[str], timeout: int) -> subprocess.CompletedProcess[str]:
    process = subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True
    )
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except BaseException:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.communicate(timeout=3)
        raise
    assert process.returncode is not None
    return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)


@pytest.mark.parametrize(
    "entry_point", ["CMakeLists.txt", "library/CMakeLists.txt", "devices/CMakeLists.txt"]
)
def test_header_based_builds_disable_module_scanning_before_creating_targets(entry_point: str) -> None:
    source = (ROOT / entry_point).read_text(encoding="utf-8")
    setting = re.search(r"^set\(CMAKE_CXX_SCAN_FOR_MODULES OFF\)$", source, re.MULTILINE)
    first_target = re.search(r"^\s*add_(?:subdirectory|library|executable)\(", source, re.MULTILINE)
    assert setting is not None, f"{entry_point}: module scanning must be disabled without a cache override"
    assert first_target is not None
    assert setting.start() < first_target.start()


@pytest.mark.skipif(sys.platform != "linux", reason="This regression exercises the Linux Clang 18 toolchain")
def test_standalone_devices_build_without_a_module_scanner(tmp_path: Path) -> None:
    compiler = shutil.which("clang++-18")
    c_compiler = shutil.which("clang-18")
    if compiler is None or c_compiler is None or shutil.which("ninja") is None:
        pytest.skip("Clang 18 and Ninja are required for the missing-scanner build")
    capabilities = run_linux_command(["cmake", "-E", "capabilities"], 15)
    assert capabilities.returncode == 0, capabilities.stdout + capabilities.stderr
    version = json.loads(capabilities.stdout)["version"]
    if (version["major"], version["minor"]) < (3, 28):
        pytest.skip("Automatic C++ module scanning was introduced in CMake 3.28")

    build_dir = tmp_path / "build"
    missing_scanner = tmp_path / "missing-clang-scan-deps"
    configured = run_linux_command(
        [
            "cmake",
            "-S", str(ROOT / "devices"),
            "-B", str(build_dir),
            "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_C_COMPILER={c_compiler}",
            f"-DCMAKE_CXX_COMPILER={compiler}",
            "-DCMAKE_CXX_FLAGS=-stdlib=libc++",
            "-DCMAKE_POLICY_DEFAULT_CMP0155=NEW",
            f"-DCMAKE_CXX_COMPILER_CLANG_SCAN_DEPS={missing_scanner}",
            "-DRAWDISK_BUILD_TOOLS=OFF",
            "-DRAWDISK_BUILD_TESTS=OFF",
            "-DRAWDISK_WERROR=ON",
        ],
        60,
    )
    assert configured.returncode == 0, configured.stdout + configured.stderr
    built = run_linux_command(
        ["cmake", "--build", str(build_dir), "--parallel", "2", "--target", "rawdisk"],
        180,
    )
    assert built.returncode == 0, built.stdout + built.stderr
    dependencies = run_linux_command(
        ["ninja", "-C", str(build_dir), "-t", "deps", "CMakeFiles/rawdisk.dir/src/errors.cpp.o"],
        15,
    )
    assert dependencies.returncode == 0, dependencies.stdout + dependencies.stderr
    assert str(ROOT / "devices/include/rawdisk/errors.hpp") in dependencies.stdout
