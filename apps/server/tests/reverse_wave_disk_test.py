"""Review a known Wave, then inspect its FAT file and packed audio independently."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from server_native_set_test import run
from server_wave_selection_test import create_source, digest

PROFILE = "yamaha.a-series-to-tx16w.wave-disk"


def catalog(cli, source):
    info = [cli, "info", "--source-set", source]
    (doc,) = json.loads(run(info))["documents"]
    rows = json.loads(run([*info, "--document", doc["id"], "--collection", "objects"]))
    return doc["id"], rows["items"]


def convert(client, source, doc, ids, *extra, ok=True, confirmation=""):
    result = subprocess.run(
        [
            client,
            "convert",
            "--source",
            source,
            "--profile",
            PROFILE,
            "--options",
            json.dumps({"documentId": doc, "waveIds": ids}),
            *extra,
        ],
        capture_output=True,
        text=True,
        input=confirmation,
        timeout=30,
    )
    assert (result.returncode == 0) == ok and result.stdout.strip(), (
        result.stderr,
        result.stdout,
    )
    return json.loads(result.stdout)


def check_disk(blob, levels):
    assert len(blob) == 737280
    fat = blob[512:2048]
    assert fat == blob[2048:3584]
    root = blob[3584:7168]
    for index, level in enumerate(levels):
        entry = root[32 * index : 32 * (1 + index)]
        assert entry[:11] == f"TONE    W{index + 1:02}".encode()
        size = int.from_bytes(entry[28:32], "little")
        assert size == 50176
        cluster = int.from_bytes(entry[26:28], "little")
        chunks = []
        for _ in range(49):
            assert 2 <= cluster <= 714
            pos = 7168 + 1024 * (cluster - 2)
            chunks.append(blob[pos : pos + 1024])
            pos = cluster * 3 // 2
            cluster = (
                int.from_bytes(fat[pos : pos + 2], "little") >> (4 * (cluster % 2))
            ) & 0xFFF
        assert cluster >= 0xFF8
        wave = b"".join(chunks)
        assert wave[:6] == b"LM8953" and wave[22:24] == bytes([0xC9, 1])
        attack, repeat = [
            int.from_bytes(wave[i : i + 3], "little") & 0x1FFFF for i in (24, 27)
        ]
        assert attack + repeat == 33333 and wave[50033:] == bytes(143)
        for frame in (7000, 9001, 20000):
            pos = 32 + 3 * (frame // 2)
            a, middle, b = wave[pos : pos + 3]
            sample = (
                ((b << 4) | (middle & 15)) if frame % 2 else ((a << 4) | (middle >> 4))
            )
            signed = sample - 4096 if sample >= 2048 else sample
            assert 16 * signed == level, (frame, 16 * signed, level)
    assert root[32 * len(levels)] == 0


def exercise(cli, client, root):
    source = create_source(cli, root, root, frames=44100)
    before = digest(source)
    doc, rows = catalog(cli, source)
    waves = {r["volumeName"]: r["id"] for r in rows}
    for index, (ids, levels) in enumerate(
        (
            ([waves["TWO"]], [-4096]),
            ([waves["TWO"], waves["ONE"]], [-4096, 4096]),
        )
    ):
        output = root / f"wave{index}.img"
        blocked = convert(
            client, source, doc, ids, "--strict", "--output", output, ok=False
        )
        assert not blocked["executable"] and not output.exists()
        review = convert(client, source, doc, ids)
        assert review["executable"] and review["newImage"]["sizeBytes"] == 737280
        assert [a["sourceObject"] for a in review["actions"]] == ids
        for j, action in enumerate(review["actions"]):
            details = {d["label"]: d["value"] for d in action["details"]}
            assert action["name"] == f"TONE.W{j + 1:02}"
            assert details["Frames"] == "33333" and details["Data clusters"] == "49"
            assert (
                details["Wave file bytes"] == "50176"
                and details["Native bytes"] == "50033"
            )
        assert any(
            "44100" in i["message"] and "33333" in i["message"]
            for i in review["issues"]
        )
        loss = next(i["id"] for i in review["issues"] if i["impact"] == "LOSS")
        assert not any(i.get("decision") for i in review["issues"])
        convert(
            client,
            source,
            doc,
            ids,
            "--output",
            output,
            ok=False,
            confirmation=f"APPLY {loss}\n",
        )
        assert not output.exists()
        assert (
            convert(
                client, source, doc, ids, "--output", output, confirmation="APPLY\n"
            )
            == review
        )
        data = output.read_bytes()
        assert hashlib.sha256(data).hexdigest() == review["newImage"]["sha256"]
        check_disk(data, levels)
        _, native = catalog(cli, output)
        assert [(r["name"], r["type"]) for r in native] == [
            (a["name"], "wave") for a in review["actions"]
        ]
    assert before == digest(source)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="axk-reverse-waves-") as tmp:
        exercise(*(Path(arg).resolve() for arg in sys.argv[1:]), Path(tmp))
