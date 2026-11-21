"""Check pitch, key ranges and playback modes in reopened Samples sharing a Typhoon Wave."""

import json
from pathlib import Path
import struct
import sys
import tempfile

from server_native_set_test import disk, run


def chunk(tag, data):
    return tag + struct.pack(">I", len(data)) + data + bytes(len(data) % 2)


def wave_file(instrument, looped, detune=0):
    version = struct.pack(">I", 1)
    info = chunk(b"VInf", bytes(12) + version)
    common = struct.pack(">HIH", 1, 64, 16) + bytes.fromhex(
        "400e ac44 0000 0000 0000 0000"
    )
    inst = b""
    if instrument is not None:
        base, low, high = instrument
        sustain = struct.pack(">HHH", 1, 1, 2) if looped else bytes(6)
        inst = chunk(
            b"INST",
            bytes((base, detune & 0xFF, low, high, 1, 127, 0, 0)) + sustain + bytes(6),
        )
        if looped:
            inst += chunk(b"MARK", struct.pack(">HHI2xHI2x", 2, 1, 8, 2, 55))
    return chunk(
        b"FORM",
        b"AIFF"
        + chunk(b"APPL", b"stoc\x07Typhoon" + info)
        + chunk(b"COMM", common)
        + inst
        + chunk(b"SSND", bytes(8 + 64 * 2)),
    )


def source_disk(instrument, looped, tunings=(), detune=0):
    wave = wave_file(instrument, looped, detune)
    version = struct.pack(">I", 1)
    info = chunk(b"VInf", bytes(12) + version)
    split = chunk(b"Splt", chunk(b"Wave", b"TONE".ljust(8, b"\0") + version + bytes(8)))
    voice = b"TYPV" + info
    groups = (
        (0, 47, 0, -2),
        (48, 59, 3, -1),
        (60, 127, 2, -2),
        (109, 127, 0, -2),
        (60, 60, 3, 0),
        (61, 61, 3, 1),
    )
    for index, (low, high, scaling, mode) in enumerate(groups):
        parm = bytearray(64)
        parm[:4] = bytes((low, high, 0, 127))
        if tunings:
            struct.pack_into("bb", parm, 4, *tunings[index])
        parm[6] = scaling
        parm[14] = mode & 0xFF
        voice += chunk(b"Grop", chunk(b"Parm", parm) + split)
    return disk([("TONE.A01", wave), ("KEYS.O01", chunk(b"FORM", voice))])


