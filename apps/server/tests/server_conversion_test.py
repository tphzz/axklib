from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import tempfile
from pathlib import Path
from typing import Any
from urllib.parse import quote

from server_test_harness import (
    ServerProcess,
    choose_port,
    request,
    wait_for_job,
    write_workspace_store,
)

TOKEN = "0123456789abcdef0123456789abcdef"


def exercise(server: Path, fixture: Path, root: Path) -> None:
    workspace = root / "workspace"
    workspace.mkdir()
    shutil.copyfile(fixture, workspace / "target.hds")
    wave = bytearray(35)
    wave[:6] = b"LM8953"
    wave[22:25] = bytes([0xC9, 2, 2])
    wave[32:] = bytes([0x7F, 0xF0, 0x80])
    (workspace / "TONE.W01").write_bytes(wave)
    write_workspace_store(root / "workspaces.json", workspace)
    port = choose_port()
    config = root / "server.json"
    config.write_text(
        json.dumps(
            {
                "bindAddress": "127.0.0.1",
                "port": port,
                "tokenHashes": [
                    {
                        "principalId": "test",
                        "sha256": hashlib.sha256(TOKEN.encode()).hexdigest(),
                    }
                ],
                "workspaceStore": str(root / "workspaces.json"),
                "stateDirectory": str(root / "state"),
            }
        ),
        encoding="utf-8",
    )
    with ServerProcess(
        server, ["--config", str(config)], port, root / "server.log"
    ) as running:
        assert running.process is not None

        def get(path: str) -> Any:
            result = request(port, TOKEN, "GET", path)
            assert result.status == 200, result.content
            return result.json()["data"]

        def job(path: str, body: dict[str, Any], identity: str = "prepare") -> Any:
            result = request(
                port, TOKEN, "POST", path, body, {"Idempotency-Key": identity}
            )
            assert result.status == 202, result.content
            completed = wait_for_job(
                port, TOKEN, result.json()["data"]["jobId"], running.process
            )
            assert completed["state"] == "COMPLETED", completed
            return completed["result"]

        opened = job(
            "/api/v1/images",
            {
                "sources": [
                    {
                        "kind": "FILE",
                        "file": {"rootId": "workspace", "relativePath": name},
                    }
                    for name in ("target.hds", "TONE.W01")
                ]
            },
            "open",
        )
        image = opened["imageId"]
        document = next(
            item["id"]
            for item in opened["documents"]
            if item["formatId"] == "yamaha.a-series"
        )
        source = next(
            item["id"]
            for item in opened["documents"]
            if item["formatId"] == "yamaha.tx16w"
        )
        base = f"/api/v1/images/{image}/documents/{document}"
        native_base = f"/api/v1/images/{image}/documents/{source}"
        native_objects = get(f"{native_base}/objects?limit=100")["items"]
        for native_object in native_objects:
            detail = get(f"{native_base}/objects/{quote(native_object['id'], safe='')}")
            assert detail["objectId"] == native_object["id"], detail
        for invalid_id in ("%", "%0", "%gg", "%00"):
            rejected = request(port, TOKEN, "GET", f"{native_base}/objects/{invalid_id}")
            assert rejected.status == 400, rejected.content
        objects = get(f"{base}/objects?limit=100")["items"]
        sample = next(item for item in objects if item["type"] == "SBNK")
        editing = get(f"{base}/objects/{sample['id']}")["editing"]
        profiles = get("/api/v1/conversion-profiles")["profiles"]
        assert any(
            profile["id"] == "yamaha.tx16w-to-a-series" for profile in profiles
        ), profiles
        body = {
            "profileId": "yamaha.tx16w-to-a-series",
            "source": {
                "kind": "DOCUMENT",
                "imageId": image,
                "documentId": source,
                "expectedRevision": 1,
            },
            "destination": {
                "imageId": image,
                "documentId": document,
                "expectedRevision": 1,
                "selection": {
                    "partitionIndex": editing["partitionIndex"],
                    "volumeName": editing["volumeName"],
                },
            },
            "options": {"mode": "WAVE_DATA_ONLY"},
            "fidelity": "STRICT",
        }
        before = hashlib.sha256((workspace / "target.hds").read_bytes()).hexdigest()
        strict = job("/api/v1/conversion-plans", body, "strict-plan")
        assert not strict["report"]["executable"], strict
        assert (
            request(
                port, TOKEN, "DELETE", f"/api/v1/conversion-plans/{strict['planId']}"
            ).status
            == 200
        )
        body["fidelity"] = "BEST"
        plan = job("/api/v1/conversion-plans", body, "best-plan")
        stale = job("/api/v1/conversion-plans", body, "stale-plan")
        assert (
            hashlib.sha256((workspace / "target.hds").read_bytes()).hexdigest()
            == before
        )
        path = f"/api/v1/conversion-plans/{plan['planId']}"
        page = get(f"{path}?offset=0&limit=1")["report"]
        assert len(page["issues"]) == 1 and page["issueCount"] > 1, page
        assert page["actions"][0]["details"], page
        rejected = request(
            port,
            TOKEN,
            "POST",
            path + "/executions",
            {"acknowledgedIssueIds": []},
            {"Idempotency-Key": "reject"},
        )
        assert rejected.status == 409, rejected.content
        ids = [
            issue["id"]
            for issue in plan["report"]["issues"]
            if issue["impact"] in {"LOSS", "UNCERTAINTY"}
        ]
        for headers in ({}, {"Idempotency-Key": ""}, {"Idempotency-Key": "x" * 129}):
            invalid = request(
                port,
                TOKEN,
                "POST",
                path + "/executions",
                {"acknowledgedIssueIds": ids},
                headers,
            )
            assert invalid.status == 400, invalid.content
            assert get(path)["state"] == "READY"
        result = job(path + "/executions", {"acknowledgedIssueIds": ids}, "execute")
        assert result["applied"] and result["revision"] == 2, result
        accepted_id = get(path)["jobId"]
        repeated = request(
            port,
            TOKEN,
            "POST",
            path + "/executions",
            {"acknowledgedIssueIds": ids},
            {"Idempotency-Key": "different-key"},
        )
        assert (
            repeated.status == 202 and repeated.json()["data"]["jobId"] == accepted_id
        ), repeated.content
        old_path = f"/api/v1/conversion-plans/{stale['planId']}/executions"
        response = request(
            port,
            TOKEN,
            "POST",
            old_path,
            {"acknowledgedIssueIds": ids},
            {"Idempotency-Key": "stale"},
        )
        assert response.status == 202, response.content
        failed = wait_for_job(
            port, TOKEN, response.json()["data"]["jobId"], running.process
        )
        assert failed["state"] == "FAILED", failed
        assert get(f"/api/v1/images/{image}")["revision"] == 2
        assert (
            request(
                port,
                TOKEN,
                "POST",
                "/api/v1/image-session-tx16w-import-inspections",
                {},
            ).status
            == 404
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="axk-conversion-api-") as directory:
        exercise(args.server.resolve(), args.fixture.resolve(), Path(directory))


if __name__ == "__main__":
    main()
