"""Prove that registering a reader changes native discovery, not Files or its routes."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
from urllib.parse import quote, urlencode

from server_test_harness import (
    ServerProcess,
    choose_port,
    request,
    wait_for_job,
    write_workspace_store,
)

TOKEN = "0123456789abcdef0123456789abcdef"


def fixture_disk() -> bytes:
    image = bytearray(2880 * 512)
    image[:11] = b"\xeb\x3c\x90MSDOS5.0"
    struct.pack_into(
        "<HBHBHHBHHHII", image, 11, 512, 1, 1, 2, 224, 2880, 0xF0, 9, 18, 2, 0, 0
    )
    image[54:62] = b"FAT12   "
    image[510:512] = b"\x55\xaa"
    for fat in (512, 10 * 512):
        image[fat : fat + 5] = b"\xf0\xff\xff\xff\x0f"
    entry = 19 * 512
    image[entry : entry + 12] = b"KEYS    BIN\x20"
    struct.pack_into("<HI", image, entry + 26, 2, 8)
    image[33 * 512 : 33 * 512 + 8] = b"FX01Keys"
    return bytes(image)


def exercise(server: Path, root: Path, enabled: bool) -> tuple[str, str]:
    port = choose_port()
    config = root / "config.json"
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
    args = ["--config", str(config)]
    if enabled:
        args.append("--with-fixture-sampler")
    with ServerProcess(server, args, port, root / "server.log") as running:

        def get(path: str) -> dict:
            response = request(port, TOKEN, "GET", path)
            assert response.status == 200, response.content
            return response.json()["data"]

        result = request(
            port,
            TOKEN,
            "POST",
            "/api/v1/images",
            {
                "sources": [
                    {
                        "kind": "FILE",
                        "file": {"rootId": "workspace", "relativePath": "fixture.img"},
                    }
                ]
            },
            {"Idempotency-Key": "open"},
        )
        assert result.status == 202, result.content
        assert running.process is not None
        job = wait_for_job(port, TOKEN, result.json()["data"]["jobId"], running.process)
        assert job["state"] == "COMPLETED", job
        image = job["result"]["imageId"]
        current = get(f"/api/v1/images/{image}")
        assert current["documents"] == job["result"]["documents"]
        (source,) = current["sources"]
        (root_id,) = source["filesystemRootIds"]
        base = f"/api/v1/images/{image}"
        query = urlencode(
            {
                "rootId": root_id,
                "query": "KEYS.BIN",
                "expectedRevision": current["revision"],
            }
        )
        (entry,) = get(f"{base}/filesystem?{query}")["items"]
        assert entry["name"] == "KEYS.BIN" and entry["sizeBytes"] == 8, entry
        if enabled:
            (doc,) = current["documents"]
            assert (
                doc["formatId"] == "fixture.instrument" and doc["objectCount"] == 1
            ), doc
            assert doc["availableOperations"] == ["images.objects"], doc
            assert doc["sourceMemberIds"] == [source["id"]], doc
            (obj,) = get(
                f"{base}/documents/{quote(doc['id'], safe='')}/objects?limit=1"
            )["items"]
            assert (obj["name"], obj["type"]) == ("Keys", "tone"), obj
            (binding,) = entry["bindings"]
            assert (binding["documentId"], binding["objectId"]) == (
                doc["id"],
                obj["id"],
            ), binding
        else:
            assert current["documents"] == [] and entry["bindings"] == [], current
        return entry["rootId"], entry["id"]


def main() -> None:
    server = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="axk-format-reader-") as directory:
        root = Path(directory)
        workspace = root / "workspace"
        workspace.mkdir()
        disk = workspace / "fixture.img"
        original = fixture_disk()
        disk.write_bytes(original)
        identities = []
        for ordinal, enabled in enumerate((False, True, False)):
            state = root / str(ordinal)
            state.mkdir()
            write_workspace_store(state / "workspaces.json", workspace)
            identities.append(exercise(server, state, enabled))
            assert disk.read_bytes() == original
        assert len(set(identities)) == 1, identities


if __name__ == "__main__":
    main()
