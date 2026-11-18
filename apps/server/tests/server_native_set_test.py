"""Compare installed SDK, CLI and HTTP against physical records on an explicit disk set."""

from __future__ import annotations

import hashlib
from collections import Counter
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from urllib.parse import quote, urlencode

from server_format_reader_test import TOKEN, fixture_disk
from server_test_harness import (
    ServerProcess,
    choose_port,
    request,
    wait_for_job,
    write_workspace_store,
)


def native_bytes(size):
    raw = bytearray(size)
    raw[:6] = b"LM8953"
    return raw


def disk(files):
    raw = bytearray(fixture_disk())
    raw[19 * 512 :] = bytes(len(raw) - 19 * 512)
    cluster = 2
    for index, (name, data) in enumerate(files):
        entry = 19 * 512 + 32 * index
        base, extension = name.split(".")
        raw[entry : entry + 12] = base.encode().ljust(8) + extension.encode() + b"\x20"
        struct.pack_into("<HI", raw, entry + 26, cluster, len(data))
        for start in range(0, len(data), 512):
            part = data[start : start + 512]
            pos = (33 + cluster - 2) * 512
            raw[pos : pos + len(part)] = part
            successor = cluster + 1 if start + 512 < len(data) else 0xFFF
            for fat in (512, 10 * 512):
                pos = fat + cluster * 3 // 2
                (old,) = struct.unpack_from("<H", raw, pos)
                word = (
                    (old & 0xF) | (successor << 4)
                    if cluster % 2
                    else (old & 0xF000) | successor
                )
                struct.pack_into("<H", raw, pos, word)
            cluster += 1
    return raw


def make_pair(root):
    setup = native_bytes(1536)
    setup[0xF7:0xFB] = b"NEED"
    wave = native_bytes(35)
    wave[22:25] = bytes((0xC9, 2, 2))
    a = root / "one.img"
    b = root / "two.img"
    a.write_bytes(
        disk([("SAME.W02", wave), ("SET.S01", setup), ("SET.V01", native_bytes(8704))])
    )
    b.write_bytes(disk([("SAME.W02", wave), ("NEED.W01", wave)]))
    return a, b


def run(args, *, stdin="", env=None):
    result = subprocess.run(
        args, text=True, input=stdin, capture_output=True, timeout=20, env=env
    )
    assert result.returncode == 0, (args, result.stderr)
    return result.stdout


def check_private_inspector(server, seed, companion):
    unrelated = disk([("OTHER.W01", native_bytes(35))])
    blobs = [seed.read_bytes(), unrelated, companion.read_bytes()]
    manifest = {
        "selected": 1,
        "files": [
            {"name": name, "size": len(blob)}
            for name, blob in zip(["seed.img", "other.img", "companion.img"], blobs, strict=True)
        ],
    }
    header = json.dumps(manifest).encode() + b"\n"
    result = subprocess.run(
        [server, "--inspect-native-companions"],
        input=header + b"".join(blobs), timeout=15, capture_output=True, check=True,
    )
    assert json.loads(result.stdout) == {"adopted": [2], "ambiguous": []}, result.stdout
    # A truncated carrier cannot publish a partial decision.
    bad = subprocess.run(
        [server, "--inspect-native-companions"],
        input=header + blobs[0], timeout=15, capture_output=True,
    )
    assert bad.returncode != 0 and not bad.stdout


def assert_inventory(doc, objects, edges, complete):
    assert doc["formatId"] == "yamaha.tx16w" and doc["completeness"].lower() == (
        "complete" if complete else "incomplete"
    ), doc
    requirements = (
        doc["sourceRequirements"]
        if "sourceRequirements" in doc
        else doc["companions"]["requirements"]
    )
    assert [(r["name"], r["state"].lower()) for r in requirements] == (
        [] if complete else [("NEED.W01", "missing")]
    )
    records = {}
    for obj in objects:
        (loc,) = obj["locations"]
        assert loc["rootId"] == "fat-0" and loc["offset"] == 0
        records[obj["id"]] = (
            loc["memberId"],
            loc["entryId"],
            obj["type"],
            obj["name"],
            loc["length"],
        )
    expected = [
        ("source-1", "fat-0-f9728", "wave", "SAME.W02", 35),
        ("source-1", "fat-0-f9760", "setup", "SET", 1536),
    ]
    if complete:
        expected.extend(
            [
                ("source-2", "fat-0-f9728", "wave", "SAME.W02", 35),
                ("source-2", "fat-0-f9760", "wave", "NEED.W01", 35),
            ]
        )
    assert len(objects) == len(records) == doc["objectCount"] and sorted(
        records.values()
    ) == sorted(expected), records
    (edge,) = edges
    assert (
        edge["type"] == "wave-reference"
        and records[edge["sourceObjectId"]][2] == "setup"
    ), edge
    assert edge.get("resolution", edge.get("quality")) == (
        "resolved" if complete else "missing"
    )
    if complete:
        assert records[edge["targetObjectId"]] == expected[-1], edge
    else:
        assert edge["targetObjectId"] is None


