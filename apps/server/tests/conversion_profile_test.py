"""Compare forward conversions with the pre-refactor file and HTTP results."""

import argparse
import hashlib
import json
from contextlib import contextmanager
from pathlib import Path
import shutil
import struct
import tempfile
import wave

from server_native_set_test import disk, native_bytes, run
from server_test_harness import (
    ServerProcess,
    choose_port,
    request,
    wait_for_job,
    write_workspace_store,
)

TOKEN = "0123456789abcdef0123456789abcdef"
FORWARD = "yamaha.tx16w-to-a-series"
FIXTURE = "fixture.inspect-wave"
SELECTION = {"kind": "EXISTING_VOLUME", "partitionIndex": 0, "volumeName": "New Volume"}


def source_disk(*, with_performance=True):
    setup, voices, performances = (native_bytes(n) for n in (1536, 8704, 5120))
    setup[0xF7:0xFB] = b"TONE"
    voices[0x96:0x9B] = b"Voice"
    voices[0x11:0x13] = bytes((36, 96))
    for i in range(1, 32):
        voices[0x11 + 4 * i : 0x13 + 4 * i] = b"\xff\xff"
    voices[0x1251] = 76
    voices[0x127E:0x1284] = b"Timbre"
    performances[0x88:0x93] = b"Performance"
    wave = native_bytes(32)
    wave[22:25] = bytes((0x49, 2, 58))
    wave[27] = 6
    wave.extend(bytes.fromhex("7ff080123eff") * 16)
    files = [("TONE.W01", wave), ("SET.S01", setup), ("SET.V01", voices)]
    if with_performance:
        files.append(("SET.U01", performances))
    return disk(files)


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def review_digest(value):
    return digest(json.dumps(value, sort_keys=True, separators=(",", ":")).encode())


def acknowledgements(report):
    return [x["id"] for x in report["issues"] if x.get("decision")]


def catalog_signature(rows):
    # Occurrence IDs are local to their source binding; compare the graph they identify.
    indices = {r["id"]: index for index, r in enumerate(rows)}
    assert len(indices) == len(rows)
    return [
        (r["kind"], r["name"], r["selectable"], [indices[d] for d in r["dependencies"]])
        for r in rows
    ]


def file_ref(name):
    return {"kind": "FILE", "file": {"rootId": "workspace", "relativePath": name}}


@contextmanager
def service(server, root, workspace, flags=(), *, writable=True):
    port = choose_port()
    config = root / "config.json"
    write_workspace_store(root / "workspaces.json", workspace, writable=writable)
    config.write_text(
        json.dumps(
            {
                "bindAddress": "127.0.0.1",
                "port": port,
                "tokenHashes": [
                    {"principalId": "test", "sha256": digest(TOKEN.encode())}
                ],
                "workspaceStore": str(root / "workspaces.json"),
                "stateDirectory": str(root / "state"),
            }
        ),
        encoding="utf-8",
    )
    with ServerProcess(
        server, ["--config", str(config), *flags], port, root / "server.log"
    ) as running:

        def get(path, *, raw=False):
            result = request(port, TOKEN, "GET", path)
            assert result.status == 200, result.content
            return result.content if raw else result.json()["data"]

        def job(path, body, key, expected_state="COMPLETED", *, expected_status=202):
            result = request(port, TOKEN, "POST", path, body, {"Idempotency-Key": key})
            assert result.status == expected_status, result.content
            if expected_status != 202:
                return result.json()["error"]
            done = wait_for_job(
                port, TOKEN, result.json()["data"]["jobId"], running.process
            )
            assert done["state"] == expected_state, done
            return (
                done.get("result") if expected_state == "COMPLETED" else done["error"]
            )

        yield get, job


def conversion_body(job, profile, options):
    opened = job("/api/v1/images", {"sources": [file_ref("target.hds")]}, "open-target")
    (doc,) = opened["documents"]
    return {
        "profileId": profile,
        "source": {"kind": "SOURCES", "sources": [file_ref("source.img")]},
        "destination": {
            "imageId": opened["imageId"],
            "documentId": doc["id"],
            "expectedRevision": 1,
            "selection": SELECTION,
        },
        "options": options,
        "fidelity": "BEST",
    }


