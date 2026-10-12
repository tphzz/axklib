from __future__ import annotations

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
