from __future__ import annotations

import ast
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
DATA = ROOT / "cpp" / "data" / "current-sbnk-fields.json"
GENERATED = ROOT / "cpp" / "include" / "axklib" / "generated" / "current_sbnk_fields.hpp"


def _python_decoder_fields() -> list[dict[str, object]]:
    source = ROOT / "oracle" / "python" / "axklib" / "parameters" / "current.py"
    tree = ast.parse(source.read_text(encoding="utf-8"))
    function = next(
        node
        for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and node.name == "decode_sbnk_member_parameter_window"
    )
    returned = next(node for node in ast.walk(function) if isinstance(node, ast.Return))
    assert isinstance(returned.value, ast.Call)
    fields: list[dict[str, object]] = []
    for keyword in returned.value.keywords:
        name = keyword.arg
        if name in {"sample_parameter_base_0x0a8", "sample_control_records"}:
            continue
        assert name is not None
        match = re.search(r"_0x([0-9a-fA-F]+)$", name)
        assert match is not None
        expression = keyword.value
        signed = False
        if (
            isinstance(expression, ast.Call)
            and isinstance(expression.func, ast.Name)
            and expression.func.id == "s8"
        ):
            signed = True
            expression = expression.args[0]
        assert isinstance(expression, ast.Call)
        assert isinstance(expression.func, ast.Name)
        width = {"raw_u8": 1, "be16": 2, "be32": 4}[expression.func.id]
        fields.append(
            {
                "name": name,
                "offset": int(match.group(1), 16),
                "signed": signed,
                "width": width,
            }
        )
    return fields


def test_current_sbnk_field_data_matches_python_decoder() -> None:
    value = json.loads(DATA.read_text(encoding="utf-8"))
    assert set(value) == {"schema_version", "sbnk_fields"}
    assert value["schema_version"] == "1.0"
    assert value["sbnk_fields"] == _python_decoder_fields()


def test_current_sbnk_generated_header_is_deterministic(tmp_path: Path) -> None:
    output = tmp_path / "current_sbnk_fields.hpp"
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "cpp" / "tools" / "generate_current_fields.py"),
            str(DATA),
            str(output),
        ],
        check=True,
    )
    assert output.read_bytes() == GENERATED.read_bytes()