def forward(server, cli, fixture, root, mode):
    workspace = root / "workspace"
    workspace.mkdir(parents=True)
    source, target = workspace / "source.img", workspace / "target.hds"
    source.write_bytes(source_disk())
    shutil.copyfile(fixture, target)
    output = root / "local.hds"
    options = {"mode": mode}
    args = [
        cli,
        "convert",
        "--profile",
        FORWARD,
        "--source",
        source,
        "--destination",
        target,
        "--selection",
        json.dumps(SELECTION),
        "--options",
        json.dumps(options),
    ]
    report = json.loads(run(args))
    confirm = " ".join(["APPLY", *acknowledgements(report)]) + "\n"
    assert json.loads(run([*args, "--output", output], stdin=confirm)) == report
    assert target.read_bytes() == fixture.read_bytes()
    with service(server, root, workspace) as (get, job):
        listing = get("/api/v1/conversion-profiles")["profiles"]
        assert not {FIXTURE, "fixture.new-image"}.intersection(p["id"] for p in listing)
        descriptor = next(p for p in listing if p["id"] == FORWARD)
        # Compare the original descriptor independently from the later output and selection controls.
        assert descriptor.pop("outputKind") == "EXISTING_IMAGE"
        selection = descriptor["optionsSchema"]["properties"].pop("sourceObjectIds")
        assert selection["type"] == "array" and selection["items"]["type"] == "string"
        assert "sourceObjectIds" not in descriptor["optionsSchema"]["required"]
        body = conversion_body(job, FORWARD, options)
        plan = job("/api/v1/conversion-plans", body, "plan")
        path = "/api/v1/conversion-plans/" + plan["planId"]
        first = plan["report"]
        full = {key: [] for key in ("actions", "issues")}
        sources = []
        for offset in range(
            0, max(first["actionCount"], first["issueCount"], first["sourceCount"]), 3
        ):
            page = get(f"{path}?offset={offset}&limit=3")["report"]
            for key, rows in full.items():
                rows.extend(page[key])
            assert len(page["sourceObjects"]) <= 3
            sources.extend(page["sourceObjects"])
        sdk_catalog = report.pop("sourceObjects")
        assert (
            len(sources)
            == len(sdk_catalog)
            == first["sourceCount"]
            == report.pop("sourceCount")
        )
        assert catalog_signature(sources) == catalog_signature(sdk_catalog)
        root_kind = "wave" if mode == "WAVE_DATA_ONLY" else "performance"
        assert [r["kind"] for r in sources if r["selectable"]] == [root_kind]
        assert all(r["location"] for r in [*sources, *sdk_catalog])

        # Exercise both bindings with the occurrence from their own report.
        def selected(rows):
            return {
                **options,
                "sourceObjectIds": [r["id"] for r in rows if r["selectable"]],
            }

        local_subset = json.loads(run([*args[:-1], json.dumps(selected(sdk_catalog))]))
        remote_subset = job(
            "/api/v1/conversion-plans",
            {**body, "options": selected(sources)},
            "plan-selection",
        )["report"]
        assert local_subset["executable"] and remote_subset["executable"]
        assert local_subset["actions"] == report["actions"]
        assert remote_subset["actions"] == full["actions"]
        assert [len(full[k]) for k in full] == [
            first["actionCount"],
            first["issueCount"],
        ]
        assert first["executable"] and report["executable"]
        applied = job(
            path + "/executions",
            {"acknowledgedIssueIds": acknowledgements(full)},
            "apply",
        )
        assert applied["applied"] and applied["revision"] == 2, applied
    assert source.read_bytes() == source_disk()
    assert target.read_bytes() == output.read_bytes()
    tree = run([cli, "info", output])
    assert "TONE" in tree
    if mode == "HIERARCHY":
        assert (
            "001: Performa [PROGRAM (PROG)]" in tree
            and "B Voice [SAMPLE BANK (SBAC)] - Rch Assign: A01" in tree
            and "Timbre [SAMPLE (SBNK)]" in tree
        ), tree
    run([cli, "extract", "wav", "file", output, "-o", root / "audio"])
    (tone,) = (root / "audio").rglob("TONE*.wav")
    with wave.open(str(tone)) as wav:
        assert (
            wav.getnchannels(),
            wav.getsampwidth(),
            wav.getframerate(),
            wav.getnframes(),
        ) == (1, 2, 48000, 65)
        assert (
            digest(wav.readframes(65))
            == "b361091390a921888bcb8b4e04165ab1fe70e286fc0be908d8a6cf39d6a927e7"
        )
    raw = tone.read_bytes()
    pos = 12
    while pos + 8 <= len(raw):
        chunk, size = struct.unpack_from("<4sI", raw, pos)
        if chunk == b"smpl":
            assert struct.unpack_from("<II", raw, pos + 8 + 44) == (56, 60)
            break
        pos += 8 + size + (size % 2)
    else:
        raise AssertionError("Reopened Wave lost its loop")
    return (
        review_digest(descriptor),
        review_digest(report),
        review_digest(full),
        digest(output.read_bytes()),
    )


