"""A real over-capacity source, an atomic CLI directory, and the same reviewed server TAR."""

import io
import json
import sys
import tarfile
import tempfile
from pathlib import Path

from conversion_profile_test import file_ref, service
from reverse_wave_disk_test import PROFILE, catalog, convert
from server_wave_selection_test import create_source, digest


def exercise(server, cli, root):
    workspace = root / "workspace"
    workspace.mkdir()
    source = create_source(cli, root, workspace, frames=225000)
    original = digest(source)
    document, rows = catalog(cli, source)
    selection = ["TWO", "ONE", "THREE"]

    def select(items):
        by_volume = {r["volumeName"]: r["id"] for r in items}
        return [by_volume[name] for name in selection]

    ids = select(rows)
    assert not convert(cli, source, document, ids, ok=False)["executable"]
    out = root / "disks"
    profile = PROFILE + "-set"
    review = convert(cli, source, document, ids, profile=profile)
    members = review["newImageSet"]["members"]
    assert len(members) == 2 and review["executable"] and not out.exists()
    convert(
        cli,
        source,
        document,
        ids,
        "--output",
        out,
        profile=profile,
        confirmation="APPLY\n",
    )
    manifest = json.loads((out / "manifest.json").read_bytes())
    assert manifest == {"profileId": profile, "members": members}
    expected = {"manifest.json", *(member["filename"] for member in members)}
    assert {f.name for f in out.iterdir()} == expected
    actual_names = []
    for member in members:
        disk = out / member["filename"]
        assert disk.stat().st_size == member["sizeBytes"] == 737280
        assert digest(disk) == member["sha256"]
        _, objects = catalog(cli, disk)
        actual_names.extend(o["name"] for o in objects)
    assert actual_names == [a["name"] for a in review["actions"]]
    # POSIX rename() can overwrite even an existing empty directory. It must not here.
    occupied = root / "occupied"
    occupied.mkdir()
    convert(
        cli,
        source,
        document,
        ids,
        "--output",
        occupied,
        profile=profile,
        ok=False,
        confirmation="APPLY\n",
    )
    assert occupied.is_dir() and not list(occupied.iterdir())
    assert not list(root.glob(".axk-images-*"))
    with service(server, root, workspace, writable=False) as (get, job):
        obj = job("/api/v1/images", {"sources": [file_ref(source.name)]}, "open")
        (doc,) = obj["documents"]
        ref = {
            "kind": "DOCUMENT",
            "imageId": obj["imageId"],
            "documentId": doc["id"],
            "expectedRevision": obj["revision"],
        }
        rows = get(
            "/api/v1/images/" + obj["imageId"] + "/documents/" + doc["id"] + "/objects"
        )["items"]
        plan = job(
            "/api/v1/conversion-plans",
            {
                "profileId": profile,
                "source": ref,
                "destination": {"kind": "NEW_IMAGE_SET"},
                "fidelity": "BEST",
                "options": {"documentId": doc["id"], "waveIds": select(rows)},
            },
            "prepare",
        )
        assert plan["report"]["destination"] == {
            "kind": "NEW_IMAGE_SET",
            "members": members,
        }
        done = job(
            "/api/v1/conversion-plans/" + plan["planId"] + "/executions",
            {"acknowledgedIssueIds": []},
            "apply",
        )
        content = get(done["download"]["contentPath"], raw=True)
        with tarfile.open(fileobj=io.BytesIO(content), mode="r:") as archive:
            assert set(archive.getnames()) == expected
            assert len(archive.getmembers()) == len(expected)
            for name in expected:
                assert archive.extractfile(name).read() == (out / name).read_bytes()
    assert list(workspace.iterdir()) == [source] and digest(source) == original


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="axk-image-set-") as tmp:
        exercise(*(Path(arg).resolve() for arg in sys.argv[1:]), Path(tmp))
