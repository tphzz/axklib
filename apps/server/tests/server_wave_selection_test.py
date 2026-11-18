"""Read the selected duplicate Wave from a large, read-only A-series disk."""

import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import wave

from conversion_profile_test import TOKEN, file_ref
from server_native_set_test import run
from server_test_harness import (
    ServerProcess,
    choose_port,
    request,
    wait_for_job,
    write_workspace_store,
)


def create_source(cli, root, workspace, *, with_sample=False, frames=4096):
    volumes = []
    for volume, name, level in (
        ("ONE", "TONE", 4096),
        ("TWO", "TONE", -4096),
        ("THREE", "UNSELECTED", 32),
    ):
        samples = [level] * frames
        samples[511] = 2 * level
        filename = root / (volume + ".wav")
        with wave.open(str(filename), "wb") as wav:
            wav.setparams((1, 2, 44100, 0, "NONE", "not compressed"))
            wav.writeframes(struct.pack(f"<{frames}h", *samples))
        volumes.append(
            {
                "name": volume,
                "waveforms": [
                    {
                        "id": "wave-" + volume,
                        "name": name,
                        "path": filename.name,
                        "root_key": 60,
                    }
                ],
                "samples": [{"name": "Not a Wave", "waveform_id": "wave-THREE"}]
                if with_sample and volume == "THREE"
                else [],
            }
        )
    size = 72 * 1024 * 1024
    manifest = root / "source.json"
    manifest.write_text(
        json.dumps(
            {
                "schema_version": "1.0",
                "size_bytes": size,
                "partitions": [{"name": "Disk", "volumes": volumes}],
            }
        ),
        encoding="utf-8",
    )
    path = workspace / "source.hds"
    run([cli, "create", "hds", manifest, "-o", path])
    assert path.stat().st_size == size
    return path


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def file_catalog(cli, root):
    source = create_source(cli, root, root)
    before = digest(source)
    info = [cli, "info", "--source-set", source]
    (doc,) = json.loads(run(info))["documents"]
    assert doc["formatId"] == "yamaha.a-series" and doc["completeness"] == "complete", (
        doc
    )
    query = [*info, "--document", doc["id"], "--collection", "objects"]
    rows = []
    for offset in range(3):
        page = json.loads(run([*query, "--offset", str(offset), "--limit", "1"]))
        assert page["total"] == 3, page
        rows.extend(page["items"])
    assert len(rows) == len({r["id"] for r in rows}) == 3
    assert {(r["volumeName"], r["name"], r["type"]) for r in rows} == {
        ("ONE", "TONE", "SMPL"),
        ("TWO", "TONE", "SMPL"),
        ("THREE", "UNSELECTED", "SMPL"),
    }, rows
    locations = [r["locations"][0] for r in rows]
    assert all(
        loc["memberId"] == "source-1" and loc["length"] >= 8192 for loc in locations
    )
    assert len({(loc["rootId"], loc["entryId"]) for loc in locations}) == 3
    assert before == digest(source)


def exercise(server, cli, root):
    workspace = root / "workspace"
    workspace.mkdir()
    source = create_source(cli, root, workspace)
    before = digest(source)
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
        server, ["--config", str(config)], port, root / "server.log"
    ) as proc:

        def job(url, body, key):
            submitted = request(
                port, TOKEN, "POST", url, body, {"Idempotency-Key": key}
            )
            assert submitted.status == 202, submitted.content
            done = wait_for_job(
                port, TOKEN, submitted.json()["data"]["jobId"], proc.process
            )
            assert done["state"] == "COMPLETED", done
            return done["result"]

        opened = job("/api/v1/images", {"sources": [file_ref(source.name)]}, "open")
        (doc,) = opened["documents"]
        assert doc["formatId"] == "yamaha.a-series"
        image = "/api/v1/images/" + opened["imageId"]
        rows = request(
            port, TOKEN, "GET", image + "/documents/" + doc["id"] + "/objects?type=SMPL"
        ).json()["data"]
        waves = {(r["volumeName"], r["name"]): r["id"] for r in rows["items"]}
        assert set(waves) == {
            ("ONE", "TONE"),
            ("TWO", "TONE"),
            ("THREE", "UNSELECTED"),
        }, waves
        assert len(set(waves.values())) == rows["totalCount"] == 3
        for volume, expected in (
            ("TWO", (-4096, -8192, -4096)),
            ("ONE", (4096, 8192, 4096)),
        ):
            selected = waves[volume, "TONE"]
            audition = job(
                "/api/v1/auditions",
                {
                    "imageId": opened["imageId"],
                    "documentId": doc["id"],
                    "expectedRevision": opened["revision"],
                    "objectIds": [selected],
                    "readIntent": "PLAYBACK",
                },
                "read-" + volume,
            )
            (clip,) = audition["clips"]
            assert clip["objectId"] == selected
            url = "/api/v1/auditions/" + audition["auditionId"]
            header = request(
                port, TOKEN, "GET", url + "/content", headers={"Range": "bytes=0-43"}
            )
            assert header.status == 206 and header.content[36:40] == b"data", (
                header.content
            )
            assert struct.unpack_from("<HHI", header.content, 20) == (1, 1, 44100)
            assert struct.unpack_from("<H", header.content, 34) == (16,)
            first = 44 + 2 * 510
            part = request(
                port,
                TOKEN,
                "GET",
                url + "/content",
                headers={"Range": f"bytes={first}-{first + 5}"},
            )
            assert (
                part.status == 206 and struct.unpack("<3h", part.content) == expected
            ), part.content
            assert request(port, TOKEN, "DELETE", url).status in (200, 204)
        assert (
            request(port, TOKEN, "GET", image).json()["data"]["revision"]
            == opened["revision"]
        )
    assert before == digest(source)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="axk-wave-selection-") as directory:
        if sys.argv[1] == "--file":
            file_catalog(Path(sys.argv[2]).resolve(), Path(directory))
        else:
            exercise(
                Path(sys.argv[1]).resolve(),
                Path(sys.argv[2]).resolve(),
                Path(directory),
            )
