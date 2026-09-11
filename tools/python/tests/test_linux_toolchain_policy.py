from __future__ import annotations

import json
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]


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
