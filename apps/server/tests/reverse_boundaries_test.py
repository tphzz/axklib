"""Exercise the limits of a real prepared disk, not just the FAT writer."""

import json
from pathlib import Path
import struct
import sys
import tempfile
import wave

from reverse_wave_disk_test import catalog, convert
from server_native_set_test import run


def source_disk(cli, root):
    specs = []

    def add(name, samples, **extra):
        with wave.open(str(root / (name + ".wav")), "wb") as f:
            f.setparams((1, 2, 44100, 0, "NONE", ""))
            f.writeframes(samples)
        specs.append(dict(id=name, name=name, path=name + ".wav", root_key=61, **extra))

    # Place the unpadded Wave comfortably inside each requested final cluster.
    for clusters in (356, 357, 358):
        wanted = (clusters * 1024 - 500) * 2 // 3
        count = round(wanted * 44100 / 33333)
        add("C" + str(clusters), struct.pack("<h", -4096) * count)
    for slot in range(65):
        add(f"S{slot:02}", struct.pack("<h", 16 + slot) * 256)
    # Positive attack, then negative loop all the way through its last frame.
    pcm = struct.pack("<h", 4096) * 22050 + struct.pack("<h", -8192) * 22050
    add("LOOP", pcm, loop_mode=1, loop_start_frame=22050, loop_length_frames=22050)
    add("TAIL", pcm, loop_mode=1, loop_start_frame=22050, loop_length_frames=1000)
    manifest = root / "manifest.json"
    manifest.write_text(
        json.dumps(
            {
                "schema_version": "1.0",
                "size_bytes": 8 * 1024 * 1024,
                "partitions": [
                    {
                        "name": "Disk",
                        "volumes": [
                            {"name": "Waves", "waveforms": specs, "samples": []}
                        ],
                    }
                ],
            }
        ),
        encoding="utf8",
    )
    disk = root / "waves.hds"
    run([cli, "create", "hds", manifest, "-o", disk])
    return disk


def exercise(cli, client, root):
    source = source_disk(cli, root)
    doc, rows = catalog(cli, source)
    ids = {r["name"]: r["id"] for r in rows}

    def batch(names, ok):
        args = [] if ok else ["--output", root / "blocked.img"]
        r = convert(client, source, doc, [ids[n] for n in names], *args, ok=ok)
        assert r["executable"] == ok and not (root / "blocked.img").exists()
        if not ok:
            assert not (r.get("newImage") or {}).get("sizeBytes")
        return r

    exact = batch(["C356", "C357"], True)
    assert [
        next(d["value"] for d in a["details"] if d["label"] == "Data clusters")
        for a in exact["actions"]
    ] == ["356", "357"]
    full = batch(["C356", "C358"], False)
    assert any("714" in i["message"] and "713" in i["message"] for i in full["issues"])
    small = [f"S{i:02}" for i in range(65)]
    assert batch(small[:64], True)["actions"][-1]["name"] == "S63.W64"
    assert any(
        "65" in i["message"] and "64" in i["message"]
        for i in batch(small, False)["issues"]
    )
    invalid = batch(["LOOP", "TAIL"], False)
    assert any(
        i["sourceObject"] == ids["TAIL"] and i["impact"] == "BLOCKER"
        for i in invalid["issues"]
    )
    loop = batch(["LOOP"], True)
    details = {d["label"]: d["value"] for d in loop["actions"][0]["details"]}
    assert [
        details[key]
        for key in ("Playback", "Sample rate", "Attack frames", "Repeat frames")
    ] == ["Forward loop", "50000", "25000", "25000"]
    out = root / "loop.img"
    convert(
        client,
        source,
        doc,
        [ids["LOOP"]],
        "--output",
        out,
        confirmation="APPLY\n",
    )
    # A fresh single-file disk starts data at sector 14, first cluster 2.
    blob = out.read_bytes()[7168:]
    assert blob[22:24] == bytes([0x49, 2])
    assert all(
        (int.from_bytes(blob[i : i + 3], "little") & 0x1FFFF) == 25000 for i in (24, 27)
    )
    for frame, sign in ((24999, 1), (25000, -1), (49999, -1)):
        i = 32 + 3 * (frame // 2)
        high = blob[i + (2 if frame % 2 else 0)]
        assert (high >= 128) == (sign < 0), frame


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="axk-reverse-boundaries-") as tmp:
        exercise(*(Path(arg).resolve() for arg in sys.argv[1:]), Path(tmp))
