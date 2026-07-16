from __future__ import annotations

import http.client
import json
import os
import socket
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


def startup_timeout() -> float:
    instrumented = any(
        os.environ.get(name)
        for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS")
    )
    return 20.0 if instrumented else 8.0


def choose_port() -> int:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def write_workspace_store(path: Path, workspace: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(
            {
                "schemaVersion": 1,
                "revision": 1,
                "workspaces": [
                    {
                        "id": "workspace",
                        "displayName": "Workspace",
                        "path": str(workspace.resolve()),
                        "writable": True,
                    }
                ],
            }
        ),
        encoding="utf-8",
    )


@dataclass(frozen=True)
class HttpResult:
    status: int
    content: bytes
    headers: dict[str, str]

    def json(self) -> Any:
        return json.loads(self.content) if self.content else None


def request(
    port: int,
    token: str | None,
    method: str,
    path: str,
    body: dict[str, Any] | bytes | str | None = None,
    headers: dict[str, str] | None = None,
    timeout: float = 5.0,
) -> HttpResult:
    request_headers = {} if headers is None else dict(headers)
    if token is not None:
        request_headers["Authorization"] = f"Bearer {token}"
    payload: bytes | str | None
    if isinstance(body, dict):
        payload = json.dumps(body)
        request_headers.setdefault("Content-Type", "application/json")
    else:
        payload = body
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    connection.request(method, path, body=payload, headers=request_headers)
    response = connection.getresponse()
    result = HttpResult(
        response.status,
        response.read(),
        {name.lower(): value for name, value in response.getheaders()},
    )
    connection.close()
    return result


def wait_until_ready(port: int, process: subprocess.Popen[bytes]) -> None:
    deadline = time.monotonic() + startup_timeout()
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(
                f"server exited during startup with {process.returncode}"
            )
        try:
            if (
                request(
                    port,
                    None,
                    "GET",
                    "/api/v1/system/health/live",
                    timeout=0.2,
                ).status
                == 204
            ):
                return
        except OSError:
            pass
        time.sleep(0.02)
    raise AssertionError("server did not become live")


def wait_for_connection_file(
    path: Path, process: subprocess.Popen[bytes]
) -> dict[str, Any]:
    deadline = time.monotonic() + startup_timeout()
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(
                f"server exited during startup with {process.returncode}"
            )
        if path.exists():
            return json.loads(path.read_text(encoding="utf-8"))
        time.sleep(0.02)
    raise AssertionError("server did not publish its connection file")


def wait_for_job(
    port: int,
    token: str,
    job_id: str,
    process: subprocess.Popen[bytes],
    timeout: float = 8.0,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(
                f"server exited while job {job_id} was active with {process.returncode}"
            )
        response = request(port, token, "GET", f"/api/v1/jobs/{job_id}")
        assert response.status == 200, response.content
        snapshot = response.json()["data"]
        if snapshot["state"] in {"COMPLETED", "FAILED", "CANCELLED"}:
            return snapshot
        time.sleep(0.01)
    raise AssertionError(f"job {job_id} did not become terminal")


def process_file_descriptors(pid: int) -> int | None:
    descriptor_directory = Path(f"/proc/{pid}/fd")
    if not descriptor_directory.is_dir():
        return None
    return len(list(descriptor_directory.iterdir()))


def process_resident_bytes(pid: int) -> int | None:
    status_path = Path(f"/proc/{pid}/status")
    if not status_path.is_file():
        return None
    for line in status_path.read_text(encoding="utf-8").splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1]) * 1024
    return None


class ServerProcess:
    def __init__(
        self,
        executable: Path,
        arguments: list[str],
        port: int,
        log_path: Path,
    ) -> None:
        self.executable = executable
        self.arguments = arguments
        self.port = port
        self.log_path = log_path
        self._log = None
        self.process: subprocess.Popen[bytes] | None = None

    def __enter__(self) -> ServerProcess:
        self._log = self.log_path.open("wb")
        self.process = subprocess.Popen(
            [str(self.executable), *self.arguments],
            stdout=self._log,
            stderr=subprocess.STDOUT,
        )
        try:
            wait_until_ready(self.port, self.process)
        except BaseException:
            self.stop()
            print(self.output(), file=sys.stderr)
            self._log.close()
            self._log = None
            raise
        return self

    def stop(self, timeout: float = 5.0) -> None:
        if self.process is None or self.process.poll() is not None:
            return
        self.process.terminate()
        try:
            self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=timeout)

    def __exit__(self, exception_type: object, *_: object) -> None:
        self.stop()
        if exception_type is not None:
            print(self.output(), file=sys.stderr)
        if self._log is not None:
            self._log.close()

    @property
    def pid(self) -> int:
        assert self.process is not None
        return self.process.pid

    def output(self) -> str:
        if self._log is not None:
            self._log.flush()
            os.fsync(self._log.fileno())
        return self.log_path.read_text(encoding="utf-8", errors="replace")
