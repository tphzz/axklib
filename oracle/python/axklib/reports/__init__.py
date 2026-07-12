"""Shared report writing helpers for axklib tools."""

from __future__ import annotations

import csv
import json
from collections.abc import Sequence
from dataclasses import asdict, fields, is_dataclass
from enum import Enum
from pathlib import Path
from typing import Any, cast


def to_plain(value: Any) -> Any:
    if isinstance(value, Enum):
        return value.value
    if is_dataclass(value) and not isinstance(value, type):
        return {key: to_plain(item) for key, item in asdict(cast(Any, value)).items()}
    if isinstance(value, dict):
        return {str(key): to_plain(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [to_plain(item) for item in value]
    if isinstance(value, bytes):
        return value.hex()
    return value


def row_to_dict(row: object) -> dict[str, object]:
    if is_dataclass(row):
        return cast(dict[str, object], to_plain(row))
    if isinstance(row, dict):
        return cast(dict[str, object], to_plain(row))
    raise TypeError(f"unsupported report row type: {type(row)!r}")


def write_json(path: Path, rows: object) -> None:
    path.write_text(json.dumps(to_plain(rows), indent=2) + "\n", encoding="utf-8")


def write_rows_json(path: Path, rows: Sequence[object]) -> None:
    write_json(path, [row_to_dict(row) for row in rows])


def write_csv(path: Path, rows: Sequence[object], row_type: type[object] | None = None) -> None:
    if row_type is not None:
        fieldnames = [field.name for field in fields(cast(Any, row_type))]
    elif rows:
        first = row_to_dict(rows[0])
        fieldnames = list(first.keys())
    else:
        path.write_text("", encoding="utf-8")
        return
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row_to_dict(row))


def write_dict_csv(path: Path, rows: list[dict[str, object]]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fieldnames = sorted({key for row in rows for key in row})
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows([to_plain(row) for row in rows])
