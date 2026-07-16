from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import socket
import tempfile
import time
from pathlib import Path
from urllib.parse import urlencode

from server_test_harness import (
    ServerProcess,
    choose_port,
    process_file_descriptors,
    process_resident_bytes,
    request,
    wait_for_connection_file,
    wait_for_job,
    write_workspace_store,
)


TOKEN_A = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
TOKEN_B = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"


def token_hash(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


def assert_hidden_reference(port: int, path: str) -> None:
    hidden = request(port, TOKEN_B, "GET", path)
    unknown = request(port, TOKEN_B, "GET", path.rsplit("/", 1)[0] + "/unknown")
    assert hidden.status == unknown.status == 404, (hidden, unknown)
    assert hidden.json()["error"]["code"] == unknown.json()["error"]["code"]


def create_upload(port: int, token: str, size: int = 12) -> str:
    response = request(
        port,
        token,
        "POST",
        "/api/v1/uploads",
        {
            "filename": "input.json",
            "kind": "manifest",
            "mediaType": "application/json",
            "size": size,
        },
    )
    assert response.status == 201, response.content
    return str(response.json()["data"]["uploadId"])


def exercise_constrained_server(server: Path, fixture: Path, root: Path) -> None:
    instrumented = any(
        os.environ.get(name)
        for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS")
    )
    maximum_rss_growth = (64 if instrumented else 32) * 1024 * 1024
    workspace = root / "workspace"
    state = root / "state"
    workspace.mkdir()
    state.mkdir()
    shutil.copyfile(fixture, workspace / "fixture.hds")
    (workspace / "durable-output.bin").write_bytes(b"published")
    sparse = workspace / "sparse.bin"
    with sparse.open("wb") as output:
        output.truncate(64 * 1024 * 1024)
    with sparse.open("r+b") as output:
        output.seek(sparse.stat().st_size - 4)
        output.write(b"tail")

    port = choose_port()
    config = {
        "bindAddress": "127.0.0.1",
        "port": port,
        "tokenHashes": [
            {"principalId": "owner-a", "sha256": token_hash(TOKEN_A)},
            {"principalId": "owner-b", "sha256": token_hash(TOKEN_B)},
        ],
        "workspaceStore": str(root / "workspaces.json"),
        "stateDirectory": str(state),
        "workerThreads": 2,
        "jobWorkerThreads": 1,
        "writeJobWorkerThreads": 1,
        "maximumQueuedJobs": 1,
        "maximumRetainedJobs": 32,
        "jobRetentionSeconds": 30,
        "replayEventsPerJob": 4,
        "maximumEventTickets": 1,
        "eventTicketTtlSeconds": 5,
        "maximumJsonBytes": 512,
        "maximumJsonDepth": 6,
        "maximumJsonNodes": 32,
        "maximumJsonContainerItems": 16,
        "maximumJsonStringBytes": 128,
        "streamThresholdBytes": 1024,
        "maximumWebsocketPayloadBytes": 64,
        "maximumWebsocketDeliveryEvents": 4,
        "maximumWebsocketDeliveryBytes": 1024,
        "maximumUploadBytes": 16,
        "maximumUploadTotalBytes": 20,
        "maximumUploads": 2,
        "maximumUploadChunkBytes": 4,
        "maximumDownloadRangeBytes": 4,
        "maximumDownloadArchiveBytes": 32,
        "maximumDownloadArchiveTotalBytes": 32,
        "maximumDownloadArchiveEntries": 2,
        "downloadArchiveRetentionSeconds": 5,
        "uploadRetentionSeconds": 5,
        "maximumImageSessions": 1,
        "maximumPageSize": 2,
        "imageIdleSeconds": 5,
    }
    config_path = root / "server.json"
    write_workspace_store(root / "workspaces.json", workspace)
    config_path.write_text(json.dumps(config), encoding="utf-8")

    with ServerProcess(
        server, ["--config", str(config_path)], port, root / "server.log"
    ) as running:
        assert running.process is not None
        baseline_fds = process_file_descriptors(running.pid)
        baseline_rss = process_resident_bytes(running.pid)

        missing = request(port, None, "GET", "/api/v1/roots")
        invalid = request(port, "not-a-token", "GET", "/api/v1/roots")
        assert missing.status == invalid.status == 401
        assert missing.json()["error"]["code"] == "authentication_required"
        assert invalid.json()["error"]["code"] == "authentication_required"

        capabilities = request(port, TOKEN_A, "GET", "/api/v1/system/capabilities")
        assert capabilities.status == 200, capabilities.content
        limits = capabilities.json()["data"]["limits"]
        assert limits["maximumJsonBytes"] == 512
        assert limits["maximumUploadTotalBytes"] == 20
        assert limits["maximumImageSessions"] == 1

        malformed_cases = [
            b"{",
            b"[]",
            json.dumps({"value": "x" * 129}),
            json.dumps({"a": {"b": {"c": {"d": {"e": {"f": {"g": 1}}}}}}}),
            b"{" + b'"padding":"' + b"x" * 520 + b'"}',
        ]
        for payload in malformed_cases:
            rejected = request(
                port,
                TOKEN_A,
                "POST",
                "/api/v1/files/metadata",
                payload,
                {"Content-Type": "application/json"},
            )
            assert rejected.status in {400, 413}, (payload[:40], rejected)
            assert rejected.json()["error"]["code"] in {
                "invalid_json",
                "json_structure_too_large",
                "request_too_large",
            }

        traversal = request(
            port,
            TOKEN_A,
            "POST",
            "/api/v1/files/metadata",
            {"rootId": "workspace", "relativePath": "../sparse.bin"},
        )
        assert traversal.status == 422, traversal.content
        assert traversal.json()["error"]["code"] == "invalid_file_reference"

        upload_id = create_upload(port, TOKEN_A)
        assert_hidden_reference(port, f"/api/v1/uploads/{upload_id}")
        quota = request(
            port,
            TOKEN_B,
            "POST",
            "/api/v1/uploads",
            {
                "filename": "other.json",
                "kind": "manifest",
                "mediaType": "application/json",
                "size": 12,
            },
        )
        assert quota.status == 429, quota.content
        assert quota.headers["retry-after"] == "1"
        assert quota.json()["error"]["retryable"] is True

        oversized_chunk = request(
            port,
            TOKEN_A,
            "PUT",
            f"/api/v1/uploads/{upload_id}",
            b"12345",
            {"Content-Type": "application/octet-stream", "Upload-Offset": "0"},
        )
        assert oversized_chunk.status == 413, oversized_chunk.content
        for offset, content in ((0, b"1234"), (4, b"5678"), (8, b"9012")):
            appended = request(
                port,
                TOKEN_A,
                "PUT",
                f"/api/v1/uploads/{upload_id}",
                content,
                {
                    "Content-Type": "application/octet-stream",
                    "Upload-Offset": str(offset),
                },
            )
            assert appended.status == 200, appended.content
        completed = request(
            port, TOKEN_A, "POST", f"/api/v1/uploads/{upload_id}/complete"
        )
        assert completed.status == 200, completed.content

        sparse_query = urlencode({"rootId": "workspace", "relativePath": "sparse.bin"})
        tail = request(
            port,
            TOKEN_A,
            "GET",
            f"/api/v1/files/content?{sparse_query}",
            headers={"Range": f"bytes={sparse.stat().st_size - 4}-"},
        )
        assert tail.status == 206 and tail.content == b"tail", tail
        excessive_range = request(
            port,
            TOKEN_A,
            "GET",
            f"/api/v1/files/content?{sparse_query}",
            headers={"Range": "bytes=0-4"},
        )
        assert excessive_range.status == 416, excessive_range.content

        opened = request(
            port,
            TOKEN_A,
            "POST",
            "/api/v1/images",
            {"source": {"rootId": "workspace", "relativePath": "fixture.hds"}},
        )
        assert opened.status == 201, opened.content
        image_id = str(opened.json()["data"]["imageId"])
        assert_hidden_reference(port, f"/api/v1/images/{image_id}")
        full_sessions = request(
            port,
            TOKEN_B,
            "POST",
            "/api/v1/images",
            {"source": {"rootId": "workspace", "relativePath": "fixture.hds"}},
        )
        assert full_sessions.status == 429, full_sessions.content
        assert full_sessions.headers["retry-after"] == "1"
        assert (
            request(port, TOKEN_A, "DELETE", f"/api/v1/images/{image_id}").status == 200
        )

        ticket = request(port, TOKEN_A, "POST", "/api/v1/event-tickets")
        assert ticket.status == 201, ticket.content
        ticket_quota = request(port, TOKEN_B, "POST", "/api/v1/event-tickets")
        assert ticket_quota.status == 429, ticket_quota.content
        assert ticket_quota.headers["retry-after"] == "1"

        slow_sockets: list[socket.socket] = []
        for _ in range(3):
            connection = socket.create_connection(("127.0.0.1", port), timeout=2)
            connection.sendall(
                b"GET /api/v1/system/version HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            )
            slow_sockets.append(connection)
        slow_upload = socket.create_connection(("127.0.0.1", port), timeout=2)
        slow_upload.sendall(
            (
                f"PUT /api/v1/uploads/{upload_id} HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                f"Authorization: Bearer {TOKEN_A}\r\n"
                "Content-Type: application/octet-stream\r\n"
                "Upload-Offset: 12\r\n"
                "Content-Length: 4\r\n\r\n"
                "x"
            ).encode("ascii")
        )
        slow_sockets.append(slow_upload)
        started = time.monotonic()
        live = request(port, None, "GET", "/api/v1/system/health/live", timeout=2)
        assert live.status == 204
        assert time.monotonic() - started < 1.0
        for connection in slow_sockets:
            connection.close()

        def read_version(_: int) -> int:
            deadline = time.monotonic() + 8.0
            while True:
                try:
                    return request(
                        port, TOKEN_A, "GET", "/api/v1/system/version", timeout=2
                    ).status
                except OSError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.02)

        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            statuses = list(executor.map(read_version, range(64)))
        assert statuses == [200] * 64

        def submit_job(_: int) -> tuple[int, object, dict[str, str]]:
            response = request(
                port,
                TOKEN_A,
                "POST",
                "/api/v1/fixture-jobs",
                {"load": True},
                timeout=2,
            )
            return response.status, response.json(), response.headers

        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            submissions = list(executor.map(submit_job, range(32)))
        accepted = [body["data"] for status, body, _ in submissions if status == 202]
        limited = [
            (body, headers) for status, body, headers in submissions if status == 429
        ]
        assert accepted and limited, submissions
        assert all(headers["retry-after"] == "1" for _, headers in limited)
        job_id = str(accepted[0]["jobId"])
        assert_hidden_reference(port, f"/api/v1/jobs/{job_id}")
        cancellation_started = time.monotonic()
        cancelled = request(port, TOKEN_A, "DELETE", f"/api/v1/jobs/{job_id}")
        assert cancelled.status == 200, cancelled.content
        terminal = wait_for_job(port, TOKEN_A, job_id, running.process)
        assert terminal["state"] in {"CANCELLED", "COMPLETED"}
        assert time.monotonic() - cancellation_started < 2.0

        time.sleep(0.2)
        after_fds = process_file_descriptors(running.pid)
        after_rss = process_resident_bytes(running.pid)
        if baseline_fds is not None and after_fds is not None:
            assert after_fds <= baseline_fds + 4, (baseline_fds, after_fds)
        if baseline_rss is not None and after_rss is not None:
            assert after_rss <= baseline_rss + maximum_rss_growth, (
                baseline_rss,
                after_rss,
                maximum_rss_growth,
            )

        metrics = request(port, TOKEN_A, "GET", "/api/v1/system/metrics")
        assert metrics.status == 200, metrics.content
        counters = metrics.json()["data"]
        assert counters["responses4xx"] >= len(malformed_cases)
        assert counters["submittedJobs"] == len(accepted)
        assert counters["activeRequests"] >= 1

        assert (
            request(port, TOKEN_A, "DELETE", f"/api/v1/uploads/{upload_id}").status
            == 204
        )
        assert (workspace / "durable-output.bin").read_bytes() == b"published"

    output = (root / "server.log").read_text(encoding="utf-8", errors="replace")
    assert TOKEN_A not in output and TOKEN_B not in output
    assert str(workspace) not in output
    assert '"relativePath"' not in output

    uploads = state / "uploads"
    uploads.mkdir(parents=True, exist_ok=True)
    (uploads / "abandoned.upload").write_bytes(b"partial")
    with ServerProcess(
        server, ["--config", str(config_path)], port, root / "restart.log"
    ):
        assert not any(uploads.iterdir())
        assert (workspace / "durable-output.bin").read_bytes() == b"published"


def exercise_sidecar_shutdown(server: Path, root: Path) -> None:
    workspace = root / "sidecar-workspace"
    workspace.mkdir()
    durable = workspace / "completed.bin"
    durable.write_bytes(b"complete")
    connection_file = root / "sidecar" / "connection.json"
    workspace_store = root / "sidecar-workspaces.json"
    write_workspace_store(workspace_store, workspace)
    port = choose_port()
    running = ServerProcess(
        server,
        [
            "--port",
            str(port),
            "--workspace-store",
            str(workspace_store),
            "--state-directory",
            str(root / "sidecar-state"),
            "--connection-file",
            str(connection_file),
            "--workers",
            "2",
            "--job-workers",
            "1",
            "--write-job-workers",
            "1",
        ],
        port,
        root / "sidecar.log",
    )
    with running:
        assert running.process is not None
        metadata = wait_for_connection_file(connection_file, running.process)
        token = str(metadata["bearerToken"])
        pending = create_upload(port, token, size=4)
        assert pending
        started = time.monotonic()
        shutdown = request(port, token, "POST", "/api/v1/system/shutdown")
        assert shutdown.status == 202, shutdown.content
        running.process.wait(timeout=5)
        assert running.process.returncode == 0
        assert time.monotonic() - started < 5.0
        assert not connection_file.exists()
        assert durable.read_bytes() == b"complete"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    arguments = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="axklib-server-resilience-") as temporary:
        root = Path(temporary)
        exercise_constrained_server(arguments.server, arguments.fixture, root)
        exercise_sidecar_shutdown(arguments.server, root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
