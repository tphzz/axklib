"""Convert Groups sharing one Wave and read the actual A-series Sample Level bytes."""

import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from typhoon_key_scaling_test import chunk, disk, run, wave_file


def source(raws):
    version = struct.pack(">I", 1)
    voice = b"TYPV" + chunk(b"VInf", bytes(12) + version)
    wave_ref = chunk(b"Wave", b"TONE".ljust(8, b"\0") + version + bytes(8))
    for i, raw in enumerate(raws):
        parm = bytearray(64)
        parm[:4] = bytes((2 * i, 2 * i + 1, 0, 127))
        parm[7] = raw
        parm[14] = 254
        # Two splits of the same Group must both inherit its volume.
        second = chunk(b"Parm", bytes((2 * i + 1, 0))) + wave_ref
        voice += chunk(
            b"Grop",
            chunk(b"Parm", parm) + chunk(b"Splt", wave_ref) + chunk(b"Splt", second),
        )
    return disk(
        [
            ("TONE.A01", wave_file((60, 0, 127), False)),
            ("LEVELS.O01", chunk(b"FORM", voice)),
        ]
    )


def exercise(cli, root):
    image, hds = root / "levels.img", root / "converted.hds"
    cmd = [
        cli,
        "convert",
        "--profile",
        "nuedge.typhoon-to-a-series.sfs",
        "--source",
        image,
        "--options",
    ]
    options = dict(
        volumeName="Levels", sizeBytes=8 * 1024**2, partitionCount=1, partitionIndex=0
    )
    hierarchy = [*cmd, json.dumps(options | {"mode": "HIERARCHY"})]
    cases = [
        (0, 127),
        (1, 122),
        (2, 122),
        (32, 64),
        (64, 32),
        (96, 16),
        (123, 9),
        (124, 9),
        (126, 8),
        (127, 2),
    ]
    image.write_bytes(source([raw for raw, _ in cases]))
    report = json.loads(run([*hierarchy, "--output", hds], stdin="APPLY\n"))
    assert report["executable"]
    objects = root / "objects"
    run([cli, "objects", hds, "-o", objects])
    catalog = json.loads((objects / "objects.json").read_bytes())
    reads = {}
    with hds.open("rb") as f:
        for obj in catalog:
            if obj["object_type"] != "SBNK":
                continue
            f.seek(obj["payload_offset"] + 0xA8 + 0x3B)
            key = f.read(1)[0]
            f.seek(obj["payload_offset"] + 0xA8 + 0x6E)
            reads[key] = f.read(1)[0]
    expected = {
        12 + 2 * i + j: level for i, (_, level) in enumerate(cases) for j in (0, 1)
    }
    assert reads == expected, (reads, expected)
    assert sum(r["object_type"] == "SMPL" for r in catalog) == 1
    for action in report["actions"]:
        if action["kind"] == "sample":
            details = {d["label"]: d["value"] for d in action["details"]}
            assert (
                int(details["Sample Level"])
                == reads[int(details["Key range"].split("..")[0])]
            )
    assert (
        sum(i["code"] == "typhoon.group-volume-approximation" for i in report["issues"])
        == 3
    )
    for invalid in (128, 255):
        image.write_bytes(source([invalid]))
        result = subprocess.run(hierarchy, capture_output=True, timeout=30)
        assert result.returncode == 2, result.stderr
        blocked = json.loads(result.stdout)
        assert not blocked["executable"] and any(
            i["code"] == "typhoon.instrument-layout" for i in blocked["issues"]
        )
        waves = json.loads(
            run([*cmd, json.dumps(options | {"mode": "WAVE_DATA_ONLY"})])
        )
        assert waves["executable"] and [a["kind"] for a in waves["actions"]] == [
            "wave-data"
        ]


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="typhoon-level-") as tmp:
        exercise(Path(sys.argv[1]).resolve(), Path(tmp))
