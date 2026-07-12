"""Export maintained Python effect tables to the versioned language-neutral dataset."""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict
from pathlib import Path

from axklib.effects import tables


def value() -> dict[str, object]:
    return {
        "schema_version": "1.0",
        "effect_types": [asdict(row) for row in tables.EFFECT_TYPE_ROWS],
        "effect_parameters": [asdict(row) for row in tables.EFFECT_PARAMETER_ROWS],
        "enum_value_tables": [
            {"name": name, "values": list(values)}
            for name, values in sorted(tables.ENUM_VALUE_TABLES.items())
        ],
        "known_display_values": [
            {
                "raw_type": key[0],
                "parameter_number": key[1],
                "raw_value": key[2],
                "display": display,
            }
            for key, display in sorted(tables.KNOWN_DISPLAY_VALUES_00027.items())
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(value(), indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
