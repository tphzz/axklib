from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]


@pytest.mark.parametrize("compiler", ["Clang", "AppleClang", "GNU", "MSVC"])
@pytest.mark.parametrize("warnings_as_errors", [True, False])
def test_project_warning_policy(
    tmp_path: Path, compiler: str, warnings_as_errors: bool
) -> None:
    result = tmp_path / "options.txt"
    script = tmp_path / "warnings.cmake"
    script.write_text(
        f'set(CMAKE_CXX_COMPILER_ID "{compiler}")\n'
        f'set(MSVC {"ON" if compiler == "MSVC" else "OFF"})\n'
        f'set(AXK_WARNINGS_AS_ERRORS {"ON" if warnings_as_errors else "OFF"})\n'
        'function(target_compile_options target scope)\n'
        '  if(NOT target STREQUAL "project_target" OR NOT scope STREQUAL "PRIVATE")\n'
        '    message(FATAL_ERROR "Warnings must remain private to the project target")\n'
        '  endif()\n'
        f'  file(APPEND "{result.as_posix()}" "${{ARGN}};")\n'
        'endfunction()\n'
        f'include("{ROOT.as_posix()}/library/cmake/AxkWarnings.cmake")\n'
        'axk_set_project_warnings(project_target)\n',
        encoding="utf-8",
    )
    subprocess.run(
        ["cmake", "-P", str(script)], check=True, capture_output=True, timeout=30
    )
    options = set(result.read_text(encoding="utf-8").split(";")) - {""}
    if compiler == "MSVC":
        expected = {"/W4", "/permissive-", "/Zc:__cplusplus"}
        if warnings_as_errors:
            expected.add("/WX")
    else:
        expected = {"-Wall", "-Wextra", "-Wpedantic", "-Wconversion", "-Wsign-conversion"}
        if compiler in {"Clang", "AppleClang"}:
            expected.update({"-Wshadow", "-Wshadow-uncaptured-local"})
        if warnings_as_errors:
            expected.add("-Werror")
    assert options == expected
