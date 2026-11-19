"""A real Yamaha source creates standalone A-series media without a destination session."""

import argparse
import hashlib
from pathlib import Path
import tempfile

from conversion_profile_test import (
    FORWARD,
    acknowledgements,
    file_ref,
    service,
    source_disk,
)


def exercise(server, root, typhoon=None):
    workspace = root / "workspace"
    workspace.mkdir()
    seed = typhoon.read_bytes() if typhoon else source_disk()
    source_format = "nuedge.typhoon" if typhoon else "yamaha.tx16w"
    direction = "nuedge.typhoon-to-a-series" if typhoon else FORWARD
    (workspace / "source.img").write_bytes(seed)
    with service(server, root, workspace) as (get, job):
        source = {"kind": "SOURCES", "sources": [file_ref("source.img")]}
        catalog = job(
            "/api/v1/conversion-inspections",
            {"source": source, "sourceFormat": source_format},
            "inspect",
        )
        for route, extension in (("floppy", "img"), ("iso", "iso"), ("sfs", "hds")):
            profile = f"{direction}.{route}"
            assert profile in catalog["profileIds"]
            options = {"mode": "HIERARCHY", "volumeName": "Imported"}
            if typhoon:
                options["sourceObjectIds"] = [
                    next(
                        row["id"]
                        for row in catalog["items"]
                        if row["type"] == "performance"
                    )
                ]
            if route == "sfs":
                options.update(
                    sizeBytes=8 * 1024 * 1024, partitionCount=2, partitionIndex=1
                )
            plan = job(
                "/api/v1/conversion-plans",
                {
                    "profileId": profile,
                    "source": source,
                    "options": options,
                    "destination": {"kind": "NEW_IMAGE"},
                    "fidelity": "BEST",
                },
                "prepare-" + route,
            )
            report = plan["report"]
            assert report["executable"], report["issues"]
            result = job(
                "/api/v1/conversion-plans/" + plan["planId"] + "/executions",
                {"acknowledgedIssueIds": acknowledgements(report)},
                "execute-" + route,
            )
            download = result["download"]
            assert download["filename"] == "conversion." + extension, download
            data = get(download["contentPath"], raw=True)
            assert hashlib.sha256(data).hexdigest() == report["destination"]["sha256"]
            filename = "reopened." + extension
            (workspace / filename).write_bytes(data)
            opened = job(
                "/api/v1/images", {"sources": [file_ref(filename)]}, "open-" + route
            )
            (doc,) = opened["documents"]
            assert doc["formatId"] == "yamaha.a-series", doc
            objects = get(
                f"/api/v1/images/{opened['imageId']}/documents/{doc['id']}/objects"
            )["items"]
            assert {"SMPL", "PROG"}.issubset({obj["type"] for obj in objects}), objects
        assert (workspace / "source.img").read_bytes() == seed


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server", type=Path)
    parser.add_argument(
        "--typhoon",
        type=Path,
        help="Optional standalone native Typhoon control disk (read-only)",
    )
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="axk-fresh-media-") as tmp:
        exercise(args.server.resolve(), Path(tmp), args.typhoon)
