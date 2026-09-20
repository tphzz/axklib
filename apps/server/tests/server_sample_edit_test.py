from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import tempfile
from pathlib import Path
from typing import Any

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
    shutil.copyfile(fixture, workspace / "sample.hds")
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
        process = running.process

        def get(path: str) -> Any:
            response = request(port, TOKEN, "GET", path)
            assert response.status == 200, response.content
            return response.json()["data"]

        def job(path: str, body: dict[str, Any], identity: str) -> Any:
            response = request(
                port, TOKEN, "POST", path, body, {"Idempotency-Key": identity}
            )
            assert response.status == 202, response.content
            job_id = response.json()["data"]["jobId"]
            completed = wait_for_job(port, TOKEN, job_id, process)
            assert completed["state"] == "COMPLETED", completed
            # Repeated status checks must serialize the committed result, not return HTTP 500.
            assert get(f"/api/v1/jobs/{job_id}") == completed
            return completed["result"]

        opened = job(
            "/api/v1/images",
            {
                "source": {
                    "kind": "FILE",
                    "file": {"rootId": "workspace", "relativePath": "sample.hds"},
                }
            },
            "open",
        )
        image_id = opened["imageId"]
        base = f"/api/v1/images/{image_id}"
        objects = get(f"{base}/objects?limit=100")["items"]
        sample = next(item for item in objects if item["type"] == "SBNK")
        detail_path = f"{base}/objects/{sample['id']}"
        detail = get(detail_path)
        assert detail["editing"]["editable"], detail
        for metadata in (
            sample["sampleFormat"],
            detail["object"]["sampleFormat"],
            detail["editing"]["sampleFormat"],
        ):
            assert metadata["format"] == "A4000_A5000_224", metadata
        assert detail["editing"]["formatConversions"][0]["targetFormat"] == "A3000_188"

        def operation(
            snapshot: Any, identity: str, kind: str, level: int
        ) -> dict[str, Any]:
            editing = snapshot["editing"]
            return {
                "id": identity,
                "type": kind,
                "partition_index": editing["partitionIndex"],
                "volume_name": editing["volumeName"],
                "sample_name": snapshot["object"]["name"],
                "expected_payload_sha256": editing["payloadSha256"],
                "parameters": {"level": level},
            }

        def alter(snapshot: Any, change: dict[str, Any]) -> Any:
            result = job(
                "/api/v1/image-session-alterations",
                {
                    "imageId": image_id,
                    "expectedRevision": snapshot["image"]["revision"],
                    "manifest": {
                        "inline": {"schema_version": "1.0", "operations": [change]}
                    },
                    "inputBindings": [],
                },
                change["id"],
            )
            assert result["applied"] is True, result
            assert result["revision"] == snapshot["image"]["revision"] + 1, result
            assert result["operations"][0]["type"] == change["type"].upper(), result
            assert get(base)["revision"] == result["revision"]
            return result

        original_level = detail["editing"]["parameters"]["level"]
        for index in (1, 2):
            level = (original_level + index) % 128
            before_hash = detail["editing"]["payloadSha256"]
            alter(
                detail,
                operation(detail, f"save-{index}", "update_sbnk_parameters", level),
            )
            detail = get(detail_path)
            assert detail["editing"]["parameters"]["level"] == level, detail
            assert detail["editing"]["payloadSha256"] != before_hash
            assert detail["editing"]["editable"]

        source_detail = detail
        duplicate = operation(detail, "duplicate", "duplicate_sbnk", 42)
        duplicate["new_name"] = "Saved Copy"
        alter(detail, duplicate)
        after = get(f"{base}/objects?limit=100")["items"]
        assert len(after) == len(objects) + 1
        copied = next(
            item
            for item in after
            if item["name"] == duplicate["new_name"] and item["type"] == "SBNK"
        )
        copied_detail = get(f"{base}/objects/{copied['id']}")
        assert copied_detail["editing"]["parameters"]["level"] == 42
        assert (
            copied_detail["editing"]["sources"] == source_detail["editing"]["sources"]
        )
        assert (
            get(detail_path)["editing"]["payloadSha256"]
            == source_detail["editing"]["payloadSha256"]
        )
        assert {item["id"] for item in after if item["type"] == "SMPL"} == {
            item["id"] for item in objects if item["type"] == "SMPL"
        }
        copied_path = f"{base}/objects/{copied['id']}"
        competing = job(
            "/api/v1/images",
            {"source": {"kind": "FILE", "file": {"rootId": "workspace", "relativePath": "sample.hds"}}},
            "open-competing-session",
        )
        rejected = operation(copied_detail, "blocked-conversion", "convert_sbnk_format", 42)
        del rejected["parameters"]
        rejected["target_format"] = "a3000_188"
        submitted = request(port, TOKEN, "POST", "/api/v1/image-session-alterations", {
            "imageId": image_id,
            "expectedRevision": copied_detail["image"]["revision"],
            "manifest": {"inline": {"schema_version": "1.0", "operations": [rejected]}},
            "inputBindings": [],
        }, {"Idempotency-Key": "blocked-conversion"})
        assert submitted.status == 202, submitted.content
        failed_id = submitted.json()["data"]["jobId"]
        failed = wait_for_job(port, TOKEN, failed_id, process)
        assert failed["state"] == "FAILED" and failed["error"]["code"] == "entry_in_use", failed
        assert get(f"/api/v1/jobs/{failed_id}") == failed
        assert get(copied_path)["editing"]["payloadSha256"] == copied_detail["editing"]["payloadSha256"]
        assert request(port, TOKEN, "DELETE", f"/api/v1/images/{competing['imageId']}").status == 200
        for target, byte_count in (("a3000_188", 188), ("a4000_a5000_224", 224)):
            preview = copied_detail["editing"]["formatConversions"][0]
            assert preview["allowed"] and preview["targetFormat"] == target.upper(), preview
            conversion = operation(copied_detail, f"convert-{target}", "convert_sbnk_format", 42)
            del conversion["parameters"]
            conversion["target_format"] = target
            alter(copied_detail, conversion)
            copied_detail = get(copied_path)
            assert copied_detail["object"]["id"] == copied["id"]
            assert copied_detail["object"]["name"] == copied["name"]
            assert copied_detail["editing"]["parameters"]["level"] == 42
            assert copied_detail["editing"]["sources"] == source_detail["editing"]["sources"]
            stored = copied_detail["editing"]["sampleFormat"]
            assert stored["format"] == target.upper() and stored["parameterBytes"] == byte_count, stored
            rows = get(f"{base}/objects?limit=100")["items"]
            refreshed = next(item for item in rows if item["id"] == copied["id"])
            assert refreshed["sampleFormat"]["format"] == target.upper(), refreshed
        alter(copied_detail, operation(copied_detail, "save-converted", "update_sbnk_parameters", 43))
        assert get(copied_path)["editing"]["parameters"]["level"] == 43
        closed = request(port, TOKEN, "DELETE", base)
        assert closed.status == 200 and closed.json()["data"]["closed"] is True, (
            closed.content
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    arguments = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="axklib-sample-edit-") as directory:
        exercise(
            arguments.server.resolve(), arguments.fixture.resolve(), Path(directory)
        )
    print("Sample edit/save/duplicate/convert both ways/edit/close HTTP regression passed")


if __name__ == "__main__":
    main()
