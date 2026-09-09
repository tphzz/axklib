#!/usr/bin/env python3
"""Run a bounded, read-only image-open matrix through the real server API."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import tempfile
import time
from collections import deque
from pathlib import Path
from typing import Any
from urllib.parse import urlencode

from release_server_smoke import (
    ApiClient,
    Client,
    create_owner_only_directory,
    require_status,
    wait_for_connection,
    wait_for_job,
)


def load_manifest(path: Path, *, maximum_cases: int = 16) -> list[dict[str, Any]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    cases = document.get("cases") if isinstance(document, dict) else None
    if not isinstance(cases, list) or not 1 <= len(cases) <= maximum_cases:
        raise ValueError(f"manifest must contain 1 to {maximum_cases} cases")
    ids: set[str] = set()
    for case in cases:
        if (
            not isinstance(case, dict)
            or not isinstance(case.get("id"), str)
            or not case["id"]
            or case["id"] in ids
        ):
            raise ValueError("case ids must be nonempty and unique")
        ids.add(case["id"])
        if case.get("path") is None:
            if not case.get("unavailableReason"):
                raise ValueError("unavailable case requires a reason")
            continue
        if not isinstance(case["path"], str) or not isinstance(case.get("root"), str):
            raise ValueError("case requires a root and relative path")
        relative = Path(case["path"])
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError("case path must stay inside its corpus root")
        expected = case.get("expected")
        if (
            not isinstance(expected, dict)
            or not {"format", "filesystemName", "deviceView", "minimumFiles"} <= expected.keys()
        ):
            raise ValueError(
                "case requires explicit format, filesystem, device and file expectations"
            )
    return cases


def check_image(
    client: ApiClient,
    relative_path: str,
    expected: dict[str, Any],
    *,
    maximum_entries: int = 10000,
    maximum_requests: int = 256,
    traversal_seconds: int = 30,
    reject_invalid: bool = True,
) -> dict[str, Any]:
    submitted = require_status(
        client.request(
            "POST",
            "/images",
            {
                "source": {
                    "kind": "FILE",
                    "file": {"rootId": "corpus", "relativePath": relative_path},
                }
            },
        ),
        202,
        "image submission",
    )
    # Fetching the completed job, not just its progress events, validates the serialized result.
    summary = wait_for_job(client, str(submitted["data"]["jobId"]))["result"]
    image_path = f"/images/{summary['imageId']}"
    requests = 0
    deadline = time.monotonic() + traversal_seconds

    def get(suffix: str, query: dict[str, Any]) -> dict[str, Any]:
        nonlocal requests
        requests += 1
        if requests > maximum_requests or time.monotonic() > deadline:
            raise RuntimeError("image traversal request/time limit exceeded")
        data: dict[str, Any] = require_status(
            client.request("GET", f"{image_path}{suffix}?{urlencode(query)}"), 200, suffix
        )["data"]
        return data

    try:
        current = require_status(client.request("GET", image_path), 200, "image summary")["data"]
        if current != summary:
            raise RuntimeError("completed job and current image summary differ")
        if summary["format"] != expected["format"]:
            raise RuntimeError(f"format: expected {expected['format']}, got {summary['format']}")
        invalid = not summary["validation"]["valid"] or summary["validation"]["errorCount"]
        if reject_invalid and invalid:
            raise RuntimeError(f"image validation failed: {summary['validation']}")
        validation_issues: list[dict[str, Any]] = []
        if invalid:
            issue_cursor: str | None = None
            issue_cursors: set[str] = set()
            while True:
                issue_query: dict[str, Any] = {"limit": 200}
                if issue_cursor is not None:
                    issue_query["cursor"] = issue_cursor
                issue_page = get("/validation/issues", issue_query)
                validation_issues.extend(issue_page["items"])
                if len(validation_issues) > maximum_entries:
                    raise RuntimeError("validation issue limit exceeded")
                issue_cursor = issue_page["nextCursor"]
                if issue_cursor is None:
                    break
                if issue_cursor in issue_cursors or not issue_page["items"]:
                    raise RuntimeError("validation pagination did not progress")
                issue_cursors.add(issue_cursor)
        parents: deque[str | None] = deque([None])
        seen: set[str] = set()
        names: set[str] = set()
        metadata: set[str] = set()
        file_count = 0
        while parents:
            parent = parents.popleft()
            offset = 0
            total: int | None = None
            while True:
                query: dict[str, Any] = {
                    "expectedRevision": summary["revision"],
                    "offset": offset,
                    "limit": 200,
                }
                if parent is not None:
                    query["parentId"] = parent
                page = get("/filesystem", query)
                if not page["available"] or page["revision"] != summary["revision"]:
                    raise RuntimeError("filesystem unavailable or revision changed")
                for field in ("filesystemName", "deviceView"):
                    if page[field] != expected[field]:
                        raise RuntimeError(
                            f"{field}: expected {expected[field]!r}, got {page[field]!r}"
                        )
                if total is not None and total != page["totalCount"]:
                    raise RuntimeError("filesystem page total changed")
                total = page["totalCount"]
                for entry in page["items"]:
                    if entry["id"] in seen or entry["parentId"] != parent:
                        raise RuntimeError("duplicate filesystem entry or incorrect parent")
                    seen.add(entry["id"])
                    names.add(entry["name"])
                    if entry.get("filesystemMetadata"):
                        metadata.add(entry["name"])
                    if len(seen) > maximum_entries:
                        raise RuntimeError("filesystem entry limit exceeded")
                    if entry["kind"] == "FILE":
                        file_count += 1
                    else:
                        parents.append(entry["id"])
                offset += len(page["items"])
                if offset == total:
                    break
                if not page["items"] or offset > total:
                    raise RuntimeError("filesystem pagination did not progress consistently")
        if file_count < expected["minimumFiles"]:
            raise RuntimeError(
                f"only {file_count} files; expected at least {expected['minimumFiles']}"
            )
        if not set(expected.get("names", [])) <= names:
            raise RuntimeError("expected named filesystem entries are missing")
        if not set(expected.get("metadata", [])) <= metadata:
            raise RuntimeError("expected filesystem metadata classification is missing")

        device_nodes = 0
        if expected["deviceView"] is not None:
            parents = deque([None])
            device_seen: set[str] = set()
            while parents:
                parent = parents.popleft()
                cursor: str | None = None
                cursors: set[str] = set()
                while True:
                    query = {"limit": 200}
                    if parent is not None:
                        query["parentId"] = parent
                    if cursor is not None:
                        query["cursor"] = cursor
                    page = get("/content", query)
                    for item in page["items"]:
                        if item["id"] in device_seen:
                            raise RuntimeError("duplicate Device content node")
                        device_seen.add(item["id"])
                        if len(device_seen) > maximum_entries:
                            raise RuntimeError("Device content entry limit exceeded")
                        # Walk navigation scopes, not each object's relationship expansion.
                        if item["childCount"] and item.get("objectId") is None:
                            parents.append(item["id"])
                    cursor = page["nextCursor"]
                    if cursor is None:
                        break
                    if cursor in cursors or not page["items"]:
                        raise RuntimeError("Device pagination did not progress")
                    cursors.add(cursor)
            device_nodes = len(device_seen)
            if not device_nodes or summary["objectCount"] == 0:
                raise RuntimeError("expected nonempty Device view")
        return {
            "format": summary["format"],
            "filesystemName": expected["filesystemName"],
            "deviceView": expected["deviceView"],
            "files": file_count,
            "filesystemEntries": len(seen),
            "deviceNodes": device_nodes,
            "requests": requests,
            "floppySet": summary.get("floppySet"),
            "validation": summary.get("validation"),
            "validationIssues": validation_issues,
        }
    finally:
        require_status(client.request("DELETE", image_path), 200, "image close")


def run_case(
    server: Path,
    source: Path,
    expected: dict[str, Any],
    log: Path,
    *,
    maximum_entries: int = 10000,
    maximum_requests: int = 256,
    traversal_seconds: int = 30,
    reject_invalid: bool = True,
) -> dict[str, Any]:
    before = source.stat()
    with (
        tempfile.TemporaryDirectory(prefix="axklib-image-open-") as temporary,
        log.open("wb") as output,
    ):
        root = Path(temporary)
        state = root / "state"
        create_owner_only_directory(state)
        connection_file = state / "connection.json"
        store = root / "workspaces.json"
        store.write_text(
            json.dumps(
                {
                    "schemaVersion": 1,
                    "revision": 1,
                    "workspaces": [
                        {
                            "id": "corpus",
                            "displayName": "Read-only corpus",
                            "path": str(source.parent),
                            "writable": False,
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )
        process = subprocess.Popen(
            [
                str(server),
                "--port",
                "0",
                "--workspace-store",
                str(store),
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
            stdout=output,
            stderr=subprocess.STDOUT,
        )
        try:
            connection = wait_for_connection(connection_file, process)
            client = Client(str(connection["baseUrl"]), str(connection["bearerToken"]))
            result = check_image(
                client,
                source.name,
                expected,
                maximum_entries=maximum_entries,
                maximum_requests=maximum_requests,
                traversal_seconds=traversal_seconds,
                reject_invalid=reject_invalid,
            )
            require_status(client.request("POST", "/system/shutdown"), 202, "server shutdown")
            process.wait(timeout=5)
            if process.returncode != 0:
                raise RuntimeError(f"server exited with {process.returncode}")
            return result
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
            after = source.stat()
            if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                raise RuntimeError(
                    "source size or modification time changed during read-only smoke"
                )


def result_code(results: list[dict[str, Any]]) -> int:
    if any(row["status"] == "failed" for row in results):
        return 1
    return 2 if not results or any(row["status"] != "passed" for row in results) else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--root", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    cases = load_manifest(args.manifest)
    roots: dict[str, Path] = {}
    for value in args.root:
        name, separator, path = value.partition("=")
        if not separator or not name or not path or name in roots:
            parser.error("each --root must be a unique NAME=PATH")
        roots[name] = Path(path).resolve()
    server = args.server.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=False)
    with server.open("rb") as handle:
        server_hash = hashlib.file_digest(handle, "sha256").hexdigest()
    results: list[dict[str, Any]] = [{"id": case["id"], "status": "not_run"} for case in cases]
    for index, case in enumerate(cases):
        started = time.monotonic()
        row: dict[str, Any] = {"id": case["id"], "status": "missing"}
        corpus = roots.get(case.get("root", ""))
        if case.get("path") is None:
            row["reason"] = case["unavailableReason"]
        elif corpus is None:
            row["reason"] = f"missing --root {case['root']}"
        else:
            source = (corpus / case["path"]).resolve()
            row["source"] = str(source)
            if not source.is_relative_to(corpus):
                row.update(status="failed", reason="source escapes corpus root")
            elif not source.is_file():
                row["reason"] = "corpus file is missing"
            else:
                log = args.output / f"{index + 1:02d}-server.log"
                row["log"] = str(log)
                row["sourceBytes"] = source.stat().st_size
                try:
                    row.update(run_case(server, source, case["expected"], log), status="passed")
                except (
                    RuntimeError,
                    OSError,
                    ValueError,
                    KeyError,
                    subprocess.TimeoutExpired,
                ) as error:
                    row.update(status="failed", reason=str(error))
        row["seconds"] = round(time.monotonic() - started, 3)
        results[index] = row
        print(
            f"{row['status'].upper()} {row['id']}: {row.get('reason', str(row['seconds']) + 's')}",
            flush=True,
        )
        report = {
            "server": str(server),
            "serverSha256": server_hash,
            "releaseReady": False,
            "exitCode": result_code(results),
            "results": results,
        }
        (args.output / "summary.json").write_text(
            json.dumps(report, indent=2) + "\n", encoding="utf-8"
        )
    return result_code(results)


if __name__ == "__main__":
    raise SystemExit(main())
