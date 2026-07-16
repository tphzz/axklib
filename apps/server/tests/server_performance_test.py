from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import statistics
import subprocess
import tempfile
import time
from pathlib import Path

from server_test_harness import (
    ServerProcess,
    choose_port,
    process_file_descriptors,
    process_resident_bytes,
    request,
    write_workspace_store,
)


def directory_bytes(path: Path) -> int:
    return sum(item.stat().st_size for item in path.rglob("*") if item.is_file())


def percentile(samples: list[float], percent: float) -> float:
    ordered = sorted(samples)
    index = min(len(ordered) - 1, int((len(ordered) - 1) * percent))
    return ordered[index]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--direct-benchmark", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()

    direct = json.loads(
        subprocess.check_output(
            [str(args.direct_benchmark), "2000"], text=True, timeout=10
        )
    )
    instrumented = any(
        os.environ.get(name)
        for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS")
    )

    with tempfile.TemporaryDirectory(prefix="axklib-server-performance-") as raw:
        root = Path(raw)
        workspace = root / "workspace"
        state = root / "state"
        workspace.mkdir()
        port = choose_port()
        token = "performance-token-0123456789abcdef"
        workspace_store = root / "workspaces.json"
        write_workspace_store(workspace_store, workspace)
        server_arguments = [
            "--port",
            str(port),
            "--token",
            token,
            "--state-directory",
            str(state),
            "--workspace-store",
            str(workspace_store),
            "--workers",
            "2",
            "--job-workers",
            "1",
            "--write-job-workers",
            "1",
        ]
        with ServerProcess(
            args.server, server_arguments, port, root / "server.log"
        ) as server:
            initial_rss = process_resident_bytes(server.pid)
            initial_descriptors = process_file_descriptors(server.pid)
            for _ in range(20):
                assert (
                    request(port, token, "GET", "/api/v1/system/version").status == 200
                )

            latencies_ms: list[float] = []
            for _ in range(300):
                started = time.perf_counter()
                response = request(port, token, "GET", "/api/v1/system/version")
                latencies_ms.append((time.perf_counter() - started) * 1000.0)
                assert response.status == 200
                assert response.json()["data"]["apiVersion"] == "v1"

            parallel_started = time.perf_counter()
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
                responses = list(
                    executor.map(
                        lambda _: request(port, token, "GET", "/api/v1/system/version"),
                        range(256),
                    )
                )
            parallel_seconds = time.perf_counter() - parallel_started
            assert all(response.status == 200 for response in responses)

            final_rss = process_resident_bytes(server.pid)
            final_descriptors = process_file_descriptors(server.pid)
            temporary_bytes = directory_bytes(state) + directory_bytes(workspace)
            version = responses[0].json()["data"]

        report = {
            "schemaVersion": "1.0",
            "profile": (
                "loopback-low-concurrency-instrumented"
                if instrumented
                else "loopback-low-concurrency"
            ),
            "operation": "system.version",
            "sourceIdentity": version["sourceIdentity"],
            "commands": [
                "axk_server_direct_benchmark 2000",
                "GET /api/v1/system/version x300 sequential",
                "GET /api/v1/system/version x256 at concurrency 8",
            ],
            "direct": direct,
            "loopbackRest": {
                "samples": len(latencies_ms),
                "meanMilliseconds": statistics.fmean(latencies_ms),
                "p50Milliseconds": percentile(latencies_ms, 0.50),
                "p95Milliseconds": percentile(latencies_ms, 0.95),
                "parallelRequestsPerSecond": 256.0 / parallel_seconds,
            },
            "resources": {
                "initialResidentBytes": initial_rss,
                "finalResidentBytes": final_rss,
                "residentGrowthBytes": (
                    None
                    if initial_rss is None or final_rss is None
                    else final_rss - initial_rss
                ),
                "initialFileDescriptors": initial_descriptors,
                "finalFileDescriptors": final_descriptors,
                "fileDescriptorGrowth": (
                    None
                    if initial_descriptors is None or final_descriptors is None
                    else final_descriptors - initial_descriptors
                ),
                "temporaryBytes": temporary_bytes,
            },
            "budgets": {
                "maximumP95Milliseconds": 250.0 if instrumented else 50.0,
                "minimumParallelRequestsPerSecond": 20.0 if instrumented else 100.0,
                "maximumResidentGrowthBytes": (
                    64 * 1024 * 1024 if instrumented else 16 * 1024 * 1024
                ),
                "maximumFileDescriptorGrowth": 8,
                "maximumTemporaryBytes": 64 * 1024,
            },
        }
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

        rest = report["loopbackRest"]
        resources = report["resources"]
        budgets = report["budgets"]
        assert rest["p95Milliseconds"] <= budgets["maximumP95Milliseconds"]
        assert (
            rest["parallelRequestsPerSecond"]
            >= budgets["minimumParallelRequestsPerSecond"]
        )
        if resources["residentGrowthBytes"] is not None:
            assert (
                resources["residentGrowthBytes"]
                <= budgets["maximumResidentGrowthBytes"]
            )
        if resources["fileDescriptorGrowth"] is not None:
            assert (
                resources["fileDescriptorGrowth"]
                <= budgets["maximumFileDescriptorGrowth"]
            )
        assert resources["temporaryBytes"] <= budgets["maximumTemporaryBytes"]
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
