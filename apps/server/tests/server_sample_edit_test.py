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
        assert detail["formatConversion"]["formatConversions"][0]["targetFormat"] == "A3000_188"
        assert "formatConversions" not in detail["editing"]

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
            body = {
                "imageId": image_id,
                "expectedRevision": snapshot["image"]["revision"],
                "manifest": {
                    "inline": {"schema_version": "1.0", "operations": [change]}
                },
                "inputBindings": [],
                "capacityPolicy": {
                    "target": "A4000_A5000",
                },
            }
            response = request(
                port, TOKEN, "POST", "/api/v1/image-session-alteration-inspections", body
            )
            assert response.status == 200, response.content
            capacity = response.json()["data"]["capacity"]
            assert capacity["allowed"], capacity
            result = job(
                "/api/v1/image-session-alterations",
                body,
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
            preview = copied_detail["formatConversion"]["formatConversions"][0]
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
        current_sample = get(detail_path)
        alter(current_sample, {"id": "insert-format-bank", "type": "insert_sbac",
                              "partition_index": current_sample["editing"]["partitionIndex"],
                              "volume_name": current_sample["editing"]["volumeName"],
                              "sample_bank": {"name": "Format bank", "member_samples": [sample["name"]],
                                              "storage_format": "a4000_a5000_224"}})
        bank = next(item for item in get(f"{base}/objects?limit=100")["items"] if item["name"] == "Format bank")
        bank_path = f"{base}/objects/{bank['id']}"
        bank_detail = get(bank_path)
        assert bank_detail["editing"]["profile"] == "a-series/sample-bank"
        assert bank_detail["editing"]["bankOverrides"]["members"] == [{"name": sample["name"], "objectId": sample["id"]}]
        members = {item["id"]: get(f"{base}/objects/{item['id']}")["formatConversion"]["payloadSha256"]
                   for item in after if item["type"] == "SBNK"}
        def relationship_values(snapshot: Any) -> Any:
            # Relationship IDs are revision-scoped; object identities and edge values must persist.
            return [{key: value for key, value in edge.items() if key != "id"} for edge in snapshot["relationships"]]

        relationships = relationship_values(bank_detail)
        for target in ("a3000_188", "a4000_a5000_224"):
            capability = bank_detail["formatConversion"]
            assert capability["canConvertFormat"], capability
            assert capability["formatConversions"][0]["allowed"], capability
            change = {"id": f"bank-{target}", "type": "convert_sbac_format",
                      "partition_index": capability["partitionIndex"], "volume_name": capability["volumeName"],
                      "sample_bank_name": bank["name"], "target_format": target,
                      "expected_payload_sha256": capability["payloadSha256"]}
            alter(bank_detail, change)
            bank_detail = get(bank_path)
            assert bank_detail["object"]["id"] == bank["id"]
            assert bank_detail["object"]["sampleFormat"]["format"] == target.upper()
            assert relationship_values(bank_detail) == relationships
            assert bank_detail["editing"]["profile"] == "a-series/sample-bank"
            editing = bank_detail["editing"]
            units = {unit["id"]: unit for unit in editing["bankOverrides"]["units"]}
            assert units[65]["keys"] == ["aeg.attack_rate", "aeg.decay_rate", "aeg.release_rate"]
            assert units[49]["selectors"] == ([49, 50, 51] if target == "a3000_188" else [49, 50, 51, 85])
            assert "root_key" in editing["blockedParameters"]
            for active in (True, False):
                change = {"id": f"overrides-{target}-{active}", "type": "update_sample_bank_overrides",
                          "partition_index": editing["partitionIndex"], "volume_name": editing["volumeName"],
                          "sample_bank_name": bank["name"], "expected_payload_sha256": editing["payloadSha256"],
                          "parameters": {"level": 81} if active else {},
                          "enable": [33] if active else [], "disable": [] if active else [33]}
                alter(bank_detail, change)
                bank_detail = get(bank_path)
                editing = bank_detail["editing"]
                assert editing["editable"] and editing["parameters"]["level"] == 81
                assert next(unit for unit in editing["bankOverrides"]["units"] if unit["id"] == 33)["activeSelectors"] == ([33] if active else [])
                assert relationship_values(bank_detail) == relationships
                for member, digest in members.items():
                    assert get(f"{base}/objects/{member}")["editing"]["payloadSha256"] == digest
            for member, digest in members.items():
                assert get(f"{base}/objects/{member}")["formatConversion"]["payloadSha256"] == digest
            refreshed = next(item for item in get(f"{base}/objects?limit=100")["items"] if item["id"] == bank["id"])
            assert refreshed["sampleFormat"]["format"] == target.upper()
        closed = request(port, TOKEN, "DELETE", base)
        assert closed.status == 200 and closed.json()["data"]["closed"] is True, (
            closed.content
        )
        # Seed only the disposable, closed image. Both stored bank formats must survive a real session reopen.
        opened = job("/api/v1/images", {"source": {"kind": "FILE", "file": {
            "rootId": "workspace", "relativePath": "sample.hds"}}}, "open-root-fixture")
        image_id = opened["imageId"]
        base = f"/api/v1/images/{image_id}"
        root_names = []
        current_rows = get(f"{base}/objects?limit=100")["items"]
        sample = next(row for row in current_rows if row["type"] == "SBNK" and row["name"] == sample["name"])
        for index, target in enumerate(("a3000_188", "a4000_a5000_224")):
            name = f"Root bank {index}"
            root_names.append(name)
            snapshot = get(f"{base}/objects/{sample['id']}")
            alter(snapshot, {"id": f"insert-root-{index}", "type": "insert_sbac",
                             "partition_index": snapshot["editing"]["partitionIndex"],
                             "volume_name": snapshot["editing"]["volumeName"],
                             "sample_bank": {"name": name, "member_samples": [sample["name"]],
                                             "storage_format": target}})
        assert request(port, TOKEN, "DELETE", base).status == 200
        image_path = workspace / "sample.hds"
        seeded = bytearray(image_path.read_bytes())
        original_banks = {}
        for name in root_names:
            marker = b"FSFSDEV3SPLXSBAC"
            offsets = []
            offset = seeded.find(marker)
            while offset != -1:
                if bytes(seeded[offset + 0x32:offset + 0x42]).rstrip(b"\0 ").decode("ascii") == name:
                    offsets.append(offset)
                offset = seeded.find(marker, offset + len(marker))
            assert len(offsets) == 1, offsets
            offset = offsets[0]
            seeded[offset + 0x137] = 0x40
            seeded[offset + 0xA6:offset + 0xB2] = bytes.fromhex("433c5622ac44ff0012ab34cd")
            seeded[offset + 0x140:offset + 0x144] = bytes.fromhex("abcdef01")
            length_field = 0x18 if name == root_names[0] else 0x1C
            length = int.from_bytes(seeded[offset + length_field:offset + length_field + 4], "big") + 0x30
            original_banks[name] = bytes(seeded[offset:offset + length])
        image_path.write_bytes(seeded)
        opened = job("/api/v1/images", {"source": {"kind": "FILE", "file": {
            "rootId": "workspace", "relativePath": "sample.hds"}}}, "open-preserved-root")
        image_id = opened["imageId"]
        base = f"/api/v1/images/{image_id}"
        root_rows = get(f"{base}/objects?limit=100")["items"]
        unaffected = {(row["type"], row["name"]): get(f"{base}/objects/{row['id']}")["formatConversion"]["payloadSha256"]
                      for row in root_rows if row["type"] in ("SBNK", "SBAC") and row["name"] not in root_names}
        for index, name in enumerate(root_names):
            row = next(item for item in root_rows if item["name"] == name)
            root_path = f"{base}/objects/{row['id']}"
            snapshot = get(root_path)
            edges = relationship_values(snapshot)
            editing = snapshot["editing"]
            assert editing["editable"] and editing["reason"] == "", editing
            assert editing["sampleFormat"]["diagnostics"] == []
            assert editing["sampleFormat"]["parameterIssues"] == []
            assert "root_key" in editing["blockedParameters"]
            assert all(unit["id"] != 6 and "root_key" not in unit["keys"] for unit in editing["bankOverrides"]["units"])
            for active in (True, False):
                change = {"id": f"root-save-{index}-{active}", "type": "update_sample_bank_overrides",
                          "partition_index": editing["partitionIndex"], "volume_name": editing["volumeName"],
                          "sample_bank_name": name, "expected_payload_sha256": editing["payloadSha256"],
                          "parameters": {"level": 82} if active else {},
                          "enable": [33] if active else [], "disable": [] if active else [33]}
                alter(snapshot, change)
                snapshot = get(root_path)
                editing = snapshot["editing"]
                assert editing["parameters"]["root_key"] == 67
                assert relationship_values(snapshot) == edges
            expected = bytearray(original_banks[name])
            expected[0xE6] = 82
            assert editing["payloadSha256"] == hashlib.sha256(expected).hexdigest()
            for rejection, forbidden in enumerate((
                {"parameters": {"root_key": 64, "level": 83}, "enable": [33], "disable": []},
                {"parameters": {}, "enable": [6], "disable": []},
                {"parameters": {}, "enable": [], "disable": [6]},
            )):
                identity = f"root-reject-{index}-{rejection}"
                invalid = {**change, **forbidden, "id": identity,
                           "expected_payload_sha256": editing["payloadSha256"]}
                body = {"imageId": image_id, "expectedRevision": snapshot["image"]["revision"],
                        "manifest": {"inline": {"schema_version": "1.0", "operations": [invalid]}}, "inputBindings": []}
                response = request(port, TOKEN, "POST", "/api/v1/image-session-alterations", body,
                                   {"Idempotency-Key": identity})
                assert response.status == 202, response.content
                failed = wait_for_job(port, TOKEN, response.json()["data"]["jobId"], process)
                assert failed["state"] == "FAILED", failed
                assert get(root_path)["editing"]["payloadSha256"] == editing["payloadSha256"]
                assert get(base)["revision"] == snapshot["image"]["revision"]
            assert not snapshot["formatConversion"]["formatConversions"][0]["allowed"]
        assert request(port, TOKEN, "DELETE", base).status == 200
        reopened = job("/api/v1/images", {"source": {"kind": "FILE", "file": {
            "rootId": "workspace", "relativePath": "sample.hds"}}}, "reopen-preserved-root")
        base = f"/api/v1/images/{reopened['imageId']}"
        for row in get(f"{base}/objects?limit=100")["items"]:
            snapshot = get(f"{base}/objects/{row['id']}")
            if row["name"] in root_names:
                expected = bytearray(original_banks[row["name"]])
                expected[0xE6] = 82
                assert snapshot["editing"]["editable"]
                assert snapshot["editing"]["payloadSha256"] == hashlib.sha256(expected).hexdigest()
            elif (row["type"], row["name"]) in unaffected:
                assert snapshot["formatConversion"]["payloadSha256"] == unaffected[(row["type"], row["name"])]
        assert request(port, TOKEN, "DELETE", base).status == 200


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
