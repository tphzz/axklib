#!/usr/bin/env python3
"""Run an extended read-only API opening matrix in memory/time-limited Linux workers."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path
from typing import Any

from image_open_smoke import load_manifest, result_code, run_case


def bounded_command(worker: list[str], *, wall_seconds: int = 120) -> list[str]:
    # 2 GiB images need mapping space plus parser/server headroom, not unbounded RAM.
    return [
        "prlimit",
        "--as=4294967296",
        "--core=0",
        "--",
        "timeout",
        "--signal=TERM",
        "--kill-after=3s",
        f"{wall_seconds}s",
        *worker,
    ]


def run_bounded(command: list[str], log: Path, *, wall_seconds: int = 120) -> dict[str, Any]:
    started = time.monotonic()
    with log.open("wb") as output:
        process = subprocess.Popen(
            bounded_command(command, wall_seconds=wall_seconds),
            stdin=subprocess.DEVNULL,
            stdout=output,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        # wait4 reports this worker tree's high-water RSS, not the suite's cumulative maximum.
        _, status, usage = os.wait4(process.pid, 0)
        process.returncode = os.waitstatus_to_exitcode(status)
    return {
        "peakRssKiB": usage.ru_maxrss,
        "elapsedSeconds": round(time.monotonic() - started, 3),
        "exitStatus": process.returncode,
    }


def summarize(rows: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "releaseReady": False,
        "exitCode": result_code(rows),
        "counts": dict(Counter(row["status"] for row in rows)),
        "results": rows,
    }


def write_json(path: Path, document: Any) -> None:
    path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


def worker_case(server: Path, source: Path, case: dict[str, Any], output: Path, index: int) -> None:
    row: dict[str, Any] = {
        "id": case["id"],
        "source": str(source),
        "sourceBytes": source.stat().st_size,
    }
    log = output / f"{index + 1:03d}-server.log"
    row["log"] = str(log)
    try:
        row.update(
            run_case(
                server,
                source,
                case["expected"],
                log,
                maximum_entries=250000,
                maximum_requests=8192,
                traversal_seconds=90,
                reject_invalid=False,
            ),
            status="passed",
        )
        row["openedAndEnumerated"] = True
        if not row["validation"]["valid"] or row["validation"]["errorCount"]:
            row.update(
                status="failed",
                stage="validation",
                reason="Opened and enumerated; object validation failed",
            )
    except (RuntimeError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        row.update(status="failed", reason=str(error))
    write_json(output / f"{index + 1:03d}-result.json", row)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--root", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--worker-index", type=int, help=argparse.SUPPRESS)
    args = parser.parse_args()
    cases = load_manifest(args.manifest, maximum_cases=256)
    roots: dict[str, Path] = {}
    for value in args.root:
        name, separator, path = value.partition("=")
        if not separator or not name or not path or name in roots:
            parser.error("each --root must be a unique NAME=PATH")
        roots[name] = Path(path).resolve()
    server = args.server.resolve(strict=True)
    output = args.output.resolve()
    if args.worker_index is not None:
        case = cases[args.worker_index]
        source = (roots[case["root"]] / case["path"]).resolve(strict=True)
        if not source.is_relative_to(roots[case["root"]]):
            parser.error("source escapes corpus root")
        worker_case(server, source, case, output, args.worker_index)
        return 0
    if sys.platform != "linux" or any(not shutil.which(tool) for tool in ("prlimit", "timeout")):
        parser.error("extended workers require Linux prlimit and timeout; limits are mandatory")
    output.mkdir(parents=True, exist_ok=False)
    with server.open("rb") as handle:
        server_hash = hashlib.file_digest(handle, "sha256").hexdigest()
    rows: list[dict[str, Any]] = [{"id": case["id"], "status": "not_run"} for case in cases]
    deadline = time.monotonic() + 1800
    metadata = {
        "server": str(server),
        "serverSha256": server_hash,
        "manifest": str(args.manifest.resolve()),
        "manifestSha256": hashlib.sha256(args.manifest.read_bytes()).hexdigest(),
        "limits": {
            "addressSpaceBytes": 4294967296,
            "caseWallSeconds": 120,
            "suiteWallSeconds": 1800,
            "traversalSeconds": 90,
            "maximumEntriesPerView": 250000,
            "maximumRequests": 8192,
        },
    }
    for index, case in enumerate(cases):
        started = time.monotonic()
        row: dict[str, Any] = {"id": case["id"], "status": "missing"}
        root = roots.get(case.get("root", ""))
        if started >= deadline:
            row.update(status="not_run", reason="suite wall limit exhausted")
        elif case.get("path") is None:
            row["reason"] = case["unavailableReason"]
        elif root is None:
            row["reason"] = f"missing --root {case['root']}"
        else:
            source = (root / case["path"]).resolve()
            row["source"] = str(source)
            if not source.is_relative_to(root):
                row.update(status="failed", reason="source escapes corpus root")
            elif not source.is_file():
                row["reason"] = "corpus file is missing"
            else:
                row["sourceBytes"] = source.stat().st_size
                usage = output / f"{index + 1:03d}-resources.json"
                driver_log = output / f"{index + 1:03d}-worker.log"
                command = [
                    sys.executable,
                    str(Path(__file__).resolve()),
                    "--server",
                    str(server),
                    "--manifest",
                    str(args.manifest.resolve()),
                    "--output",
                    str(output),
                    "--worker-index",
                    str(index),
                ]
                for name, path in roots.items():
                    command.extend(["--root", f"{name}={path}"])
                measured = run_bounded(
                    command, driver_log, wall_seconds=min(120, max(1, int(deadline - started)))
                )
                write_json(usage, measured)
                result = output / f"{index + 1:03d}-result.json"
                if result.is_file():
                    row = json.loads(result.read_text(encoding="utf-8"))
                if measured["exitStatus"] != 0 or not result.is_file():
                    row.update(
                        status="failed",
                        reason=f"bounded worker exited {measured['exitStatus']}; see worker log",
                    )
                row["workerLog"] = str(driver_log)
                if usage.is_file():
                    try:
                        row["resources"] = json.loads(usage.read_text(encoding="utf-8"))
                    except ValueError:
                        row.update(status="failed", reason="resource measurement was incomplete")
                else:
                    row.update(status="failed", reason="resource measurement is missing")
        row["category"] = case.get("category", "unspecified")
        if case.get("note"):
            row["note"] = case["note"]
        row["seconds"] = round(time.monotonic() - started, 3)
        rows[index] = row
        write_json(output / "summary.json", {**metadata, **summarize(rows)})
        print(
            f"{row['status'].upper()} {row['id']}: {row.get('reason', str(row['seconds']) + 's')}",
            flush=True,
        )
    return result_code(rows)


if __name__ == "__main__":
    raise SystemExit(main())
