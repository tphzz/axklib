"""Use the three shipping clients against read-only source media; compare delivered bytes."""

import hashlib
import io
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
from urllib.parse import urlencode
import wave

from conversion_profile_test import file_ref, service
from reverse_wave_disk_test import PROFILE, catalog, check_disk, convert
from server_native_set_test import file_clients, run
from server_wave_selection_test import create_source, digest


def reopen_alone(server, root, folder):
    # This server has no workspace containing the A-series source.
    with service(server, root, folder, writable=False) as (get, job):
        for i, levels in enumerate([[-4096], [-4096, 4096]]):
            opened = job(
                "/api/v1/images",
                {"sources": [file_ref(f"download{i}.img")]},
                f"open{i}",
            )
            (doc,) = opened["documents"]
            assert doc["formatId"] == "yamaha.tx16w"
            prefix = "/api/v1/images/" + opened["imageId"]
            wanted = [f"TONE.W{j + 1:02}" for j in range(len(levels))]
            parent = {}
            for _ in range(3):
                page = get(
                    prefix
                    + "/filesystem?"
                    + urlencode(dict(expectedRevision=opened["revision"], **parent))
                )
                assert page["available"]
                if all(e["kind"] == "FILE" for e in page["items"]):
                    break
                (entry,) = page["items"]
                parent = {"parentId": entry["id"]}
            assert [e["name"] for e in page["items"]] == wanted
            rows = get(prefix + "/documents/" + doc["id"] + "/objects")["items"]
            assert [r["name"] for r in rows] == wanted
            for j, (row, level) in enumerate(zip(rows, levels, strict=True)):
                clip = job(
                    "/api/v1/auditions",
                    {
                        "imageId": opened["imageId"],
                        "documentId": doc["id"],
                        "expectedRevision": opened["revision"],
                        "objectIds": [row["id"]],
                        "readIntent": "PLAYBACK",
                    },
                    f"audition{i}-{j}",
                )
                wav = get(
                    "/api/v1/auditions/" + clip["auditionId"] + "/content", raw=True
                )
                with wave.open(io.BytesIO(wav), "rb") as audio:
                    assert (
                        audio.getnframes(),
                        audio.getframerate(),
                        audio.getnchannels(),
                        audio.getsampwidth(),
                    ) == (33333, 33333, 1, 2)
                    audio.setpos(7000)
                    assert struct.unpack("<2h", audio.readframes(2)) == (level, level)