def voice_mapping_decision(server, cli, fixture, root):
    workspace = root / "workspace"
    workspace.mkdir(parents=True)
    source, target = workspace / "source.img", workspace / "target.hds"
    source.write_bytes(source_disk(with_performance=False))
    shutil.copyfile(fixture, target)
    options = {"mode": "HIERARCHY"}
    args = [
        cli,
        "convert",
        "--profile",
        FORWARD,
        "--source",
        source,
        "--destination",
        target,
        "--selection",
        json.dumps(SELECTION),
        "--options",
        json.dumps(options),
    ]
    local = json.loads(run(args))
    output = root / "voice.hds"
    run(
        [*args, "--output", output],
        stdin="APPLY " + " ".join(acknowledgements(local)) + "\n",
    )
    with service(server, root, workspace) as (_, job):
        body = conversion_body(job, FORWARD, options)
        plan = job("/api/v1/conversion-plans", body, "plan")
        for report in (local, plan["report"]):
            assert report["executable"]
            (decision,) = [i for i in report["issues"] if i.get("decision")]
            assert decision["code"] == "tx16w.missing_performance"
            assert decision["impact"] == "UNCERTAINTY"
            assert any(i["impact"] == "LOSS" for i in report["issues"])
        path = "/api/v1/conversion-plans/" + plan["planId"] + "/executions"
        job(path, {"acknowledgedIssueIds": []}, "missing-choice", expected_status=409)
        result = job(
            path,
            {"acknowledgedIssueIds": acknowledgements(plan["report"])},
            "apply-choice",
        )
        assert result["applied"]
    assert target.read_bytes() == output.read_bytes()


def registration(server, file_client, fixture, root):
    workspace = root / "workspace"
    workspace.mkdir(parents=True)
    source, target = workspace / "source.img", workspace / "target.hds"
    source.write_bytes(source_disk())
    shutil.copyfile(fixture, target)
    for index, enabled in enumerate((False, True, False)):
        state = root / str(index)
        state.mkdir()
        with service(
            server, state, workspace, ["--with-fixture-profile"] if enabled else []
        ) as (get, job):
            profiles = get("/api/v1/conversion-profiles")["profiles"]
            assert FORWARD in [p["id"] for p in profiles]
            matches = [p for p in profiles if p["id"] == FIXTURE]
            assert len(matches) == int(enabled)
            body = conversion_body(job, FIXTURE, {})
            for label in ("First review", "Other review"):
                local = json.loads(
                    run(
                        [file_client, "on" if enabled else "off", source, target, label]
                    )
                )
                assert local["profiles"] == profiles
                body["options"] = {"reviewLabel": label}
                result = job(
                    "/api/v1/conversion-plans",
                    body,
                    label.replace(" ", "-"),
                    "COMPLETED" if enabled else "FAILED",
                )
                if enabled:
                    assert matches[0]["optionsSchema"]["required"] == ["reviewLabel"]
                    for report in (result["report"], local["report"]):
                        assert (
                            report["executable"] and report["profileId"] == FIXTURE
                        ), report
                        (action,) = report["actions"]
                        assert (action["action"], action["name"]) == (
                            "inspect",
                            label,
                        ) and action["sourceObject"]
                else:
                    assert (
                        result["code"] == "invalid_request"
                        and "Unknown conversion profile" in result["message"]
                    ), result
                    assert local["error"] == "Unknown conversion profile"
    assert (
        target.read_bytes() == fixture.read_bytes()
        and source.read_bytes() == source_disk()
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for name in ("server", "cli", "fixture"):
        parser.add_argument(name, type=Path)
    parser.add_argument(
        "--registration", metavar=("SERVER", "FILE_CLIENT"), nargs=2, type=Path
    )
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="axk-conversion-profile-") as directory:
        descriptor = "ef906f7b6af3853ed6cdb308102a7b2325b2dd6e66bce9498276e8f16adae7cd"
        for mode, expected in {
            "WAVE_DATA_ONLY": (
                "2074e186c4e10228b13fd8935ba2743ecda788ab70bb8c35e115d3e0918c50e9",
                "85812015eac18b930822a76cc09824f8b96c0280a522a2cb6159275ed4df4c5c",
                "7f9759fd2a2aad854e4eba177d5700b6e1b787eed49478ab3bc26cfe6611f153",
            ),
            "HIERARCHY": (
                "5bc877842da1d41a57d6b54ae053e2f1caf49b73e47bb636da20114947200c19",
                "3054913fca8c5a4ad3d65160c3fef53effce213b8ce9fa265e7b46e7a3f53dc6",
                "7642699908caa87f3df017c46c494609925e37ac4a5c7fcf8fe194e1551b15c9",
            ),
        }.items():
            observed = forward(
                args.server.resolve(),
                args.cli.resolve(),
                args.fixture.resolve(),
                Path(directory) / mode,
                mode,
            )
            assert observed == (descriptor, *expected), (mode, observed)
        voice_mapping_decision(
            args.server.resolve(),
            args.cli.resolve(),
            args.fixture.resolve(),
            Path(directory) / "voice-decision",
        )
        if args.registration:
            test_server, file_client = args.registration
            registration(
                test_server.resolve(),
                file_client.resolve(),
                args.fixture.resolve(),
                Path(directory) / "registration",
            )
