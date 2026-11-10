from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]


@pytest.mark.parametrize(
    ("source_name", "dependent_headers"),
    [
        ("win32_helper_main.cpp", ("shellapi.h",)),
        ("win32_elevation.cpp", ("objbase.h", "shellapi.h")),
        ("win32_identity.cpp", ("cfgmgr32.h", "setupapi.h", "winioctl.h")),
        ("win32_pipe.cpp", ("bcrypt.h", "sddl.h")),
        ("ownership.cpp", ("shlobj.h",)),
    ],
)
def test_device_windows_sdk_headers_follow_windows_header(
    source_name: str, dependent_headers: tuple[str, ...]
) -> None:
    source = (ROOT / "devices" / "src" / source_name).read_text(encoding="utf-8")
    windows_position = source.index("#include <windows.h>")
    for header in dependent_headers:
        assert windows_position < source.index(f"#include <{header}>"), (
            f"{source_name}: windows.h must precede {header}"
        )


def test_device_host_does_not_export_private_pimpl_storage() -> None:
    header = (ROOT / "library/include/axklib/sdk/devices.hpp").read_text(encoding="utf-8")
    assert "class device_host final {" in header
    assert "class AXK_SDK_API device_host" not in header


@pytest.mark.parametrize("defined_value", [None, "1", ""])
def test_secure_random_preserves_windows_build_macro_and_sdk_order(
    tmp_path: Path, defined_value: str | None
) -> None:
    compiler = shutil.which("clang++-18")
    if compiler is None:
        pytest.skip("Clang 18 is required for the Windows preprocessor regression")
    includes = tmp_path / "include"
    application = includes / "axklib/application"
    application.mkdir(parents=True)
    (application / "secure_random.hpp").write_text("", encoding="utf-8")
    for header in ("array", "cerrno", "cstdint", "fstream", "limits", "span", "vector"):
        (includes / header).write_text("", encoding="utf-8")
    (includes / "windows.h").write_text(
        '#ifndef NOMINMAX\n#error NOMINMAX must precede windows.h\n#endif\n'
        '#define AXK_TEST_WINDOWS_HEADER 1\n'
        '#define AXK_TEST_STRINGIFY(value) #value\n'
        '#define AXK_TEST_EXPAND(value) AXK_TEST_STRINGIFY(value)\n'
        'const char *axk_test_nominmax = AXK_TEST_EXPAND(NOMINMAX);\n',
        encoding="utf-8",
    )
    (includes / "bcrypt.h").write_text(
        '#ifndef AXK_TEST_WINDOWS_HEADER\n#error windows.h must precede bcrypt.h\n#endif\n',
        encoding="utf-8",
    )
    command = [
        compiler, "-E", "-std=c++23", "-Werror", "-nostdinc", "-nostdinc++", "-D_WIN32", "-I", str(includes)
    ]
    if defined_value is not None:
        command.append(f"-DNOMINMAX={defined_value}")
    command.append(str(ROOT / "apps/application/src/secure_random.cpp"))
    result = subprocess.run(command, check=False, capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stdout + result.stderr
    assert f'axk_test_nominmax = "{defined_value or ""}"' in result.stdout
