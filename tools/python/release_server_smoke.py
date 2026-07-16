#!/usr/bin/env python3
"""Smoke-test an installed axklib-server distribution."""

from __future__ import annotations

import argparse
import http.client
import json
import shutil
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit


def wait_for_connection(path: Path, process: subprocess.Popen[bytes]) -> dict[str, Any]:
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"axklib-server exited during startup with {process.returncode}")
        if path.is_file():
            document = json.loads(path.read_text(encoding="utf-8"))
            if not isinstance(document, dict):
                raise RuntimeError("connection file is not a JSON object")
            return document
        time.sleep(0.02)
    raise RuntimeError("axklib-server did not publish a connection file")


class Client:
    def __init__(self, base_url: str, token: str) -> None:
        parsed = urlsplit(base_url)
        if parsed.scheme != "http" or parsed.hostname != "127.0.0.1" or parsed.port is None:
            raise RuntimeError("installed smoke requires a loopback HTTP connection")
        if parsed.path != "/api/v1":
            raise RuntimeError("connection file contains an unsupported API base path")
        self._port = parsed.port
        self._token = token

    def request(self, method: str, path: str, body: dict[str, Any] | None = None) -> tuple[int, Any]:
        payload = None if body is None else json.dumps(body).encode("utf-8")
        headers = {"Authorization": f"Bearer {self._token}"}
        if payload is not None:
            headers["Content-Type"] = "application/json"
        connection = http.client.HTTPConnection("127.0.0.1", self._port, timeout=5)
        connection.request(method, f"/api/v1{path}", payload, headers)
        response = connection.getresponse()
        content = response.read()
        status = response.status
        connection.close()
        return status, json.loads(content) if content else None


def require_status(actual: tuple[int, Any], expected: int, context: str) -> Any:
    status, document = actual
    if status != expected:
        raise RuntimeError(f"{context} returned HTTP {status}: {document!r}")
    return document


def wait_for_job(client: Client, job_id: str) -> dict[str, Any]:
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        document = require_status(client.request("GET", f"/jobs/{job_id}"), 200, "job status")
        data = document["data"]
        if data["state"] in {"COMPLETED", "FAILED", "CANCELLED"}:
            if data["state"] != "COMPLETED":
                raise RuntimeError(f"fixture report did not complete: {data!r}")
            return data
        time.sleep(0.02)
    raise RuntimeError("fixture report did not reach a terminal state")


def exercise(server: Path, fixture: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="axklib-installed-server-") as temporary:
        workspace = Path(temporary)
        fixture_name = "fixture.hds"
        shutil.copyfile(fixture, workspace / fixture_name)
        state = workspace / "state"
        connection_file = state / "connection.json"
        process = subprocess.Popen(
            [
                str(server),
                "--port",
                "0",
                "--root",
                f"workspace={workspace}",
                "--state-directory",
                str(state),
                "--connection-file",
                str(connection_file),
                "--workers",
                "2",
                "--job-workers",
                "1",
                "--write-job-workers",
                "1",
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        clean_shutdown = False
        try:
            connection = wait_for_connection(connection_file, process)
            client = Client(str(connection["baseUrl"]), str(connection["bearerToken"]))
            version = require_status(client.request("GET", "/system/version"), 200, "version")
            if not version["data"]["semanticVersion"]:
                raise RuntimeError("installed server returned an empty semantic version")
            capabilities = require_status(client.request("GET", "/system/capabilities"), 200, "capabilities")
            operations = capabilities["data"]["operations"]
            if not operations or any(not operation["implemented"] for operation in operations):
                raise RuntimeError("installed server does not implement its complete advertised operation registry")
            submitted = require_status(
                client.request(
                    "POST",
                    "/reports/info",
                    {
                        "sources": [{"rootId": "workspace", "relativePath": fixture_name}],
                    },
                ),
                202,
                "fixture report submission",
            )
            result = wait_for_job(client, str(submitted["data"]["jobId"]))["result"]
            if result["loadedCount"] != 1 or result["failedCount"] != 0:
                raise RuntimeError(f"installed server fixture report was incomplete: {result!r}")
            if len(result["trees"]) != 1 or result["trees"][0]["sourcePath"] != fixture_name:
                raise RuntimeError(f"installed server fixture report returned the wrong source tree: {result!r}")
            shutdown = require_status(client.request("POST", "/system/shutdown"), 202, "sidecar shutdown")
            if shutdown["data"]["accepted"] is not True:
                raise RuntimeError("installed server did not accept clean shutdown")
            process.wait(timeout=5)
            if process.returncode != 0:
                raise RuntimeError(f"installed server exited with {process.returncode}")
            clean_shutdown = True
            if connection_file.exists():
                raise RuntimeError("installed server left its connection file after shutdown")
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
            if process.stdout is not None:
                output = process.stdout.read().decode("utf-8", errors="replace")
                process.stdout.close()
                if not clean_shutdown and output:
                    print(output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    arguments = parser.parse_args()
    exercise(arguments.server.resolve(), arguments.fixture.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