def file_clients(cli, consumer, env, paths, limit):
    args = [cli, "info", "--source-set", *paths]
    summary = json.loads(run(args))
    assert len(summary["members"]) == len(paths)
    (doc,) = summary["documents"]
    commands = []
    rows = {}
    for collection, total in [
        ("objects", doc["objectCount"]),
        ("relationships", doc["relationshipCount"]),
    ]:
        rows[collection] = []
        for offset in [*range(0, total, limit), total]:
            commands.append(f"{collection} {doc['id']} {offset} {limit}")
            page = json.loads(
                run(
                    [
                        *args,
                        "--document",
                        doc["id"],
                        "--collection",
                        collection,
                        "--offset",
                        str(offset),
                        "--limit",
                        str(limit),
                    ]
                )
            )
            assert (
                page["total"] == total
                and page["offset"] == offset
                and len(page["items"]) == min(limit, total - offset)
            )
            rows[collection].extend(page["items"])
    lines = run(
        [consumer, *paths], stdin="\n".join(commands) + "\n", env=env
    ).splitlines()
    assert json.loads(lines[0]) == summary
    sdk_rows = {"objects": [], "relationships": []}
    for query, line in zip(commands, lines[1:], strict=True):
        collection, _, offset, _ = query.split()
        page = json.loads(line)
        assert page["offset"] == int(offset) and page["total"] == len(rows[collection])
        sdk_rows[collection].extend(page["items"])
    assert sdk_rows == rows
    return doc, rows["objects"], rows["relationships"]


def http_client(port, process, paths, limit):
    def get(url):
        reply = request(port, TOKEN, "GET", url)
        assert reply.status == 200, reply.content
        return reply.json()["data"]

    reply = request(
        port,
        TOKEN,
        "POST",
        "/api/v1/images",
        {
            "sources": [
                {
                    "kind": "FILE",
                    "file": {"rootId": "workspace", "relativePath": p.name},
                }
                for p in paths
            ]
        },
        {"Idempotency-Key": f"open-{len(paths)}"},
    )
    assert reply.status == 202, reply.content
    job = wait_for_job(port, TOKEN, reply.json()["data"]["jobId"], process)
    assert job["state"] == "COMPLETED", job
    base = "/api/v1/images/" + job["result"]["imageId"]
    state = get(base)
    assert len(state["sources"]) == len(paths)
    (doc,) = state["documents"]
    base += "/documents/" + quote(doc["id"], safe="")

    def pages(collection):
        cursor = None
        seen = set()
        rows = []
        while True:
            query = {"limit": limit}
            if cursor is not None:
                query["cursor"] = cursor
            page = get(f"{base}/{collection}?" + urlencode(query))
            rows.extend(page["items"])
            cursor = page.get("nextCursor")
            if not cursor:
                return rows
            assert cursor not in seen
            seen.add(cursor)

    objects = pages("objects")
    for obj in objects:
        detail = get(base + "/objects/" + quote(obj["id"], safe=""))
        obj["locations"] = detail["locations"]
    return doc, objects, pages("relationships")


def facts(doc, objects, edges):
    requirements = (
        doc["sourceRequirements"]
        if "sourceRequirements" in doc
        else doc["companions"]["requirements"]
    )
    return (
        doc["formatId"],
        doc["completeness"].lower(),
        sorted((r["name"], r["state"].lower(), r["objectId"]) for r in requirements),
        sorted(
            (
                o["id"],
                o["type"],
                o["name"],
                tuple(
                    (
                        loc["memberId"],
                        loc["rootId"],
                        loc["entryId"],
                        loc["offset"],
                        loc["length"],
                    )
                    for loc in o["locations"]
                ),
            )
            for o in objects
        ),
        sorted(
            (
                e["id"],
                e["type"],
                e["sourceObjectId"],
                e["targetObjectId"],
                e.get("resolution", e.get("quality")),
                tuple(e["candidateObjectIds"]),
            )
            for e in edges
        ),
    )