def exercise(
    cli,
    root,
    instrument,
    expected_fixed_tune,
    looped=False,
    *,
    tunings=(),
    detune=0,
    expected_pitch=None,
    expect_range_loss=False,
):
    source = root / "keys.img"
    source.write_bytes(source_disk(instrument, looped, tunings, detune))
    output = root / "keys.hds"
    args = [
        cli,
        "convert",
        "--profile",
        "nuedge.typhoon-to-a-series.sfs",
        "--source",
        source,
        "--options",
        json.dumps(
            {
                "mode": "HIERARCHY",
                "volumeName": "Keys",
                "sizeBytes": 8 * 1024**2,
                "partitionCount": 1,
                "partitionIndex": 0,
            }
        ),
    ]
    report = json.loads(run([*args, "--output", output], stdin="APPLY\n"))
    assert report["executable"], report["issues"]
    catalog = root / "objects"
    run([cli, "objects", output, "--object-type", "SBNK", "-o", catalog])
    with output.open("rb") as stream:
        samples, pitch = {}, {}
        for row in json.loads((catalog / "objects.json").read_bytes()):
            stream.seek(row["payload_offset"] + 0xD1)
            flags, coarse, root_key = struct.unpack(">B3xbB", stream.read(6))
            stream.seek(row["payload_offset"] + 0xDC)
            (fine,) = struct.unpack("b", stream.read(1))
            pitch[row["object_name"]] = (coarse, fine)
            stream.seek(row["payload_offset"] + 0xA8 + 0x3A)
            high, low, mode = struct.unpack(">BBxB", stream.read(4))
            if looped:
                stream.seek(row["payload_offset"] + 0xF8)
                assert struct.unpack(">I4xI", stream.read(12)) == (8, 47)
            samples[row["object_name"]] = (
                bool(flags & 0x10),
                root_key,
                low,
                high,
                mode,
            )
    stored_root = 60 if instrument is None else instrument[0]
    assert samples == {
        "TONE": (True, stored_root, 12, 59, 1 if looped else 0),
        "TONE 2": (False, stored_root, 60, 71, 1 if looped else 4),
        "TONE 3": (False, stored_root, 72, 120, 1 if looped else 0),
        "TONE 4": (False, stored_root, 72, 72, 1 if looped else 0),
        "TONE 5": (False, stored_root, 73, 73, 1 if looped else 4),
    }, (instrument, samples)
    names = ["TONE", "TONE 2", "TONE 3", "TONE 4", "TONE 5"]
    assert pitch == dict(
        zip(names, expected_pitch or [(expected_fixed_tune, 0)] + [(0, 0)] * 4)
    ), pitch
    for action in report["actions"]:
        if action["kind"] == "sample":
            details = {d["label"]: d["value"] for d in action["details"]}
            assert (int(details["Coarse Tune"]), int(details["Fine Tune"])) == pitch[
                action["name"]
            ]
    assert (
        any(
            i["code"] == "typhoon.pitch-range" and i["impact"] == "LOSS"
            for i in report["issues"]
        )
        == expect_range_loss
    )
    assert [
        i["impact"] for i in report["issues"] if i["code"] == "typhoon.key-scaling"
    ] == ["LOSS"]
    trigger_losses = [
        i for i in report["issues"] if i["code"] == "typhoon.trigger-mode"
    ]
    assert len(trigger_losses) == (3 if looped else 2), trigger_losses
    assert all(i["impact"] == "LOSS" for i in trigger_losses)


if __name__ == "__main__":
    for instrument, expected in (
        ((48, 0, 127), 12),
        ((60, 0, 127), 0),
        ((72, 0, 127), -12),
        ((61, 0, 127), -1),
        ((7, 0, 127), 48),
        ((125, 0, 127), -60),
        ((48, 48, 48), 0),
        (None, 0),
    ):
        with tempfile.TemporaryDirectory(prefix="axk-typhoon-scaling-") as tmp:
            exercise(Path(sys.argv[1]).resolve(), Path(tmp), instrument, expected)
    with tempfile.TemporaryDirectory(prefix="axk-typhoon-loop-") as tmp:
        exercise(Path(sys.argv[1]).resolve(), Path(tmp), (48, 0, 127), 12, looped=True)
    # All Groups share one Wave. The fourth Group is above the keyboard.
    tunings = [(12, 32), (-12, -32), (0, 0), (0, 0), (63, 43), (-64, -43)]
    for origin, detune, expected in (
        ((48, 0, 127), 0, [(24, 32), (-12, -32), (0, 0), (63, 43), (-64, -43)]),
        ((48, 0, 127), 32, [(24, 4), (-12, -60), (0, -28), (63, 15), (-64, -63)]),
        ((48, 48, 48), 32, [(12, 32), (-12, -32), (0, 0), (63, 43), (-64, -43)]),
        ((72, 0, 127), -32, [(0, 60), (-12, -4), (0, 28), (63, 63), (-64, -15)]),
    ):
        with tempfile.TemporaryDirectory(prefix="axk-typhoon-pitch-") as tmp:
            exercise(
                Path(sys.argv[1]).resolve(),
                Path(tmp),
                origin,
                0,
                tunings=tunings,
                detune=detune,
                expected_pitch=expected,
                expect_range_loss=origin[1] != origin[2] and detune != 0,
            )
    with tempfile.TemporaryDirectory(prefix="axk-typhoon-pitch-limits-") as tmp:
        exercise(
            Path(sys.argv[1]).resolve(),
            Path(tmp),
            (60, 0, 127),
            0,
            tunings=[(64, -43), (-65, 43), (0, 64), (0, 0), (0, -64), (120, 43)],
            expected_pitch=[(63, 42), (-64, -42), (1, -21), (-1, 21), (63, 63)],
            expect_range_loss=True,
        )
