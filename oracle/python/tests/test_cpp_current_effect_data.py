from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
DATA = ROOT / "cpp" / "data" / "current-effects.json"
GENERATED = ROOT / "cpp" / "include" / "axklib" / "generated" / "current_effects.hpp"


def test_current_effect_data_matches_python_tables(tmp_path: Path) -> None:
    output = tmp_path / "current-effects.json"
    subprocess.run(
        [sys.executable, str(ROOT / "oracle" / "export_current_effects.py"), str(output)],
        check=True,
    )
    assert output.read_bytes() == DATA.read_bytes()


def test_current_effect_header_is_deterministic(tmp_path: Path) -> None:
    output = tmp_path / "current_effects.hpp"
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "cpp" / "tools" / "generate_current_effects.py"),
            str(DATA),
            str(output),
        ],
        check=True,
    )
    assert output.read_bytes() == GENERATED.read_bytes()
