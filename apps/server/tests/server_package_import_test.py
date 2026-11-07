from __future__ import annotations

import argparse
import hashlib
import json
import signal
import socket
import struct
import subprocess
import tempfile
import wave
from pathlib import Path
from typing import Any

from server_test_harness import (
    ServerProcess,
    choose_port,
    request,
    scaled_timeout,
    wait_for_job,
    write_workspace_store,
)

TOKEN = "0123456789abcdef0123456789abcdef"
FOREIGN_TOKEN = "abcdef0123456789abcdef0123456789"
PLAN_ROUTE = "/api/v1/image-session-package-import-plans"
APPLY_ROUTE = "/api/v1/image-session-package-imports"
RELEASE_ROUTE = "/api/v1/image-session-package-import-plan-releases"


def run_cli(cli: Path, workspace: Path, *arguments: str) -> None:
    completed = subprocess.run(
        [str(cli), *arguments],
        cwd=workspace,
        capture_output=True,
        text=True,
        timeout=scaled_timeout(10),
    )
    assert completed.returncode == 0, (arguments, completed.stdout, completed.stderr)


def prepare_media(cli: Path, workspace: Path) -> dict[str, bytes]:
    def write_manifest(name: str, manifest: dict[str, Any]) -> None:
        (workspace / f"{name}.json").write_text(json.dumps(manifest), encoding="utf-8")

    def hds_manifest(volume: dict[str, Any]) -> dict[str, Any]:
        return {
            "schema_version": "1.0",
            "size_bytes": 1048576,
            "partitions": [{"name": "Test", "volumes": [volume]}],
        }

    expected_pcm: dict[str, bytes] = {}
    for name, suffix, amplitude in (("alpha", "A", 1000), ("beta", "B", 2000), ("conflict", "A", 3000)):
        pcm = struct.pack("<hhhh", 0, amplitude, -amplitude, 0)
        # Raw Wave Data exports include the writer's four PCM guard frames.
        expected_pcm[name] = pcm + pcm[:8]
        with wave.open(str(workspace / f"{name}.wav"), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(44100)
            output.writeframes(pcm)
        volume_name = f"Source {suffix}"
        volume = {
            "name": volume_name,
            "waveforms": [{
                "id": "tone", "name": f"Tone {suffix}", "path": f"{name}.wav", "root_key": 60,
            }],
            "samples": [{"name": f"Sample {suffix}", "waveform_id": "tone"}],
        }
        write_manifest(name, hds_manifest(volume))
        run_cli(cli, workspace, "create", "hds", f"{name}.json", "-o", f"{name}.hds")
        run_cli(
            cli, workspace, "package", "export", f"{name}.hds", "--root", "volume",
            "--partition", "0", "--volume", volume_name, "-o", f"{name}.axkvol",
        )
        if name == "alpha":
            write_manifest("floppy", {
                "schema_version": "1.0", "format": "fat12_floppy", "authored_volume": volume,
            })
            run_cli(cli, workspace, "create", "floppy", "floppy.json", "-o", "source.ima")
    write_manifest("target", hds_manifest({"name": "Imported", "waveforms": [], "samples": []}))
    run_cli(cli, workspace, "create", "hds", "target.json", "-o", "target.hds")
    for name in ("create", "hints", "floppy"):
        (workspace / f"{name}-target.hds").write_bytes((workspace / "target.hds").read_bytes())
    return expected_pcm


def verify_pcm(cli: Path, workspace: Path, filename: str, expected: list[bytes]) -> None:
    output_name = f"extract-{filename}"
    run_cli(cli, workspace, "extract", "wav", "file", filename, "-o", output_name, "--strict")
    actual: list[bytes] = []
    for path in sorted((workspace / output_name).rglob("*.wav")):
        with wave.open(str(path), "rb") as audio:
            assert (audio.getnchannels(), audio.getsampwidth(), audio.getframerate()) == (1, 2, 44100), path
            actual.append(audio.readframes(audio.getnframes()))
    assert sorted(actual) == sorted(expected), (filename, actual, expected)


def exercise(server: Path, cli: Path, root: Path) -> None:
    workspace = root / "workspace"
    workspace.mkdir()
    expected_pcm = prepare_media(cli, workspace)
    write_workspace_store(root / "workspaces.json", workspace)
    port = choose_port()
    config = root / "server.json"
    config.write_text(
        json.dumps({
            "bindAddress": "127.0.0.1",
            "port": port,
            "tokenHashes": [
                {"principalId": owner, "sha256": hashlib.sha256(token.encode()).hexdigest()}
                for owner, token in (("owner", TOKEN), ("foreign", FOREIGN_TOKEN))
            ],
            "workspaceStore": str(root / "workspaces.json"),
            "stateDirectory": str(root / "state"),
        }),
        encoding="utf-8",
    )
    with ServerProcess(server, ["--config", str(config)], port, root / "server.log") as running:
        process = running.process
        assert process is not None
        sequence = 0

        def post(path: str, body: dict[str, Any]) -> Any:
            response = request(port, TOKEN, "POST", path, body)
            print(f"POST {path}: HTTP {response.status}", flush=True)
            assert response.status == 200, response.content
            return response.json()["data"]

        def get(path: str, token: str = TOKEN) -> Any:
            response = request(port, token, "GET", path)
            assert response.status == 200, response.content
            return response.json()["data"]

        def job(path: str, body: dict[str, Any], token: str = TOKEN, error: str | None = None) -> Any:
            nonlocal sequence
            sequence += 1
            response = request(
                port, token, "POST", path, body, {"Idempotency-Key": f"package-test-{sequence}"}
            )
            assert response.status == 202, response.content
            job_id = response.json()["data"]["jobId"]
            completed = wait_for_job(port, token, job_id, process)
            if error is not None:
                assert completed["state"] == "FAILED", completed
                assert completed["error"]["code"] == error, completed
            else:
                assert completed["state"] == "COMPLETED", completed
            for _ in range(2):
                assert get(f"/api/v1/jobs/{job_id}", token) == completed
            return completed["result"] if error is None else completed

        def open_image(filename: str) -> Any:
            return job("/api/v1/images", {"sources": [{"kind": "FILE", "file": {
                "rootId": "workspace", "relativePath": filename,
            }}]})

        def identity(opened: Any) -> dict[str, Any]:
            document = opened["documents"][0]
            assert document["formatId"] == "yamaha.a-series", document
            return {
                "imageId": opened["imageId"], "documentId": document["id"],
                "expectedRevision": opened["revision"],
            }

        def bases(opened: Any) -> tuple[str, str]:
            image = f"/api/v1/images/{opened['imageId']}"
            return image, f"{image}/documents/{opened['documents'][0]['id']}"

        def snapshot(opened: Any, filename: str) -> Any:
            image, document = bases(opened)
            return {
                "sha256": hashlib.sha256((workspace / filename).read_bytes()).hexdigest(),
                "image": get(image),
                "objects": get(f"{document}/objects?limit=100"),
                "relationships": get(f"{document}/relationships?limit=100"),
            }

        def expect_error(path: str, body: dict[str, Any], status: int, code: str, token: str = TOKEN) -> None:
            response = request(port, token, "POST", path, body)
            assert response.status == status, response.content
            assert response.json()["error"]["code"] == code, response.content

        def upload(name: str) -> dict[str, Any]:
            content = (workspace / f"{name}.axkvol").read_bytes()
            response = request(port, TOKEN, "POST", "/api/v1/uploads", {
                "filename": f"{name}.axkvol", "kind": "PACKAGE",
                "mediaType": "application/vnd.axklib.package", "size": len(content),
                "sha256": hashlib.sha256(content).hexdigest(),
            })
            assert response.status == 201, response.content
            upload_id = response.json()["data"]["uploadId"]
            response = request(
                port, TOKEN, "PUT", f"/api/v1/uploads/{upload_id}", content,
                {"Content-Type": "application/octet-stream", "Upload-Offset": "0"},
            )
            assert response.status == 200, response.content
            assert response.headers["upload-offset"] == str(len(content)), response.headers
            assert post(f"/api/v1/uploads/{upload_id}/complete", {})["state"] == "READY"
            return {"uploadRef": {"uploadId": upload_id}}

        files = {
            name: {"fileRef": {"rootId": "workspace", "relativePath": f"{name}.axkvol"}}
            for name in ("alpha", "beta", "conflict")
        }
        uploads = {name: upload(name) for name in ("alpha", "beta")}
        summaries: dict[str, dict[str, Any]] = {}
        for name, reference in [*files.items(), *uploads.items()]:
            inspected = post("/api/v1/package-inspections", {"package": reference})
            verified = post("/api/v1/package-verifications", {"package": reference})
            assert inspected["valid"] and verified["valid"], (inspected, verified)
            assert inspected["packageKind"] == verified["packageKind"] == "VOLUME"
            assert not inspected["payloadsVerified"] and verified["payloadsVerified"]
            assert len(verified["objects"]) == 2 and verified["relationshipCount"] == 1, verified
            assert verified["relationships"][0]["role"] == "SBNK_LEFT_MEMBER_TO_SMPL", verified
            if name in summaries:
                assert verified == summaries[name]
            summaries[name] = verified

        def check_plan(plan: Any, opened: Any, names: list[str], destination_names: list[str], valid: bool = True) -> None:
            expected_identity = identity(opened)
            assert plan["imageId"] == expected_identity["imageId"]
            assert plan["documentId"] == expected_identity["documentId"]
            assert plan["sourceMemberId"] == opened["documents"][0]["sourceMemberIds"][0]
            assert plan["revision"] == expected_identity["expectedRevision"]
            assert plan["schemaVersion"] == "1.0" and plan["targetKind"] == "SFS", plan
            for field in ("imageId", "documentId", "sourceMemberId", "planToken", "planId", "targetSnapshotId"):
                assert isinstance(plan[field], str) and plan[field], (field, plan)
            assert plan["expiresInSeconds"] > 0
            assert plan["valid"] is valid and plan["capacity"]["allowed"] is valid, plan
            for field in (
                "warnings", "conflicts", "actions", "opaqueSequences", "programAssignmentAdjustments",
                "programSlotPlacements", "allocation", "sfsIndexCapacity", "packages",
            ):
                assert isinstance(plan[field], list), (field, plan)
            assert len(plan["packages"]) == len(names), plan
            assert len(plan["actions"]) == 2 * len(names), plan
            for index, (name, destination_name) in enumerate(zip(names, destination_names, strict=True)):
                package = plan["packages"][index]
                assert package["packageIndex"] == index and package["packageId"] == summaries[name]["packageId"]
                assert package["destinationVolumeName"] == destination_name, package
                assert package["objectCount"] == 2 and package["payloadBytes"] == summaries[name]["totalPayloadBytes"]
                actions = [action for action in plan["actions"] if action["packageIndex"] == index]
                assert {action["nodeId"] for action in actions} == {obj["nodeId"] for obj in summaries[name]["objects"]}
                assert {action["objectType"] for action in actions} == {"SMPL", "SBNK"}
                assert all(action["volumeName"] == destination_name and action["actions"] for action in actions)
            if valid:
                assert not plan["conflicts"] and plan["allocation"] and plan["sfsIndexCapacity"], plan
            else:
                assert plan["conflicts"], plan
                for conflict in plan["conflicts"]:
                    assert conflict["code"] and conflict["message"] and conflict["volumeName"], conflict

        def plan_request(opened: Any, references: list[Any], destination: dict[str, Any]) -> dict[str, Any]:
            return {
                **identity(opened), "packages": references, "destination": destination,
                "capacityPolicy": {"target": "A4000_A5000"},
            }

        existing = {"kind": "EXISTING_VOLUME", "partitionIndex": 0, "volumeName": "Imported"}
        create = {"kind": "CREATE_VOLUME", "partitionIndex": 0, "volumeName": "Created"}
        hints = {"kind": "CREATE_VOLUMES_FROM_HINTS", "partitionIndex": 0, "volumeNameOverrides": []}
        opened = open_image("target.hds")
        before = snapshot(opened, "target.hds")
        sources = (
            ([files["alpha"]], ["alpha"]), ([uploads["alpha"]], ["alpha"]),
            ([files["alpha"], uploads["beta"]], ["alpha", "beta"]),
            ([uploads["alpha"], files["beta"]], ["alpha", "beta"]),
        )
        for destination in (existing, create, hints):
            for references, names in sources:
                print(f"Planning {destination['kind']} with {len(names)} package(s)", flush=True)
                plan = post(PLAN_ROUTE, plan_request(opened, references, destination))
                destination_names: list[str]
                if destination == hints:
                    destination_names = [summaries[name]["roots"][0]["displayName"] for name in names]
                else:
                    destination_name = destination["volumeName"]
                    assert isinstance(destination_name, str), destination
                    destination_names = [destination_name] * len(names)
                check_plan(plan, opened, names, destination_names)
                assert snapshot(opened, "target.hds") == before
                assert post(RELEASE_ROUTE, {"planToken": plan["planToken"]}) == {"released": True}
                assert snapshot(opened, "target.hds") == before

        conflict = post(PLAN_ROUTE, plan_request(opened, [files["alpha"], files["conflict"]], existing))
        check_plan(conflict, opened, ["alpha", "conflict"], ["Imported", "Imported"], valid=False)
        assert snapshot(opened, "target.hds") == before
        post(RELEASE_ROUTE, {"planToken": conflict["planToken"]})

        body = plan_request(opened, [uploads["alpha"]], existing)
        retained = post(PLAN_ROUTE, body)
        check_plan(retained, opened, ["alpha"], ["Imported"])
        token_body = {"planToken": retained["planToken"]}
        expect_error(PLAN_ROUTE, body, 404, "image_not_found", FOREIGN_TOKEN)
        expect_error(RELEASE_ROUTE, token_body, 404, "package_plan_not_found", FOREIGN_TOKEN)
        job(APPLY_ROUTE, token_body, FOREIGN_TOKEN, "package_plan_not_found")
        expect_error(PLAN_ROUTE, {**body, "expectedRevision": 2}, 409, "image_revision_stale")
        expect_error(
            PLAN_ROUTE, {**body, "packages": [uploads["beta"]], "replacePlanToken": retained["planToken"]},
            409, "package_plan_stale",
        )
        assert snapshot(opened, "target.hds") == before
        replacement = post(PLAN_ROUTE, {**body, "replacePlanToken": retained["planToken"], "destination": create})
        check_plan(replacement, opened, ["alpha"], ["Created"])
        assert replacement["planToken"] != retained["planToken"]
        job(APPLY_ROUTE, token_body, error="package_plan_not_found")
        post(RELEASE_ROUTE, {"planToken": replacement["planToken"]})
        job(APPLY_ROUTE, {"planToken": replacement["planToken"]}, error="package_plan_not_found")
        assert snapshot(opened, "target.hds") == before

        stale = post(PLAN_ROUTE, body)
        batch = post(PLAN_ROUTE, plan_request(opened, [files["alpha"], uploads["beta"]], existing))
        check_plan(batch, opened, ["alpha", "beta"], ["Imported", "Imported"])
        assert snapshot(opened, "target.hds") == before

        def apply_and_verify(plan: Any, session: Any, filename: str, pcm: list[bytes], route: str = APPLY_ROUTE) -> None:
            result = job(route, {"planToken": plan["planToken"]})
            assert result["applied"] and result["revision"] == plan["revision"] + 1, result
            for field in ("imageId", "documentId", "sourceMemberId", "planId", "targetSnapshotId", "targetKind"):
                assert result[field] == plan[field], (field, result, plan)
            after = snapshot(session, filename)
            assert after["image"]["revision"] == result["revision"]
            assert len(after["objects"]["items"]) == len(pcm) * 2, after
            objects = {obj["id"]: obj for obj in after["objects"]["items"]}
            edges = after["relationships"]["items"]
            assert len(edges) == len(pcm), edges
            for edge in edges:
                assert objects[edge["sourceObjectId"]]["type"] == "SBNK", edge
                assert objects[edge["targetObjectId"]]["type"] == "SMPL", edge
            verify_pcm(cli, workspace, filename, pcm)

        apply_and_verify(batch, opened, "target.hds", [expected_pcm["alpha"], expected_pcm["beta"]])
        after = snapshot(opened, "target.hds")
        assert after["sha256"] != before["sha256"]
        job(APPLY_ROUTE, {"planToken": stale["planToken"]}, error="image_revision_stale")
        assert snapshot(opened, "target.hds") == after
        post(RELEASE_ROUTE, {"planToken": stale["planToken"]})

        for filename, destination, references, names, destination_names in (
            ("create-target.hds", create, [uploads["alpha"]], ["alpha"], ["Created"]),
            ("hints-target.hds", hints, [uploads["alpha"], files["beta"]], ["alpha", "beta"], ["Source A", "Source B"]),
        ):
            session = open_image(filename)
            original = snapshot(session, filename)
            plan = post(PLAN_ROUTE, plan_request(session, references, destination))
            check_plan(plan, session, names, destination_names)
            assert snapshot(session, filename) == original
            apply_and_verify(plan, session, filename, [expected_pcm[name] for name in names])

        session = open_image("floppy-target.hds")
        original = snapshot(session, "floppy-target.hds")
        inspection = job("/api/v1/floppy-import-inspections", {"sources": [{"fileRef": {
            "rootId": "workspace", "relativePath": "source.ima",
        }}]})
        assert inspection["format"] == "A_SERIES" and inspection["complete"] and inspection["canImport"], inspection
        sample = next(obj for obj in inspection["objects"] if obj["objectType"] == "SBNK")
        floppy_plan = post("/api/v1/image-floppy-import-plans", {
            **identity(session), "inspectionToken": inspection["inspectionToken"],
            "selectedObjectKeys": [sample["objectKey"]], "acknowledgeSourceIssues": False,
            "destination": create, "capacityPolicy": {"target": "A4000_A5000"},
        })
        assert floppy_plan["valid"] and floppy_plan["capacity"]["allowed"], floppy_plan
        for field in ("imageId", "documentId", "sourceMemberId"):
            assert isinstance(floppy_plan[field], str) and floppy_plan[field], (field, floppy_plan)
        assert floppy_plan["imageId"] == session["imageId"]
        assert floppy_plan["documentId"] == session["documents"][0]["id"]
        assert floppy_plan["sourceMemberId"] == session["documents"][0]["sourceMemberIds"][0]
        assert floppy_plan["revision"] == session["revision"] and len(floppy_plan["actions"]) == 2
        post("/api/v1/floppy-import-inspections/release", {"inspectionToken": inspection["inspectionToken"]})
        assert snapshot(session, "floppy-target.hds") == original
        apply_and_verify(floppy_plan, session, "floppy-target.hds", [expected_pcm["alpha"]], "/api/v1/image-floppy-imports")

    assert process.poll() is not None
    with socket.socket() as connection:
        connection.settimeout(0.2)
        assert connection.connect_ex(("127.0.0.1", port)) != 0, "owned server port remained open"
    print("Package and floppy session import HTTP regressions passed", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--cli", type=Path, required=True)
    arguments = parser.parse_args()

    def terminate(signum: int, _frame: Any) -> None:
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGTERM, terminate)
    with tempfile.TemporaryDirectory(prefix="axklib-package-import-http-") as directory:
        exercise(arguments.server.resolve(), arguments.cli.resolve(), Path(directory))


if __name__ == "__main__":
    main()