def check_nearby(port, process, seed, companion):
    def source(name):
        return {"kind": "FILE", "file": {"rootId": "workspace", "relativePath": name}}

    def opened(key):
        reply = request(
            port,
            TOKEN,
            "POST",
            "/api/v1/images",
            {"sources": [source(seed.name)]},
            {"Idempotency-Key": key},
        )
        assert reply.status == 202, reply.content
        job = wait_for_job(port, TOKEN, reply.json()["data"]["jobId"], process)
        assert job["state"] == "COMPLETED", job
        image = job["result"]
        return "/api/v1/images/" + image["imageId"], image["documents"][0]["id"]

    def attach(image, doc, selection):
        return request(
            port,
            TOKEN,
            "POST",
            f"{image}/documents/{doc}/companions",
            {"expectedRevision": 1, "selection": selection},
        )

    nearby = {"kind": "IMMEDIATE_SIBLINGS"}
    image, doc = opened("unique-nearby")
    unique = attach(image, doc, nearby)
    assert unique.status == 200, unique.content
    result = unique.json()["data"]
    assert result["revision"] == 2 and len(result["sources"]) == 2
    assert not result["documents"][0]["companions"]["canSearchNearby"]

    companion.with_name("also-matches.img").write_bytes(companion.read_bytes())
    image, doc = opened("ambiguous-nearby")
    ambiguous = attach(image, doc, nearby)
    assert ambiguous.json()["error"]["code"] == "companion_sources_ambiguous", (
        ambiguous.content
    )
    unchanged = request(port, TOKEN, "GET", image).json()["data"]
    assert unchanged["revision"] == 1 and len(unchanged["sources"]) == 1
    explicit = attach(
        image, doc, {"kind": "SOURCES", "sources": [source(companion.name)]}
    )
    assert explicit.status == 200, explicit.content
    assert explicit.json()["data"]["documents"][0]["completeness"] == "COMPLETE"


def main():
    server, cli, installed = [Path(arg).resolve() for arg in sys.argv[1:4]]
    external = [Path(arg).resolve() for arg in sys.argv[4:]]
    exe = "axk_set_consumer.exe" if os.name == "nt" else "axk_set_consumer"
    (consumer,) = (installed / "build").rglob(exe)
    env = dict(os.environ)
    env["PATH"] = str(installed / "install/bin") + os.pathsep + env["PATH"]
    with tempfile.TemporaryDirectory(prefix="axk-native-set-") as tmp:
        root = Path(tmp)
        if external:
            a, b = external
            assert a.parent == b.parent
            workspace = a.parent
        else:
            workspace = root / "workspace"
            workspace.mkdir()
            a, b = make_pair(workspace)
            check_private_inspector(server, a, b)
        hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in (a, b)]
        assert run([cli, "info", "--format", "summary", a, b]).count("objects=0") == 2
        write_workspace_store(root / "workspaces.json", workspace, writable=False)
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
            encoding="utf8",
        )
        with ServerProcess(
            server, ["--config", str(config)], port, root / "server.log"
        ) as running:
            for paths in ([a], [a, b]):
                limit = 100 if external else 1
                local = file_clients(cli, consumer, env, paths, limit)
                remote = http_client(port, running.process, paths, limit)
                assert facts(*local) == facts(*remote)
                if not external:
                    assert_inventory(*local, len(paths) == 2)
                else:
                    doc, objects, edges = local
                    print(
                        json.dumps(
                            {
                                "members": [str(p) for p in paths],
                                "sourceSha256": hashes[: len(paths)],
                                "document": doc["id"],
                                "completeness": doc["completeness"],
                                "objects": len(objects),
                                "types": dict(Counter(o["type"] for o in objects)),
                                "relationships": len(edges),
                                "performanceNames": dict(
                                    Counter(
                                        o["name"]
                                        for o in objects
                                        if o["type"] == "performance"
                                    )
                                ),
                            }
                        )
                    )
            if not external:
                check_nearby(port, running.process, a, b)
        assert hashes == [hashlib.sha256(p.read_bytes()).hexdigest() for p in (a, b)]


if __name__ == "__main__":
    main()