def exercise(server, cli, installed, root):
    workspace = root / "workspace"
    workspace.mkdir()
    deliveries = root / "deliveries"
    deliveries.mkdir()
    source = create_source(cli, root, workspace, frames=44100)
    before = digest(source)
    env = dict(os.environ)
    env["PATH"] = str(installed / "install/bin") + os.pathsep + env["PATH"]

    def binary(name):
        (consumer,) = (installed / "build").rglob(
            name + (".exe" if os.name == "nt" else "")
        )
        return consumer

    sdk = binary("axk_convert_consumer")
    doc, rows, _ = file_clients(cli, binary("axk_set_consumer"), env, [source], 10)
    file_ids = {o["volumeName"]: o["id"] for o in rows}
    with service(server, root, workspace, writable=False) as (get, job):
        profiles = get("/api/v1/conversion-profiles")["profiles"]
        assert json.loads(run([sdk], env=env))["profiles"] == profiles
        assert (
            next(p for p in profiles if p["id"] == PROFILE)["outputKind"] == "NEW_IMAGE"
        )
        opened = job("/api/v1/images", {"sources": [file_ref(source.name)]}, "open")
        (http_doc,) = opened["documents"]
        prefix = "/api/v1/images/" + opened["imageId"]
        remote = get(prefix + "/documents/" + http_doc["id"] + "/objects")["items"]
        http_ids = {o["volumeName"]: o["id"] for o in remote}
        source_ref = {
            "kind": "DOCUMENT",
            "imageId": opened["imageId"],
            "documentId": http_doc["id"],
            "expectedRevision": opened["revision"],
        }
        inspected = []
        for offset in range(4):
            page = job(
                "/api/v1/conversion-inspections",
                {
                    "source": source_ref,
                    "objectType": "SMPL",
                    "offset": offset,
                    "limit": 1,
                },
                f"inspect{offset}",
            )
            assert page["sourceFormat"] == "yamaha.a-series"
            assert (
                page["documentId"] == http_doc["id"] and PROFILE in page["profileIds"]
            )
            assert (
                page["total"] == 3 and page["offset"] == offset and "planId" not in page
            )
            inspected.extend(page["items"])
        assert {row["volumeName"]: row["id"] for row in inspected} == http_ids
        assert (
            job(
                "/api/v1/conversion-inspections",
                {"source": {**source_ref, "expectedRevision": opened["revision"] + 1}},
                "inspect-stale",
                "FAILED",
            )["code"]
            == "image_revision_stale"
        )
        for i, volumes in enumerate([["TWO"], ["TWO", "ONE"]]):
            ids = [file_ids[v] for v in volumes]
            review = convert(cli, source, doc["id"], ids)
            assert not any(i.get("decision") for i in review["issues"])
            local = root / f"cli{i}.img"
            assert (
                convert(
                    cli,
                    source,
                    doc["id"],
                    ids,
                    "--output",
                    local,
                    confirmation="APPLY\n",
                )
                == review
            )
            # An existing destination is never replaced, even after confirmation.
            occupied = root / f"occupied{i}.img"
            occupied.write_bytes(b"keep")
            convert(
                cli,
                source,
                doc["id"],
                ids,
                "--output",
                occupied,
                ok=False,
                confirmation="APPLY\n",
            )
            assert occupied.read_bytes() == b"keep"
            output = root / f"sdk{i}.img"
            args = [
                sdk,
                PROFILE,
                source,
                json.dumps({"documentId": doc["id"], "waveIds": ids}),
            ]
            assert json.loads(run(args, env=env)) == review
            assert json.loads(run([*args, output], env=env)) == review
            remote_ids = [http_ids[v] for v in volumes]
            plans = "/api/v1/conversion-plans"
            plan = job(
                plans,
                {
                    "profileId": PROFILE,
                    "source": source_ref,
                    "destination": {"kind": "NEW_IMAGE"},
                    "fidelity": "BEST",
                    "options": {"documentId": http_doc["id"], "waveIds": remote_ids},
                },
                f"prepare{i}",
            )
            url = plans + "/" + plan["planId"]
            report = plan["report"]
            full = {"issues": [], "actions": []}
            for offset in range(max(report["issueCount"], report["actionCount"])):
                page = get(f"{url}?offset={offset}&limit=1")["report"]
                for key in full:
                    full[key].extend(page[key])
            assert [a["sourceObject"] for a in full["actions"]] == remote_ids
            for key in full:
                mapped = {http_ids[v]: file_ids[v] for v in volumes}
                assert [
                    {**obj, "sourceObject": mapped[obj["sourceObject"]]}
                    for obj in full[key]
                ] == review[key]
            done = job(
                url + "/executions",
                {"acknowledgedIssueIds": []},
                f"execute{i}",
            )
            blob = get(done["download"]["contentPath"], raw=True)
            assert blob == local.read_bytes() == output.read_bytes()
            assert (
                hashlib.sha256(blob).hexdigest()
                == report["destination"]["sha256"]
                == review["newImage"]["sha256"]
            )
            downloaded = deliveries / f"download{i}.img"
            downloaded.write_bytes(blob)
            _, reopened = catalog(cli, downloaded)
            assert [o["name"] for o in reopened] == [
                a["name"] for a in review["actions"]
            ]
            check_disk(blob, [-4096 if v == "TWO" else 4096 for v in volumes])
            print(
                json.dumps(
                    {
                        "volumes": volumes,
                        "reviewedAndDeliveredSha256": review["newImage"]["sha256"],
                        "names": [a["name"] for a in review["actions"]],
                    }
                )
            )
        assert get(prefix)["revision"] == opened["revision"]
    assert list(workspace.iterdir()) == [source] and digest(source) == before
    with service(server, root, workspace) as (get, job):
        opened = job(
            "/api/v1/images", {"sources": [file_ref(source.name)]}, "open-write-root"
        )
        (document,) = opened["documents"]
        reference = {
            "kind": "DOCUMENT",
            "imageId": opened["imageId"],
            "documentId": document["id"],
            "expectedRevision": opened["revision"],
        }
        page = job(
            "/api/v1/conversion-inspections",
            {"source": reference, "objectType": "SMPL"},
            "inventory",
        )
        wave_id = next(row["id"] for row in page["items"] if row["volumeName"] == "TWO")
        for occupied in (False, True):
            filename = "taken.img" if occupied else "converted.img"
            file = file_ref(filename)["file"]
            plan = job(
                "/api/v1/conversion-plans",
                {
                    "source": reference,
                    "profileId": PROFILE,
                    "destination": {"kind": "NEW_IMAGE", "file": file},
                    "options": {"documentId": document["id"], "waveIds": [wave_id]},
                    "fidelity": "BEST",
                },
                "workspace-" + filename,
            )
            target = workspace / filename
            assert not target.exists() and plan["report"]["destination"]["file"] == file
            execution = "/api/v1/conversion-plans/" + plan["planId"] + "/executions"
            if occupied:
                target.write_bytes(b"keep")
                job(execution, {"acknowledgedIssueIds": []}, "apply-occupied", "FAILED")
                assert target.read_bytes() == b"keep"
            else:
                assert job(execution, {"acknowledgedIssueIds": []}, "apply-output") == {
                    "planId": plan["planId"],
                    "file": file,
                }
                assert (
                    target.read_bytes() == (deliveries / "download0.img").read_bytes()
                )
        assert digest(source) == before
    reopen_alone(server, root / "fresh-server", deliveries)
    print("Unmodified A-series source SHA-256:", before)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="axk-reverse-delivery-") as tmp:
        exercise(*(Path(arg).resolve() for arg in sys.argv[1:]), Path(tmp))
