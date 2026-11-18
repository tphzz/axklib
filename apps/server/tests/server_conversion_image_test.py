"""Publish one reviewed raw image from a read-only workspace, then recover its job."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from conversion_profile_test import TOKEN, file_ref, source_disk
from server_test_harness import (
    ServerProcess,
    choose_port,
    request,
    wait_for_job,
    write_workspace_store,
)


def file_delivery(cli, source, root, expected):
    args = [cli, "convert", "--profile", "fixture.new-image", "--source", source]

    def invoke(options, *extra, confirm="", ok=True):
        result = subprocess.run(
            [*args, "--options", json.dumps(options), *extra],
            input=confirm,
            text=True,
            capture_output=True,
            timeout=20,
        )
        assert (result.returncode == 0) == ok, (result.returncode, result.stderr)
        return json.loads(result.stdout)

    discovery = invoke({})
    (action,) = discovery["actions"]
    assert action["name"] == "TONE.W01"
    selection = {"wave": action["sourceObject"]}
    output = root / "file.img"
    review = invoke(selection)
    assert not output.exists() and review["newImage"] == {
        "sizeBytes": len(expected),
        "sha256": hashlib.sha256(expected).hexdigest(),
    }
    assert [(i["id"], i["impact"]) for i in review["issues"]] == [("L", "LOSS")]
    assert (
        invoke(selection, "--output", output, confirm="APPLY L\n", ok=False) == review
    )
    assert not output.exists()
    occupied = root / "occupied.img"
    occupied.write_bytes(b"keep me")
    assert (
        invoke(selection, "--output", occupied, confirm="APPLY\n", ok=False) == review
    )
    assert occupied.read_bytes() == b"keep me"
    blocked = invoke(
        selection, "--strict", "--output", output, confirm="APPLY\n", ok=False
    )
    assert not blocked["executable"] and not output.exists()
    assert invoke(selection, "--output", output, confirm="APPLY\n") == review
    assert output.read_bytes() == expected
    return review


def exercise(server, cli, root):
    workspace = root / "workspace"
    workspace.mkdir()
    source = source_disk()
    path = workspace / "source.img"
    path.write_bytes(source)
    expected = bytearray(source)
    expected[400] ^= 0x5A
    file_review = file_delivery(cli, path, root, expected)
    assert path.read_bytes() == source
    port = choose_port()
    other = "fedcba9876543210fedcba9876543210"
    write_workspace_store(root / "roots.json", workspace, writable=False)
    config = root / "config.json"
    config.write_text(
        json.dumps(
            {
                "bindAddress": "127.0.0.1",
                "port": port,
                "tokenHashes": [
                    {"principalId": p, "sha256": hashlib.sha256(t.encode()).hexdigest()}
                    for p, t in (("owner", TOKEN), ("other", other))
                ],
                "workspaceStore": str(root / "roots.json"),
                "stateDirectory": str(root / "state"),
            }
        ),
        encoding="utf-8",
    )
    with ServerProcess(
        server,
        ["--config", str(config), "--with-fixture-profile"],
        port,
        root / "server.log",
    ) as proc:

        def get(url):
            reply = request(port, TOKEN, "GET", url)
            assert reply.status == 200, reply.content
            return reply.json()["data"]

        def submit(url, body, key):
            return request(port, TOKEN, "POST", url, body, {"Idempotency-Key": key})

        def job(url, body, key, status="COMPLETED"):
            reply = submit(url, body, key)
            assert reply.status == 202, reply.content
            done = wait_for_job(
                port, TOKEN, reply.json()["data"]["jobId"], proc.process
            )
            assert done["state"] == status, done
            return done.get("result")

        profiles = get("/api/v1/conversion-profiles")["profiles"]
        assert (
            next(p for p in profiles if p["id"] == "fixture.new-image")["outputKind"]
            == "NEW_IMAGE"
        )
        opened = job("/api/v1/images", {"sources": [file_ref(path.name)]}, "open")
        (doc,) = opened["documents"]
        image = "/api/v1/images/" + opened["imageId"]
        rows = get(image + "/documents/" + doc["id"] + "/objects")["items"]
        wave = next(row for row in rows if row["type"] == "wave")
        body = {
            "profileId": "fixture.new-image",
            "fidelity": "BEST",
            "options": {"wave": wave["id"]},
            "source": {
                "kind": "DOCUMENT",
                "imageId": opened["imageId"],
                "documentId": doc["id"],
                "expectedRevision": 1,
            },
            "destination": {"kind": "NEW_IMAGE"},
        }
        plans = "/api/v1/conversion-plans"
        plan = job(plans, body, "prepare")
        url = plans + "/" + plan["planId"]
        review = plan["report"]
        assert review["executable"] and review["capacity"] is None
        assert review["destination"] == {
            "kind": "NEW_IMAGE",
            "sizeBytes": len(expected),
            "sha256": hashlib.sha256(expected).hexdigest(),
        }
        assert review["actions"][0]["sourceObject"] == wave["id"]
        (local_wave,) = file_review["actions"]
        assert wave["name"] == local_wave["name"] == "TONE.W01"
        for key in ("actions", "issues"):
            assert all(item["sourceObject"] == wave["id"] for item in review[key])
            assert [
                {**item, "sourceObject": local_wave["sourceObject"]}
                for item in review[key]
            ] == file_review[key]
        assert review["destination"]["sha256"] == file_review["newImage"]["sha256"]
        assert (
            get(url + "?offset=1&limit=1")["report"]["destination"]
            == review["destination"]
        )
        assert path.read_bytes() == source and list(workspace.iterdir()) == [path]
        assert not list((root / "state").rglob("*.download"))
        execute = url + "/executions"
        assert submit(
            execute, {"acknowledgedIssueIds": ["L"]}, "gratuitous-ack"
        ).status in (
            400,
            409,
            422,
        )
        assert get(url)["state"] == "READY"
        assert submit(execute, {"acknowledgedIssueIds": []}, "execute").status == 202
        # Deliberately discard the submission response: follow the retained plan instead.
        job_id = get(url)["jobId"]
        done = wait_for_job(port, TOKEN, job_id, proc.process)
        assert done["state"] == "COMPLETED", done
        artifact = done["result"]["download"]
        assert "revision" not in done["result"] and artifact["sizeBytes"] == len(
            expected
        )
        raw = request(port, TOKEN, "GET", artifact["contentPath"])
        assert raw.status == 200 and raw.content == expected
        assert raw.headers["content-type"] == "application/x-raw-disk-image"
        assert len(list((root / "state").rglob("*.download"))) == 1
        assert request(port, other, "GET", artifact["contentPath"]).status in (403, 404)
        assert (
            get(url)["jobId"] == job_id
            and get("/api/v1/jobs/" + job_id)["result"]["download"] == artifact
        )
        assert get(image)["revision"] == 1 and path.read_bytes() == source
        strict = job(plans, {**body, "fidelity": "STRICT"}, "strict")
        assert not strict["report"]["executable"]
        assert submit(
            plans + "/" + strict["planId"] + "/executions",
            {"acknowledgedIssueIds": []},
            "cannot-ack-blocker",
        ).status in (400, 409, 422)
        assert len(list((root / "state").rglob("*.download"))) == 1
        job(
            plans,
            {**body, "capacityPolicy": {"target": "A3000"}},
            "wrong-policy",
            "FAILED",
        )
        stale = job(plans, body, "before-source-change")
        path.write_bytes(bytes(len(source)))
        job(
            plans + "/" + stale["planId"] + "/executions",
            {"acknowledgedIssueIds": []},
            "changed-source",
            "FAILED",
        )
        assert len(list((root / "state").rglob("*.download"))) == 1
        assert path.read_bytes() == bytes(len(source))


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="axk-conversion-delivery-") as tmp:
        exercise(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), Path(tmp))
