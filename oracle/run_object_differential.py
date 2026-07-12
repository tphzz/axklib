"""Compare independent Python and C++ canonical current-object semantics."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from python_object_semantic import semantic_value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp-cli", type=Path, required=True)
    parser.add_argument("images", nargs="+", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    rows: list[dict[str, object]] = []
    failed = False
    for image in args.images:
        python_value = semantic_value(image)
        process = subprocess.run(
            [str(args.cpp_cli), "object-json", str(image)],
            check=False,
            capture_output=True,
            text=True,
        )
        cpp_value = json.loads(process.stdout) if process.returncode == 0 else None
        matches = process.returncode == 0 and cpp_value == python_value
        failed |= not matches
        rows.append(
            {
                "image": image.as_posix(),
                "cpp_exit_code": process.returncode,
                "matches": matches,
                "cpp_stderr": process.stderr.strip(),
            }
        )
    baseline = json.loads(
        (Path(__file__).resolve().parent / "baseline.json").read_text(encoding="utf-8")
    )
    cpp_version = subprocess.run(
        [str(args.cpp_cli), "--version"], check=True, capture_output=True, text=True
    ).stdout.strip()
    report = {
        "schema_version": "1.0",
        "operation": "current-object-read",
        "oracle": {
            "implementation": "python",
            "git_commit": baseline["oracle"]["git_commit"],
            "python_version": f"{sys.version_info.major}.{sys.version_info.minor}",
            "cpp_version": cpp_version,
        },
        "results": rows,
    }
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
