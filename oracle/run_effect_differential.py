"""Compare every maintained effect parameter display transform with native C++."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path
from typing import Any

from axklib.effects.tables import format_effect_parameter_display


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp-dump", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()

    process = subprocess.Popen(
        [str(args.cpp_dump)], stdout=subprocess.PIPE, text=True
    )
    assert process.stdout is not None
    count = 0
    mismatches: list[dict[str, Any]] = []
    for line in process.stdout:
        native = dict(json.loads(line))
        expected = format_effect_parameter_display(
            int(native["raw_type"]),
            int(native["parameter_number"]),
            int(native["raw_value"]),
        )
        expected_value = {
            "value": expected.value,
            "table_index": expected.table_index,
            "quality": expected.quality,
            "source": expected.source,
        }
        native_value = {key: native[key] for key in expected_value}
        if native_value != expected_value and len(mismatches) < 50:
            mismatches.append(
                {
                    "raw_type": native["raw_type"],
                    "parameter_number": native["parameter_number"],
                    "raw_value": native["raw_value"],
                    "python": expected_value,
                    "cpp": native_value,
                }
            )
        count += 1
    return_code = process.wait()
    success = return_code == 0 and not mismatches
    report = {
        "schema_version": "1.0",
        "operation": "effect-display-transform",
        "case_count": count,
        "success": success,
        "mismatches": mismatches,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
