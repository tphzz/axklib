#!/usr/bin/env python3
"""Check packaged device bindings without enumerating, opening or authorizing hardware."""

from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

from release_server_smoke import (
    ApiClient,
    Client,
    create_owner_only_directory,
    require_status,
    wait_for_connection,
)

REQUIRED_OPERATIONS = {
    "devices.xml.inspect", "devices.list", "devices.connect", "devices.status", "devices.close",
    "devices.authorize", "devices.operations", "devices.resolve", "devices.recover",
}


def inspect_metadata(client: ApiClient) -> dict[str, Any]:
    version = require_status(client.request("GET", "/system/version"), 200, "server version")
    identity = version["data"]["sourceIdentity"]
    if not isinstance(identity, str) or not identity:
        raise RuntimeError("packaged server did not report its source identity")
    capabilities = require_status(client.request("GET", "/system/capabilities"), 200, "capabilities")
    operations = capabilities["data"]["operations"]
    implemented = {entry["id"] for entry in operations if entry["implemented"] is True}
    missing = REQUIRED_OPERATIONS - implemented
    if missing:
        raise RuntimeError(f"packaged server lacks device bindings: {', '.join(sorted(missing))}")
    return {"sourceIdentity": identity, "operations": sorted(REQUIRED_OPERATIONS)}


def server_command(server: Path, architecture: str) -> list[str]:
    if architecture == "native":
        return [str(server)]
    if sys.platform != "darwin":
        raise ValueError("explicit architecture selection requires macOS")
    return ["/usr/bin/arch", f"-{architecture}", str(server)]


def sidecar_arguments(state: Path) -> list[str]:
    return [
        "--port", "0", "--enable-devices", "--state-directory", str(state),
        "--connection-file", str(state / "connection.json"), "--parent-pid", str(os.getpid()),
        "--workspace-store", str(state / "workspaces.json"),
        "--workers", "2", "--job-workers", "1", "--write-job-workers", "1",
    ]


def stop_server(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    if os.name == "posix":
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    elif process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        if os.name == "posix":
            os.killpg(process.pid, signal.SIGKILL)
        else:
            process.kill()
        process.wait(timeout=3)


def exercise(server: Path, architecture: str, output_directory: Path) -> None:
    command = server_command(server, architecture)
    output_directory.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    report: dict[str, Any] = {"server": str(server), "architecture": architecture, "outcome": "failed"}
    try:
        with tempfile.TemporaryDirectory(prefix="axklib-device-capabilities-") as temporary:
            state = Path(temporary) / "state"
            create_owner_only_directory(state)
            connection_file = state / "connection.json"
            command += sidecar_arguments(state)
            with (output_directory / "server.log").open("wb") as log:
                process = subprocess.Popen(
                    command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                    start_new_session=os.name == "posix",
                )
                report["serverPid"] = process.pid
                try:
                    connection = wait_for_connection(connection_file, process)
                    report["listenPort"] = urlsplit(str(connection["baseUrl"])).port
                    client = Client(str(connection["baseUrl"]), str(connection["bearerToken"]))
                    report.update(inspect_metadata(client))
                    shutdown = require_status(client.request("POST", "/system/shutdown"), 202, "shutdown")
                    if shutdown["data"]["accepted"] is not True:
                        raise RuntimeError("packaged server refused shutdown")
                    process.wait(timeout=5)
                    if process.returncode != 0 or connection_file.exists():
                        raise RuntimeError("packaged server did not shut down cleanly")
                    report["outcome"] = "passed"
                finally:
                    stop_server(process)
                    report["serverExitCode"] = process.returncode
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        report["elapsedMs"] = round((time.monotonic() - started) * 1000)
        (output_directory / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Device capability smoke passed ({architecture}): {report['sourceIdentity']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--architecture", choices=("native", "x86_64", "arm64"), default="native")
    parser.add_argument("--output-directory", type=Path, required=True)
    arguments = parser.parse_args()
    previous_handler = signal.signal(signal.SIGTERM, lambda _signal, _frame: sys.exit(128 + signal.SIGTERM))
    try:
        exercise(arguments.server.resolve(), arguments.architecture, arguments.output_directory.resolve())
    finally:
        signal.signal(signal.SIGTERM, previous_handler)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
