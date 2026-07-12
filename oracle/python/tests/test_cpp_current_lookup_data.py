from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

from axklib.objects import current as object_current
from axklib.parameters import current as parameter_current
from axklib.parameters import sbnk_contract

ROOT = Path(__file__).resolve().parents[3]
DATA = ROOT / "cpp" / "data" / "current-lookups.json"
GENERATED = ROOT / "cpp" / "include" / "axklib" / "generated" / "current_lookups.hpp"

SOURCES = {
    **{
        name.lower(): getattr(sbnk_contract, name)
        for name in (
            "SAMPLE_EQ_FREQUENCY_UI_LABELS",
            "SAMPLE_EQ_TYPE_UI_LABELS",
            "SAMPLE_CONTROL_FUNCTION_UI_LABELS",
            "SAMPLE_CONTROL_TYPE_UI_LABELS",
            "SAMPLE_CONTROL_DEVICE_UI_LABELS",
            "FILTER_TYPE_UI_LABELS",
            "OUTPUT1_DESTINATION_UI_LABELS",
            "OUTPUT2_DESTINATION_UI_LABELS",
            "SAMPLE_PORTAMENTO_TYPE_UI_LABELS",
            "PITCH_BEND_TYPE_UI_LABELS",
        )
    },
    **{
        name.lower(): getattr(parameter_current, name)
        for name in (
            "PROG_LFO_CYCLE_LABELS",
            "PROG_LFO_WAVE_LABELS",
            "PROG_LFO_INITIAL_PHASE_LABELS",
            "PROG_LFO_RESET_CHANNEL_LABELS",
            "PROG_PORTAMENTO_TYPE_LABELS",
            "PROG_CONTROL_TYPE_LABELS",
            "PROG_SLOT_KIND_TARGET_CATEGORY",
        )
    },
    "current_smpl_loop_mode_labels": object_current.CURRENT_SMPL_LOOP_MODE_LABELS,
}


def _rows(value: object) -> list[dict[str, object]]:
    if isinstance(value, dict):
        items = sorted(value.items())
    elif isinstance(value, (tuple, list)):
        items = list(enumerate(value))
    else:
        raise TypeError(type(value))
    return [{"key": int(key), "label": str(label)} for key, label in items]


def test_current_lookup_data_matches_python_tables() -> None:
    value = json.loads(DATA.read_text(encoding="utf-8"))
    assert set(value) == {"schema_version", "tables"}
    assert value["schema_version"] == "1.0"
    assert value["tables"] == [
        {"name": name, "values": _rows(source)} for name, source in SOURCES.items()
    ]


def test_current_lookup_header_is_deterministic(tmp_path: Path) -> None:
    output = tmp_path / "current_lookups.hpp"
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "cpp" / "tools" / "generate_current_lookups.py"),
            str(DATA),
            str(output),
        ],
        check=True,
    )
    assert output.read_bytes() == GENERATED.read_bytes()
