#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--warn-percent", type=float, default=15.0)
    parser.add_argument("--fail-percent", type=float, default=30.0)
    args = parser.parse_args()
    baseline_document = json.loads(args.baseline.read_text())
    candidate_document = json.loads(args.candidate.read_text())
    baseline = {row["name"]: row["wall_ms"] for row in baseline_document["metrics"]}
    candidate = {row["name"]: row["wall_ms"] for row in candidate_document["metrics"]}
    failures: list[str] = []
    maximum_peak = baseline_document.get("maximum_peak_memory_bytes")
    peak = candidate_document.get("peak_memory_bytes")
    if not isinstance(maximum_peak, int) or not isinstance(peak, int):
        failures.append("peak_memory_bytes: missing candidate metric or baseline budget")
    elif peak > maximum_peak:
        failures.append(f"peak_memory_bytes: {peak} exceeds {maximum_peak}")
        print(f"FAIL peak_memory_bytes: {peak} bytes (budget {maximum_peak})")
    else:
        print(f"OK peak_memory_bytes: {peak} bytes (budget {maximum_peak})")
    for name, expected in baseline.items():
        if name not in candidate or expected <= 0:
            failures.append(f"{name}: missing metric or invalid baseline")
            continue
        change = ((candidate[name] / expected) - 1.0) * 100.0
        level = "FAIL" if change > args.fail_percent else "WARN" if change > args.warn_percent else "OK"
        print(f"{level} {name}: {candidate[name]:.3f} ms ({change:+.1f}%)")
        if level == "FAIL":
            failures.append(name)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
