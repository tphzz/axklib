from __future__ import annotations

import json
import os
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest
import yaml

ROOT = Path(__file__).resolve().parents[3]


def linux_preflight() -> dict[str, Any]:
    path = ROOT / ".github/workflows/native-platform.yml"
    workflow = yaml.load(path.read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
    assert isinstance(workflow, dict)
    matches = [
        step
        for step in workflow["jobs"]["native"]["steps"]
        if step.get("name") == "Verify preinstalled Linux C++23 toolchain"
    ]
    assert len(matches) == 1
    return dict(matches[0])


def test_linux_preflight_receives_the_requested_architecture() -> None:
    step = linux_preflight()
    assert step["if"] == "runner.os == 'Linux'"
    assert step["shell"] == "bash"
    assert step["env"]["AXK_LINUX_ARCHITECTURE"] == "${{ inputs.desktop_architecture }}"


@pytest.mark.skipif(sys.platform != "linux", reason="The Linux CI preflight executes only on Linux")
@pytest.mark.parametrize(
    ("architecture", "distribution", "codename", "error"),
    [
        ("x64", "ubuntu", "jammy", ""),
        ("arm64", "ubuntu", "noble", ""),
        ("x64", "ubuntu", "noble", "requires Ubuntu jammy"),
        ("arm64", "ubuntu", "jammy", "requires Ubuntu noble"),
        ("arm64", "debian", "noble", "requires Ubuntu noble"),
        ("x64", "debian", "jammy", "requires Ubuntu jammy"),
        ("arm64", "ubuntu", None, "requires Ubuntu noble"),
        ("x64", "ubuntu", None, "requires Ubuntu jammy"),
        ("riscv64", "ubuntu", "noble", "Unsupported Linux architecture: riscv64"),
    ],
)
def test_linux_preflight_enforces_architecture_specific_ubuntu_baselines(
    tmp_path: Path,
    architecture: str,
    distribution: str,
    codename: str | None,
    error: str,
) -> None:
    os_release = tmp_path / "os-release"
    os_release.write_text(
        f'ID={distribution}\nPRETTY_NAME="{distribution.title()} test"\n'
        + (f"VERSION_CODENAME={codename}\n" if codename is not None else ""),
        encoding="utf-8",
    )
    script = str(linux_preflight()["run"])
    assert script.count("source /etc/os-release") == 1
    script = script.replace("source /etc/os-release", f"source {shlex.quote(str(os_release))}")
    binaries = tmp_path / "bin"
    binaries.mkdir()
    for command in ("clang-18", "clang++-18", "ninja"):
        binary = binaries / command
        binary.write_text(
            '#!/bin/sh\nprintf \'%s %s\\n\' "${0##*/}" "$*" >> "$AXK_TOOLCHAIN_COMMAND_LOG"\n'
            'if [ "${1:-}" = "-std=c++23" ]; then cat > /dev/null; fi\n',
            encoding="utf-8",
        )
        binary.chmod(0o755)
    command_log = tmp_path / "commands.log"
    github_env = tmp_path / "github-env"
    environment = {
        key: value
        for key, value in os.environ.items()
        if key not in {"ID", "PRETTY_NAME", "VERSION_CODENAME"}
    }
    environment.update(
        AXK_LINUX_ARCHITECTURE=architecture,
        AXK_TOOLCHAIN_COMMAND_LOG=str(command_log),
        GITHUB_ENV=str(github_env),
        PATH=f"{binaries}{os.pathsep}{environment['PATH']}",
    )
    result = subprocess.run(
        ["bash", "-c", script],
        env=environment,
        capture_output=True,
        text=True,
        timeout=15,
        check=False,
    )
    if error:
        assert result.returncode != 0
        assert error in result.stderr
        if architecture in {"x64", "arm64"}:
            assert f"detected {distribution.title()} test ({codename or 'unknown'})" in result.stderr
        assert not command_log.exists()
        assert not github_env.exists()
    else:
        assert result.returncode == 0, result.stdout + result.stderr
        assert github_env.read_text(encoding="utf-8").splitlines() == [
            "CC=clang-18",
            "CXX=clang++-18",
            f"NINJA_EXE={binaries / 'ninja'}",
        ]
        assert command_log.read_text(encoding="utf-8").splitlines() == [
            "clang++-18 --version",
            "ninja --version",
            "clang++-18 -std=c++23 -stdlib=libc++ -x c++ -fsyntax-only -",
        ]


def test_local_release_and_linux_ci_share_clang18_selection() -> None:
    presets = json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
    for kind in ("configurePresets", "buildPresets", "testPresets"):
        names = {preset["name"] for preset in presets[kind]}
        assert "release" in names
        assert "release-clang19" not in names
    for architecture in ("x64", "arm64"):
        triplet = ROOT / f"library/cmake/triplets/{architecture}-linux-axk.cmake"
        assert "LinuxClang18Libcxx.cmake" in triplet.read_text(encoding="utf-8")
    workflow = (ROOT / ".github/workflows/native-platform.yml").read_text(encoding="utf-8")
    assert 'echo "CXX=clang++-18"' in workflow


@pytest.mark.parametrize(
    ("host", "processor", "triplet", "explicit", "expected"),
    [
        ("Linux", "x86_64", "", "", "default"),
        ("Linux", "aarch64", "", "", "default"),
        ("Linux", "x86_64", "x64-linux-axk", "/custom/toolchain.cmake", "custom"),
        ("Linux", "x86_64", "custom-linux", "", "none"),
        ("Windows", "AMD64", "", "", "none"),
    ],
)
def test_default_chainload_reaches_main_project(
    tmp_path: Path,
    host: str,
    processor: str,
    triplet: str,
    explicit: str,
    expected: str,
) -> None:
    result = tmp_path / "result.txt"
    script = tmp_path / "defaults.cmake"
    script.write_text(
        f'set(CMAKE_HOST_SYSTEM_NAME "{host}")\n'
        f'set(CMAKE_HOST_SYSTEM_PROCESSOR "{processor}")\n'
        f'include("{ROOT.as_posix()}/library/cmake/AxkVcpkgDefaults.cmake")\n'
        f'file(WRITE "{result.as_posix()}" "${{VCPKG_CHAINLOAD_TOOLCHAIN_FILE}}")\n',
        encoding="utf-8",
    )
    command = ["cmake"]
    if triplet:
        command.append(f"-DVCPKG_TARGET_TRIPLET={triplet}")
    if explicit:
        command.append(f"-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE={explicit}")
    subprocess.run(command + ["-P", str(script)], check=True, capture_output=True, timeout=30)
    actual = result.read_text(encoding="utf-8")
    if expected == "default":
        assert Path(actual) == ROOT / "library/cmake/toolchains/LinuxClang18Libcxx.cmake"
    else:
        assert actual == (explicit if expected == "custom" else "")
