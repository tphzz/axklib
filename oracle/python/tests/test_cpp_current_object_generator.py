from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Any, cast

from oracle.generate_current_object_fixture import generate
from oracle.python_object_semantic import semantic_value


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def test_current_object_fixture_generator_is_deterministic(tmp_path: Path) -> None:
    first = tmp_path / "first" / "current.hds"
    second = tmp_path / "second" / "current.hds"
    generate(first)
    generate(second)
    assert _sha256(first) == _sha256(second)
    assert semantic_value(first) == semantic_value(second)
    rows = cast(list[dict[str, Any]], semantic_value(first)["objects"])
    kinds = {row["decoded"]["kind"] for row in rows}
    assert kinds == {"SMPL", "SBNK", "SBAC", "PROG"}
