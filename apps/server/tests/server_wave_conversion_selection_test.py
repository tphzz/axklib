"""Prepare from catalog IDs, not audition IDs guessed from native names."""

import hashlib
import json
import mmap
import os
from pathlib import Path
import subprocess
import struct
import shutil
import sys
import tempfile

from conversion_profile_test import TOKEN, file_ref
from server_native_set_test import file_clients, run
from server_test_harness import (
    ServerProcess,
    choose_port,
    request,
    wait_for_job,
    write_workspace_store,
)
from server_wave_selection_test import create_source, digest


PROFILE = "fixture.a-series-window"


def selection_cases(prepare, doc, objects):
    waves = {r["volumeName"]: r["id"] for r in objects if r["type"] == "SMPL"}
    other = next(r["id"] for r in objects if r["type"] == "SBNK")
    for first, count, levels in [
        (0, 1, [4096]),
        (510, 3, [4096, 8192, 4096]),
        (4095, 1, [4096]),
    ]:
        ids = [waves["TWO"], waves["ONE"]]
        report = prepare(doc, ids, first, count, True)
        assert [a["sourceObject"] for a in report["actions"]] == ids
        assert [json.loads(a["details"][0]["value"]) for a in report["actions"]] == [
            [-n for n in levels],
            levels,
        ], report
    for document, ids, first in [
        (doc, [], 0),
        (doc, [waves["ONE"], waves["ONE"]], 0),
        (doc, [waves["ONE"], "missing"], 0),
        (doc, [other], 0),
        ("foreign-document", [waves["TWO"]], 0),
        (doc, [waves["TWO"]], 4096),
    ]:
        prepare(document, ids, first, 1, False)


def exercise(server, cli, file_client, installed, root):
    workspace = root / "workspace"
    workspace.mkdir()
    source = create_source(cli, root, workspace, with_sample=True)
    before = digest(source)
    # Locate the authored PCM by signed bytes independently of either SFS reader.
    with (
        source.open("rb") as stream,
        mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data,
    ):
        sentinels = []
        for level in [4096, -4096, 32]:
            triple = struct.pack(">3h", level, 2 * level, level)
            pos = data.find(triple)
            assert pos != -1 and data.find(triple, pos + 1) == -1
            sentinels.append(pos)
    measured = json.loads(run([file_client, "measure-sfs", source]))
    assert measured["frames"] == [-4096, -8192, -4096]
    assert measured["selectedReads"] == [[sentinels[1], 6]], measured["selectedReads"]
    assert not any(
        start < pos + 6 and start + size > pos
        for start, size in measured["metadataReads"]
        for pos in sentinels
    )
    exe = "axk_set_consumer.exe" if os.name == "nt" else "axk_set_consumer"
    (consumer,) = (installed / "build").rglob(exe)
    env = dict(os.environ)
    env["PATH"] = str(installed / "install/bin") + os.pathsep + env["PATH"]
    file_doc, rows, _ = file_clients(cli, consumer, env, [source], 10)

    def local(document, ids, first, count, ok):
        options = dict(documentId=document, waveIds=ids, first=first, count=count)
        result = subprocess.run(
            [
                file_client,
                "convert",
                "--source",
                source,
                "--profile",
                PROFILE,
                "--options",
                json.dumps(options),
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
        assert (result.returncode == 0) == ok, (result.stderr, result.stdout)
        return json.loads(result.stdout) if ok else None

    selection_cases(local, file_doc["id"], rows)
    other = workspace / "other.hds"
    # Even a byte-identical file has its own occurrences. No disk-sized test allocation.
    shutil.copyfile(source, other)
    info = [cli, "info", "--source-set", other]
    (foreign_doc,) = json.loads(run(info))["documents"]
    foreign_objects = json.loads(
        run([*info, "--document", foreign_doc["id"], "--collection", "objects"])
    )["items"]
    foreign_id = next(row["id"] for row in foreign_objects if row["type"] == "SMPL")
    local(file_doc["id"], [foreign_id], 0, 1, False)
    port = choose_port()
    write_workspace_store(root / "roots.json", workspace, writable=False)
    config = root / "config.json"
    config.write_text(
        json.dumps(
            {
                "bindAddress": "127.0.0.1",
                "port": port,
                "tokenHashes": [
                    {
                        "principalId": "reader",
                        "sha256": hashlib.sha256(TOKEN.encode()).hexdigest(),
                    }
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
        serial = 0

        def job(url, body, ok=True):
            nonlocal serial
            serial += 1
            reply = request(
                port, TOKEN, "POST", url, body, {"Idempotency-Key": f"window-{serial}"}
            )
            assert reply.status == 202, reply.content
            done = wait_for_job(
                port, TOKEN, reply.json()["data"]["jobId"], proc.process
            )
            assert done["state"] == ("COMPLETED" if ok else "FAILED"), done
            return done.get("result")

        def opened(path):
            image = job("/api/v1/images", {"sources": [file_ref(path.name)]})
            (doc,) = image["documents"]
            rows = request(
                port,
                TOKEN,
                "GET",
                "/api/v1/images/"
                + image["imageId"]
                + "/documents/"
                + doc["id"]
                + "/objects",
            ).json()["data"]["items"]
            return image, doc, rows

        image, doc, http_rows = opened(source)

        def http(document, ids, first, count, ok, *, revision=image["revision"]):
            result = job(
                "/api/v1/conversion-plans",
                {
                    "profileId": PROFILE,
                    "fidelity": "BEST",
                    "source": {
                        "kind": "DOCUMENT",
                        "imageId": image["imageId"],
                        "documentId": doc["id"],
                        "expectedRevision": revision,
                    },
                    "destination": {"kind": "NEW_IMAGE"},
                    "options": dict(
                        documentId=document, waveIds=ids, first=first, count=count
                    ),
                },
                ok,
            )
            if result:
                status = request(
                    port,
                    TOKEN,
                    "DELETE",
                    "/api/v1/conversion-plans/" + result["planId"],
                ).status
                assert status in (200, 204), status
                return result["report"]
            return None

        selection_cases(http, doc["id"], http_rows)
        _, _, foreign = opened(other)
        http(
            doc["id"],
            [next(row["id"] for row in foreign if row["type"] == "SMPL")],
            0,
            1,
            False,
        )
        http(
            doc["id"],
            [next(r["id"] for r in http_rows if r["type"] == "SMPL")],
            0,
            1,
            False,
            revision=image["revision"] + 1,
        )
        assert (
            request(port, TOKEN, "GET", "/api/v1/images/" + image["imageId"]).json()[
                "data"
            ]["revision"]
            == image["revision"]
        )
    assert before == digest(source)
    # A stale file selection cannot be reused even when the record numbers still line up.
    with source.open("r+b") as stream:
        stream.seek(-1, 2)
        original = stream.read(1)
        stream.seek(-1, 2)
        stream.write(bytes([original[0] ^ 1]))
    local(
        file_doc["id"],
        [next(r["id"] for r in rows if r["type"] == "SMPL")],
        0,
        1,
        False,
    )


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="axk-conversion-selection-") as temp:
        exercise(*(Path(arg).resolve() for arg in sys.argv[1:]), Path(temp))
