from __future__ import annotations

import argparse
import base64
import hashlib
import http.client
import io
import json
import os
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import tarfile
import time
import zipfile
from pathlib import Path
from typing import Any
from urllib.parse import urlencode

from server_test_harness import write_workspace_store


TOKEN = "0123456789abcdef0123456789abcdef"
SUBPROTOCOL = "axklib.events.v1"


def choose_port() -> int:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def raw_http_request(
    port: int,
    method: str,
    path: str,
    payload: bytes | str | None = None,
    headers: dict[str, str] | None = None,
) -> tuple[int, bytes, dict[str, str]]:
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    request_headers = {"Authorization": f"Bearer {TOKEN}"}
    if headers is not None:
        request_headers.update(headers)
    connection.request(method, path, body=payload, headers=request_headers)
    response = connection.getresponse()
    content = response.read()
    status = response.status
    response_headers = {name.lower(): value for name, value in response.getheaders()}
    connection.close()
    return status, content, response_headers


def http_request(
    port: int,
    method: str,
    path: str,
    body: dict[str, Any] | None = None,
    headers: dict[str, str] | None = None,
) -> tuple[int, Any]:
    payload = None if body is None else json.dumps(body)
    request_headers = {} if headers is None else dict(headers)
    if payload is not None:
        request_headers["Content-Type"] = "application/json"
    status, content, _ = raw_http_request(port, method, path, payload, request_headers)
    return status, json.loads(content) if content else None


def read_http_headers(connection: socket.socket) -> tuple[bytes, bytearray]:
    response = bytearray()
    while b"\r\n\r\n" not in response:
        chunk = connection.recv(4096)
        if not chunk:
            raise AssertionError("connection closed before HTTP headers")
        response.extend(chunk)
    boundary = response.index(b"\r\n\r\n") + 4
    return bytes(response[:boundary]), response[boundary:]


def websocket_handshake(
    port: int, ticket: str
) -> tuple[socket.socket, bytes, bytearray]:
    connection = socket.create_connection(("127.0.0.1", port), timeout=3)
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    request = (
        f"GET /api/v1/events?ticket={ticket} HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        f"Sec-WebSocket-Protocol: {SUBPROTOCOL}\r\n\r\n"
    )
    connection.sendall(request.encode("ascii"))
    headers, remaining = read_http_headers(connection)
    return connection, headers, remaining


def receive_frame(connection: socket.socket, buffered: bytearray) -> tuple[int, bytes]:
    def take(count: int) -> bytes:
        while len(buffered) < count:
            chunk = connection.recv(4096)
            if not chunk:
                raise AssertionError("WebSocket closed before a complete frame")
            buffered.extend(chunk)
        result = bytes(buffered[:count])
        del buffered[:count]
        return result

    header = take(2)
    opcode = header[0] & 0x0F
    length = header[1] & 0x7F
    if length == 126:
        length = struct.unpack("!H", take(2))[0]
    elif length == 127:
        length = struct.unpack("!Q", take(8))[0]
    assert header[1] & 0x80 == 0, "server frames must not be masked"
    return opcode, take(length)


def send_client_frame(connection: socket.socket, opcode: int, payload: bytes) -> None:
    mask = os.urandom(4)
    masked = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
    if len(payload) < 126:
        length = bytes([0x80 | len(payload)])
    elif len(payload) <= 0xFFFF:
        length = bytes([0x80 | 126]) + struct.pack("!H", len(payload))
    else:
        length = bytes([0x80 | 127]) + struct.pack("!Q", len(payload))
    connection.sendall(bytes([0x80 | opcode]) + length + mask + masked)


def send_text(connection: socket.socket, message: str) -> None:
    send_client_frame(connection, 1, message.encode("utf-8"))


def wait_until_ready(port: int, process: subprocess.Popen[bytes]) -> None:
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(
                f"server exited during startup with {process.returncode}"
            )
        try:
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=0.2)
            connection.request("GET", "/api/v1/system/health/live")
            response = connection.getresponse()
            response.read()
            connection.close()
            if response.status == 204:
                return
        except OSError:
            pass
        time.sleep(0.02)
    raise AssertionError("server did not become ready")


def wait_for_connection_file(
    path: Path, process: subprocess.Popen[bytes]
) -> dict[str, Any]:
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"server exited with {process.returncode}")
        if path.exists():
            return json.loads(path.read_text(encoding="utf-8"))
        time.sleep(0.02)
    raise AssertionError("server did not publish its connection file")


def wait_for_job(
    port: int, job_id: str, process: subprocess.Popen[bytes]
) -> dict[str, Any]:
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(
                f"server exited while job {job_id} was running with {process.returncode}"
            )
        status, snapshot = http_request(port, "GET", f"/api/v1/jobs/{job_id}")
        assert status == 200, snapshot
        job = snapshot["data"]
        if job["state"] in {"COMPLETED", "FAILED", "CANCELLED"}:
            return job
        time.sleep(0.01)
    raise AssertionError(f"job {job_id} did not reach a terminal state")


def canonical_info_node(node: dict[str, Any]) -> dict[str, Any]:
    def value(camel: str, snake: str) -> Any:
        return node[camel] if camel in node else node[snake]

    return {
        "nodeId": value("nodeId", "node_id"),
        "nodeType": value("nodeType", "node_type"),
        "displayName": value("displayName", "display_name"),
        "objectKey": value("objectKey", "object_key"),
        "objectType": value("objectType", "object_type"),
        "count": node["count"],
        "selectorPath": value("selectorPath", "selector_path"),
        "children": [canonical_info_node(child) for child in node["children"]],
    }


def artifact_hashes(root: Path) -> dict[str, str]:
    return {
        path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


def verify_extraction_result(root: Path, result: dict[str, Any]) -> None:
    artifacts = result["artifacts"]
    assert result["artifactCount"] == result["writtenFileCount"] == len(artifacts), (
        result
    )
    assert result["selectionGraphCount"] >= 1, result
    assert result["decodeErrorCount"] >= 0 and result["loadErrorCount"] >= 0, result
    serialized = json.dumps(result)
    assert str(root) not in serialized, serialized
    for artifact in artifacts:
        relative = Path(artifact["relativePath"])
        assert not relative.is_absolute() and ".." not in relative.parts, artifact
        file_ref = artifact["fileRef"]
        assert file_ref["rootId"] == "workspace", artifact
        destination = Path(result["destination"]["relativePath"])
        assert Path(file_ref["relativePath"]) == destination / relative, artifact
        path = root / file_ref["relativePath"]
        content = path.read_bytes()
        assert artifact["sizeBytes"] == len(content), artifact
        assert artifact["sha256"] == hashlib.sha256(content).hexdigest(), artifact
        assert artifact["owners"], artifact
        for owner in artifact["owners"]:
            assert owner["source"]["rootId"] == "workspace", owner
            source_path = Path(owner["source"]["relativePath"])
            assert not source_path.is_absolute() and ".." not in source_path.parts, (
                owner
            )
            assert owner["objectType"] in {"VOLUME", "SMPL", "SBNK", "SBAC"}, owner
            assert owner["objectName"], owner


def canonical_package_summary(value: dict[str, Any]) -> dict[str, Any]:
    def field(camel: str, snake: str) -> Any:
        return value[camel] if camel in value else value[snake]

    root_kind = {"PROGRAM": "prog"}
    source_kind = {
        "FAT12_FLOPPY": "fat12-floppy",
        "STANDALONE_OBJECT": "standalone-object",
    }
    return {
        "packageId": field("packageId", "package_id"),
        "packageKind": str(field("packageKind", "package_kind")).lower(),
        "requiredExtension": field("requiredExtension", "required_extension"),
        "sourceMediaKind": source_kind.get(
            str(field("sourceMediaKind", "source_media_kind")),
            str(field("sourceMediaKind", "source_media_kind")).lower(),
        ),
        "valid": value["valid"],
        "payloadsVerified": field("payloadsVerified", "payloads_verified"),
        "roots": [
            {
                "kind": root_kind.get(str(root["kind"]), str(root["kind"]).lower()),
                "displayName": root.get("displayName", root.get("display_name")),
                "nodeIds": root.get("nodeIds", root.get("node_ids")),
            }
            for root in value["roots"]
        ],
        "objects": [
            {
                "nodeId": item.get("nodeId", item.get("node_id")),
                "objectType": item.get("objectType", item.get("object_type")),
                "name": item["name"],
                "payloadSha256": item.get("payloadSha256", item.get("payload_sha256")),
                "normalizedSha256": item.get(
                    "normalizedSha256", item.get("normalized_sha256")
                ),
                "semanticSha256": item.get(
                    "semanticSha256", item.get("semantic_sha256")
                ),
                "audioSha256": item.get("audioSha256", item.get("audio_sha256")),
            }
            for item in value["objects"]
        ],
        "relationshipCount": field("relationshipCount", "relationship_count"),
        "issues": value["issues"],
    }


def canonical_package_plan(value: dict[str, Any]) -> dict[str, Any]:
    def normalized_item(item: dict[str, Any]) -> dict[str, Any]:
        return {
            key.replace("_", "").lower(): (
                [str(entry).lower() for entry in item[key]]
                if key in {"actions"}
                else item[key]
            )
            for key in sorted(item)
        }

    actions = value.get("actions", value.get("objects", []))
    return {
        "planId": value.get("planId", value.get("plan_id")),
        "targetKind": str(value.get("targetKind", value.get("target_kind")))
        .lower()
        .replace("_", "-"),
        "targetSnapshotId": value.get(
            "targetSnapshotId", value.get("target_snapshot_id")
        ),
        "valid": value["valid"],
        "warnings": value["warnings"],
        "conflicts": [normalized_item(item) for item in value["conflicts"]],
        "actions": [normalized_item(item) for item in actions],
        "allocation": [normalized_item(item) for item in value["allocation"]],
    }


def canonical_alteration_operation(value: dict[str, Any]) -> dict[str, Any]:
    def field(camel: str, snake: str) -> Any:
        return value[camel] if camel in value else value[snake]

    raw_audio = value.get("audioImport", value.get("audio_import"))
    audio = None
    if raw_audio is not None:
        audio = {
            "sourcePath": field_from(raw_audio, "sourcePath", "source_path"),
            "sourceFormat": field_from(raw_audio, "sourceFormat", "source_format"),
            "sourceSubtype": field_from(raw_audio, "sourceSubtype", "source_subtype"),
            "sourceChannels": field_from(
                raw_audio, "sourceChannels", "source_channels"
            ),
            "sourceSampleRate": field_from(
                raw_audio, "sourceSampleRate", "source_sample_rate"
            ),
            "outputSampleRate": field_from(
                raw_audio, "outputSampleRate", "output_sample_rate"
            ),
            "outputFrames": field_from(raw_audio, "outputFrames", "output_frames"),
            "resampled": raw_audio["resampled"],
            "quantized": raw_audio["quantized"],
            "ditherAlgorithm": field_from(
                raw_audio, "ditherAlgorithm", "dither_algorithm"
            ),
            "splitStereo": field_from(raw_audio, "splitStereo", "split_stereo"),
            "clippedSamples": field_from(
                raw_audio, "clippedSamples", "clipped_samples"
            ),
        }
    return {
        "id": value["id"],
        "type": str(value["type"]).lower(),
        "partitionIndex": field("partitionIndex", "partition_index"),
        "volumeName": field("volumeName", "volume_name"),
        "objectName": field("objectName", "object_name"),
        "removedSfsIds": field("removedSfsIds", "removed_sfs_ids"),
        "insertedSfsIds": field("insertedSfsIds", "inserted_sfs_ids"),
        "freedClusters": field("freedClusters", "freed_clusters"),
        "allocatedClusters": field("allocatedClusters", "allocated_clusters"),
        "audioImport": audio,
    }


def field_from(value: dict[str, Any], camel: str, snake: str) -> Any:
    return value[camel] if camel in value else value[snake]


def upload_bytes(port: int, filename: str, kind: str, content: bytes) -> str:
    media_types = {
        "audio": "audio/wav",
        "manifest": "application/json",
        "package": "application/vnd.axklib.package",
    }
    status, created = http_request(
        port,
        "POST",
        "/api/v1/uploads",
        {
            "filename": filename,
            "kind": kind,
            "mediaType": media_types[kind],
            "size": len(content),
            "sha256": hashlib.sha256(content).hexdigest(),
        },
    )
    assert status == 201, created
    upload_id = str(created["data"]["uploadId"])
    status, response, headers = raw_http_request(
        port,
        "PUT",
        f"/api/v1/uploads/{upload_id}",
        content,
        {"Content-Type": "application/octet-stream", "Upload-Offset": "0"},
    )
    assert status == 200 and headers["upload-offset"] == str(len(content)), response
    status, completed = http_request(
        port, "POST", f"/api/v1/uploads/{upload_id}/complete"
    )
    assert status == 200 and completed["data"]["state"] == "ready", completed
    return upload_id


def prepare_cross_format_sources(root: Path, cli: Path, sfs_fixture: Path) -> list[str]:
    pcm = struct.pack("<hhhh", 0, 1000, -1000, 0)
    wave_bytes = (
        b"RIFF"
        + struct.pack("<I", 36 + len(pcm))
        + b"WAVEfmt "
        + struct.pack("<IHHIIHH", 16, 1, 1, 44100, 88200, 2, 16)
        + b"data"
        + struct.pack("<I", len(pcm))
        + pcm
    )
    (root / "tone.wav").write_bytes(wave_bytes)
    authored_volume = {
        "name": "Volume",
        "waveforms": [
            {"id": "tone", "name": "Tone", "path": "tone.wav", "root_key": 60}
        ],
        "sample_banks": [
            {
                "name": "Tone Bank",
                "waveform_id": "tone",
                "root_key": 60,
                "key_low": 0,
                "key_high": 127,
            }
        ],
    }
    manifests = {
        "authored.ima": {
            "schema_version": "1.0",
            "format": "fat12_floppy",
            "authored_volume": authored_volume,
        },
        "authored.iso": {
            "schema_version": "1.0",
            "format": "iso9660",
            "iso": {
                "volume_id": "AXK_TEST",
                "raw_group": "46DEF120",
                "group_name": "Test Group",
                "raw_volume": "F001",
                "volume_name": "Test Volume",
            },
            "authored_volume": authored_volume,
        },
    }
    for output_name, manifest in manifests.items():
        manifest_path = root / f"{output_name}.json"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        mode = "floppy" if output_name.endswith(".ima") else "iso"
        completed = subprocess.run(
            [str(cli), "create", mode, manifest_path.name, "-o", output_name],
            cwd=root,
            capture_output=True,
            text=True,
        )
        assert completed.returncode == 0, completed.stderr

    object_report = root / "standalone-source"
    completed = subprocess.run(
        [
            str(cli),
            "objects",
            "fixture.hds",
            "-o",
            object_report.name,
            "--object-type",
            "SBNK",
        ],
        cwd=root,
        capture_output=True,
        text=True,
    )
    assert completed.returncode == 0, completed.stderr
    object_row = json.loads(
        (object_report / "objects.json").read_text(encoding="utf-8")
    )[0]
    fixture_bytes = sfs_fixture.read_bytes()
    payload_offset = int(object_row["payload_offset"])
    payload_size = int(object_row["payload_size"])
    (root / "standalone.sbnk").write_bytes(
        fixture_bytes[payload_offset : payload_offset + payload_size]
    )
    (root / "malformed.bin").write_bytes(b"not a Yamaha image or object")
    return sorted(
        [
            "fixture.hds",
            "authored.ima",
            "authored.iso",
            "standalone.sbnk",
            "malformed.bin",
        ]
    )


def prepare_all_action_alteration(root: Path, cli: Path) -> None:
    waveforms = [
        {"id": "wave", "name": "Wave", "path": "tone.wav", "root_key": 60},
        {
            "id": "delete-wave",
            "name": "Delete Wave",
            "path": "tone.wav",
            "root_key": 60,
        },
        {
            "id": "old-wave",
            "name": "Old Wave",
            "path": "tone.wav",
            "root_key": 60,
        },
    ]
    bank_names = [
        "Delete Bank",
        "Old Bank",
        "Bank A",
        "Bank B",
        "Del Group Bank",
        "Delete Direct",
        "Old Group Bank",
        "Old Direct",
    ]
    sample_banks = [
        {
            "name": name,
            "waveform_id": "wave",
            "root_key": 60,
            "key_low": 0,
            "key_high": 127,
        }
        for name in bank_names
    ]
    source_manifest = {
        "schema_version": "1.0",
        "size_bytes": 8 * 1024 * 1024,
        "partitions": [
            {
                "name": "hd1",
                "volumes": [
                    {
                        "name": "Volume",
                        "waveforms": waveforms,
                        "sample_banks": sample_banks,
                        "sample_bank_groups": [
                            {
                                "name": "Delete Group",
                                "member_sample_banks": ["Del Group Bank"],
                            },
                            {
                                "name": "Old Group",
                                "member_sample_banks": ["Old Group Bank"],
                            },
                        ],
                        "programs": [
                            {
                                "number": 128,
                                "assignments": [
                                    {
                                        "sample_bank_group": "Delete Group",
                                        "receive_channel": 1,
                                    },
                                    {
                                        "sample_bank": "Delete Direct",
                                        "receive_channel": 2,
                                    },
                                ],
                            },
                            {
                                "number": 127,
                                "assignments": [
                                    {
                                        "sample_bank_group": "Old Group",
                                        "receive_channel": 1,
                                    },
                                    {
                                        "sample_bank": "Old Direct",
                                        "receive_channel": 2,
                                    },
                                ],
                            },
                        ],
                    },
                    {"name": "Delete Volume", "waveforms": [], "sample_banks": []},
                ],
            }
        ],
    }
    alteration_manifest = {
        "schema_version": "1.0",
        "operations": [
            {
                "id": "delete-volume",
                "type": "delete_volume",
                "partition_index": 0,
                "volume_name": "Delete Volume",
            },
            {
                "id": "insert-volume",
                "type": "insert_volume",
                "partition_index": {"operation_ref": "delete-volume"},
                "volume": {
                    "name": "Insert Volume",
                    "waveforms": [],
                    "sample_banks": [],
                },
            },
            {
                "id": "delete-bank",
                "type": "delete_sbnk",
                "partition_index": 0,
                "volume_name": "Volume",
                "sample_bank_name": "Delete Bank",
            },
            {
                "id": "insert-bank",
                "type": "insert_sbnk",
                "partition_index": {"operation_ref": "delete-bank"},
                "volume_name": "Volume",
                "sample_bank": {
                    "name": "Insert Bank",
                    "waveform_name": "Wave",
                    "root_key": 60,
                    "key_low": 0,
                    "key_high": 127,
                },
            },
            {
                "id": "insert-wave",
                "type": "insert_waveform",
                "partition_index": 0,
                "volume_name": "Volume",
                "audio": {
                    "path": "tone.wav",
                    "waveform_names": ["Insert Wave"],
                    "root_key": 60,
                },
            },
            {
                "id": "delete-wave",
                "type": "delete_waveform",
                "partition_index": {"operation_ref": "insert-wave"},
                "volume_name": "Volume",
                "waveform_name": "Delete Wave",
            },
            {
                "id": "rename-wave",
                "type": "rename_waveform",
                "partition_index": 0,
                "volume_name": "Volume",
                "waveform_name": "Old Wave",
                "new_waveform_name": "New Wave",
            },
            {
                "id": "rename-bank",
                "type": "rename_sbnk",
                "partition_index": 0,
                "volume_name": "Volume",
                "sample_bank_name": "Old Bank",
                "new_sample_bank_name": "New Bank",
            },
            {
                "id": "delete-program",
                "type": "delete_program",
                "partition_index": 0,
                "volume_name": "Volume",
                "program_number": 128,
            },
            {
                "id": "delete-group",
                "type": "delete_sbac",
                "partition_index": {"operation_ref": "delete-program"},
                "volume_name": "Volume",
                "sample_bank_group_name": "Delete Group",
            },
            {
                "id": "insert-group",
                "type": "insert_sbac",
                "partition_index": {"operation_ref": "delete-group"},
                "volume_name": "Volume",
                "sample_bank_group": {
                    "name": "Insert Group",
                    "member_sample_banks": ["Bank A", "Bank B"],
                },
            },
            {
                "id": "rename-group",
                "type": "rename_sbac",
                "partition_index": 0,
                "volume_name": "Volume",
                "sample_bank_group_name": "Old Group",
                "new_sample_bank_group_name": "New Group",
            },
            {
                "id": "insert-program",
                "type": "insert_program",
                "partition_index": {"operation_ref": "rename-group"},
                "volume_name": "Volume",
                "program": {
                    "number": 128,
                    "assignments": [
                        {
                            "sample_bank_group": "Insert Group",
                            "receive_channel": 1,
                        },
                        {"sample_bank": "Delete Direct", "receive_channel": 2},
                    ],
                },
            },
        ],
    }
    (root / "all-actions-source.json").write_text(
        json.dumps(source_manifest), encoding="utf-8"
    )
    (root / "all-actions.json").write_text(
        json.dumps(alteration_manifest), encoding="utf-8"
    )
    completed = subprocess.run(
        [
            str(cli),
            "create",
            "hds",
            "all-actions-source.json",
            "-o",
            "all-actions-source.hds",
        ],
        cwd=root,
        capture_output=True,
        text=True,
    )
    assert completed.returncode == 0, completed.stderr


def cli_version(cli: Path) -> dict[str, str]:
    completed = subprocess.run(
        [str(cli), "--version"], check=True, capture_output=True, text=True
    )
    lines = completed.stdout.splitlines()
    assert len(lines) == 6 and lines[0].startswith("axklib "), completed.stdout
    values = {"sourceIdentity": lines[0].removeprefix("axklib ")}
    for line in lines[1:]:
        name, separator, value = line.partition(": ")
        assert separator, line
        values[name] = value
    return values


def exercise(server: Path, cli: Path, fixture: Path) -> None:
    server = server.resolve()
    cli = cli.resolve()
    fixture = fixture.resolve()
    with tempfile.TemporaryDirectory(prefix="axklib-server-test-") as root:
        root_path = Path(root)
        connection_path = root_path / "state" / "connection.json"
        (root_path / "download.bin").write_bytes(b"abcdef")
        (root_path / "archive-source" / "nested").mkdir(parents=True)
        (root_path / "reports" / "server").mkdir(parents=True)
        (root_path / "reports" / "cli").mkdir()
        (root_path / "extractions" / "server").mkdir(parents=True)
        (root_path / "extractions" / "cli").mkdir()
        (root_path / "packages" / "server").mkdir(parents=True)
        (root_path / "packages" / "cli").mkdir()
        (root_path / "builds" / "server").mkdir(parents=True)
        (root_path / "builds" / "cli").mkdir()
        (root_path / "manifests" / "cli").mkdir(parents=True)
        (root_path / "archive-source" / "alpha.txt").write_bytes(b"alpha")
        (root_path / "archive-source" / "nested" / "beta.bin").write_bytes(b"beta")
        (root_path / "fixture.hds").write_bytes(fixture.read_bytes())
        source_names = prepare_cross_format_sources(
            root_path, cli, root_path / "fixture.hds"
        )
        prepare_all_action_alteration(root_path, cli)
        target_manifest = root_path / "package-target.json"
        target_manifest.write_text(
            json.dumps(
                {
                    "schema_version": "1.0",
                    "size_bytes": 1048576,
                    "partitions": [
                        {
                            "name": "Target",
                            "volumes": [
                                {
                                    "name": "Imported",
                                    "waveforms": [],
                                    "sample_banks": [],
                                }
                            ],
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )
        created_target = subprocess.run(
            [
                str(cli),
                "create",
                "hds",
                target_manifest.name,
                "-o",
                "package-target.hds",
            ],
            cwd=root_path,
            capture_output=True,
            text=True,
        )
        assert created_target.returncode == 0, created_target.stderr
        target_bytes = (root_path / "package-target.hds").read_bytes()
        for target_name in (
            "package-target-file.hds",
            "package-target-upload.hds",
            "package-target-cli.hds",
        ):
            (root_path / target_name).write_bytes(target_bytes)
        abandoned_publication = (
            root_path / ".export.axklib-publication.p4294967295.1.tmp"
        )
        abandoned_publication.mkdir()
        (abandoned_publication / "partial.wav").write_bytes(b"partial")
        ordinary_temporary_file = root_path / ".export.tmp"
        ordinary_temporary_file.write_bytes(b"ordinary")
        server_log_path = root_path / "server.log"
        workspace_store = root_path / "workspaces.json"
        write_workspace_store(workspace_store, root_path)
        server_log = server_log_path.open("wb")
        process = subprocess.Popen(
            [
                str(server),
                "--port",
                "0",
                "--token",
                TOKEN,
                "--workspace-store",
                str(workspace_store),
                "--state-directory",
                str(root_path / "state"),
                "--connection-file",
                str(connection_path),
                "--allow-origin",
                "https://allowed.example",
                "--job-workers",
                "1",
                "--max-event-tickets",
                "1",
                "--job-replay-events",
                "3",
            ],
            stdout=server_log,
            stderr=subprocess.STDOUT,
        )
        shutdown_requested = False
        try:
            connection_metadata = wait_for_connection_file(connection_path, process)
            if os.name != "nt":
                assert stat.S_IMODE(connection_path.stat().st_mode) == 0o600
            assert connection_metadata["schemaVersion"] == 1
            assert connection_metadata["apiVersion"] == "v1"
            assert connection_metadata["bearerToken"] == TOKEN
            assert connection_metadata["pid"] == process.pid
            port = int(connection_metadata["baseUrl"].split(":")[-1].split("/")[0])
            assert connection_metadata["baseUrl"] == f"http://127.0.0.1:{port}/api/v1"
            assert (
                connection_metadata["websocketUrl"]
                == f"ws://127.0.0.1:{port}/api/v1/events"
            )
            wait_until_ready(port, process)
            assert not abandoned_publication.exists()
            assert ordinary_temporary_file.read_bytes() == b"ordinary"
            status, denied_body, _ = raw_http_request(
                port,
                "GET",
                "/api/v1/roots",
                headers={"Authorization": ""},
            )
            assert status == 401
            assert json.loads(denied_body)["error"]["code"] == "authentication_required"
            status, readiness = http_request(port, "GET", "/api/v1/system/health/ready")
            assert status == 200
            assert readiness["data"] == {
                "ready": True,
                "checks": {
                    "configuration": "READY",
                    "executorAdmission": "READY",
                    "sandbox": "READY",
                    "startupCleanup": "READY",
                    "stateStorage": "READY",
                    "workspaceConfiguration": "READY",
                },
            }
            status, roots = http_request(port, "GET", "/api/v1/roots")
            assert status == 200
            assert roots["data"]["roots"] == [
                {"id": "workspace", "displayName": "Workspace", "writable": True}
            ]
            status, workspaces = http_request(port, "GET", "/api/v1/workspaces")
            assert status == 200
            assert workspaces["data"]["state"] == "READY"
            assert workspaces["data"]["revision"] == 1
            assert workspaces["data"]["workspaces"][0]["id"] == "workspace"
            status, host_roots = http_request(
                port, "GET", "/api/v1/host-directories/roots"
            )
            assert status == 200 and host_roots["data"]["roots"]
            status, host_listing = http_request(
                port,
                "POST",
                "/api/v1/host-directories/list",
                {"path": str(root_path), "limit": 10},
            )
            assert status == 200
            assert host_listing["data"]["path"] == str(root_path.resolve())
            secondary_workspace = root_path / "secondary-workspace"
            secondary_workspace.mkdir()
            status, added_workspace = http_request(
                port,
                "POST",
                "/api/v1/workspaces",
                {
                    "displayName": "Secondary",
                    "path": str(secondary_workspace),
                    "writable": True,
                    "revision": 1,
                },
            )
            assert status == 201, added_workspace
            secondary_id = added_workspace["data"]["id"]
            status, stale_workspace = http_request(
                port,
                "POST",
                "/api/v1/workspaces",
                {
                    "displayName": "Stale",
                    "path": str(secondary_workspace),
                    "writable": True,
                    "revision": 1,
                },
            )
            assert status == 409
            assert stale_workspace["error"]["code"] == "workspace_revision_conflict"
            status, removed_workspace = http_request(
                port,
                "DELETE",
                f"/api/v1/workspaces/{secondary_id}",
                {"revision": 2},
            )
            assert status == 204 and removed_workspace is None
            status, metadata = http_request(
                port,
                "POST",
                "/api/v1/files/metadata",
                {"rootId": "workspace", "relativePath": "download.bin"},
            )
            assert status == 200 and metadata["data"]["size"] == 6
            status, created_directory = http_request(
                port,
                "POST",
                "/api/v1/filesystem/directories",
                {
                    "parent": {"rootId": "workspace", "relativePath": ""},
                    "name": "managed",
                },
            )
            assert status == 201, created_directory
            assert created_directory["data"] == {
                "rootId": "workspace",
                "relativePath": "managed",
                "kind": "directory",
                "size": None,
                "writable": True,
            }
            managed_file = root_path / "managed" / "before.txt"
            managed_file.write_text("managed", encoding="utf-8")
            status, renamed_entry = http_request(
                port,
                "PATCH",
                "/api/v1/filesystem/entries",
                {
                    "entry": {
                        "rootId": "workspace",
                        "relativePath": "managed/before.txt",
                    },
                    "name": "after.txt",
                },
            )
            assert status == 200, renamed_entry
            assert renamed_entry["data"]["relativePath"] == "managed/after.txt"
            assert not managed_file.exists()
            assert (root_path / "managed" / "after.txt").read_text(
                encoding="utf-8"
            ) == "managed"
            delete_query = urlencode(
                {"rootId": "workspace", "relativePath": "managed"}
            )
            status, nonempty = http_request(
                port, "DELETE", f"/api/v1/filesystem/entries?{delete_query}"
            )
            assert status == 409, nonempty
            assert nonempty["error"]["code"] == "directory_not_empty"
            delete_query = urlencode(
                {"rootId": "workspace", "relativePath": "managed/after.txt"}
            )
            status, deleted_file = http_request(
                port, "DELETE", f"/api/v1/filesystem/entries?{delete_query}"
            )
            assert status == 200 and deleted_file["data"]["deleted"] is True
            delete_query = urlencode(
                {"rootId": "workspace", "relativePath": "managed"}
            )
            status, deleted_directory = http_request(
                port, "DELETE", f"/api/v1/filesystem/entries?{delete_query}"
            )
            assert (
                status == 200 and deleted_directory["data"]["deleted"] is True
            )
            for traversal in (
                "../download.bin",
                "%2e%2e/download.bin",
                "..%2fdownload.bin",
                "folder\\download.bin",
            ):
                status, rejected = http_request(
                    port,
                    "POST",
                    "/api/v1/files/metadata",
                    {"rootId": "workspace", "relativePath": traversal},
                )
                assert status == 422, (traversal, status, rejected)
                assert rejected["error"]["code"] == "invalid_file_reference"
            status, version = http_request(port, "GET", "/api/v1/system/version")
            assert status == 200 and "semanticVersion" in version["data"]
            command_version = cli_version(cli)
            assert command_version == {
                "sourceIdentity": version["data"]["sourceIdentity"],
                "version": version["data"]["semanticVersion"],
                "package": version["data"]["packageBasename"],
                "git": version["data"]["gitShaShort"],
                "ref": version["data"]["gitRef"],
                "source": "modified" if version["data"]["isDirty"] else "clean",
            }
            status, content, headers = raw_http_request(
                port,
                "GET",
                "/api/v1/system/version",
                headers={"X-Request-Id": "client.request_1"},
            )
            correlated = json.loads(content)
            assert status == 200
            assert headers["x-request-id"] == "client.request_1"
            assert correlated["meta"]["requestId"] == "client.request_1"
            status, content, headers = raw_http_request(
                port,
                "GET",
                "/api/v1/system/version",
                headers={"X-Request-Id": "invalid/request/id"},
            )
            generated_id = headers["x-request-id"]
            generated = json.loads(content)
            assert status == 200 and generated_id.startswith("request-")
            assert generated["meta"]["requestId"] == generated_id
            for request_id_length, should_echo in ((96, True), (97, False)):
                supplied_id = "r" * request_id_length
                status, content, headers = raw_http_request(
                    port,
                    "GET",
                    "/api/v1/system/version",
                    headers={"X-Request-Id": supplied_id},
                )
                body = json.loads(content)
                expected_id = supplied_id if should_echo else headers["x-request-id"]
                assert status == 200 and body["meta"]["requestId"] == expected_id
                assert (headers["x-request-id"] == supplied_id) is should_echo
            status, _, headers = raw_http_request(
                port, "GET", "/api/v1/system/health/live"
            )
            assert status == 204 and headers["x-request-id"].startswith("request-")
            status, capabilities = http_request(
                port, "GET", "/api/v1/system/capabilities"
            )
            assert status == 200
            assert capabilities["meta"]["requestId"]
            assert capabilities["data"]["limits"] == {
                "maximumJsonBytes": 1024 * 1024,
                "maximumJsonDepth": 32,
                "maximumJsonNodes": 100000,
                "maximumJsonContainerItems": 10000,
                "maximumJsonStringBytes": 256 * 1024,
                "maximumUploadBytes": 4 * 1024 * 1024 * 1024,
                "maximumUploadTotalBytes": 8 * 1024 * 1024 * 1024,
                "maximumUploadChunkBytes": 1024 * 1024,
                "maximumDownloadRangeBytes": 8 * 1024 * 1024,
                "maximumDownloadArchiveBytes": 4 * 1024 * 1024 * 1024,
                "maximumDownloadArchiveTotalBytes": 8 * 1024 * 1024 * 1024,
                "maximumDownloadArchiveEntries": 100000,
                "downloadArchiveRetentionSeconds": 300,
                "maximumWebsocketDeliveryEvents": 1024,
                "maximumWebsocketDeliveryBytes": 4 * 1024 * 1024,
                "maximumQueuedJobs": 64,
                "maximumImageSessions": 32,
                "maximumPageSize": 500,
            }
            status, metrics = http_request(port, "GET", "/api/v1/system/metrics")
            assert status == 200
            assert metrics["data"]["totalRequests"] >= 3
            assert metrics["data"]["activeRequests"] >= 1
            assert metrics["data"]["responses2xx"] >= 2
            assert metrics["data"]["responses5xx"] == 0
            nested: dict[str, Any] = {
                "rootId": "workspace",
                "relativePath": "download.bin",
            }
            for _ in range(40):
                nested = {"nested": nested}
            status, rejected = http_request(
                port, "POST", "/api/v1/files/metadata", nested
            )
            assert status == 413, rejected
            assert rejected["error"]["code"] == "json_structure_too_large", rejected
            status, openapi = http_request(port, "GET", "/api/v1/openapi.json")
            assert status == 200
            documented = openapi["paths"]
            capability_ids = {
                operation["id"] for operation in capabilities["data"]["operations"]
            }
            documented_ids: set[str] = set()
            for operation in capabilities["data"]["operations"]:
                assert operation["method"] in {"GET", "POST"}
                assert operation["mode"] in {"REQUEST", "JOB"}
                assert operation["operationClass"] in {"READ", "WRITE"}
                route = operation["route"].removeprefix("/api/v1")
                assert route in documented, operation
                operation_ids = documented[route][operation["method"].lower()][
                    "x-axklib-operation-ids"
                ]
                assert operation["id"] in operation_ids
                documented_ids.update(operation_ids)
            assert capability_ids == documented_ids

            fixture_request = {
                "sources": [{"rootId": "workspace", "relativePath": "fixture.hds"}],
                "destination": {"rootId": "workspace", "relativePath": "exports"},
                "scope": "FILE",
                "stereo": "AUTO",
            }
            malformed_requests: list[bytes | str] = [
                b"{",
                b"[]",
                json.dumps(
                    {
                        "operationId": "fixture.echo.alpha",
                        **fixture_request,
                        "scope": "INVALID",
                    }
                ),
                json.dumps(
                    {
                        "operationId": "fixture.echo.alpha",
                        **fixture_request,
                        "unexpected": True,
                    }
                ),
                json.dumps(
                    {
                        "operationId": "fixture.echo.alpha",
                        **fixture_request,
                        "sources": [fixture_request["sources"][0]] * 257,
                    }
                ),
                json.dumps({"operationId": "fixture.echo.unknown", **fixture_request}),
            ]
            for payload in malformed_requests:
                status, content, _ = raw_http_request(
                    port,
                    "POST",
                    "/api/v1/fixture-operations",
                    payload,
                    {"Content-Type": "application/json"},
                )
                assert status == 400, (payload, content)

            for expected_invocations, operation_id in enumerate(
                ("fixture.echo.alpha", "fixture.echo.beta"), start=1
            ):
                status, echoed = http_request(
                    port,
                    "POST",
                    "/api/v1/fixture-operations",
                    {"operationId": operation_id, **fixture_request},
                )
                assert status == 200, echoed
                assert echoed["data"]["operationId"] == operation_id
                assert echoed["data"]["invocationCount"] == expected_invocations
                assert echoed["data"]["echo"]["scope"] == "file"
                assert echoed["data"]["echo"]["stereo"] == "auto"

            status, _, headers = raw_http_request(
                port,
                "OPTIONS",
                "/api/v1/uploads",
                headers={"Origin": "https://denied.example"},
            )
            assert status == 403 and "access-control-allow-origin" not in headers, (
                status,
                headers,
            )
            status, _, headers = raw_http_request(
                port,
                "OPTIONS",
                "/api/v1/uploads",
                headers={"Origin": "https://allowed.example"},
            )
            assert (
                status == 204
                and headers["access-control-allow-origin"] == "https://allowed.example"
            )

            status, created = http_request(
                port,
                "POST",
                "/api/v1/uploads",
                {
                    "filename": "manifest.json",
                    "kind": "manifest",
                    "mediaType": "application/json",
                    "size": 2,
                },
            )
            assert status == 201, created
            upload_id = str(created["data"]["uploadId"])
            status, hidden_upload, _ = raw_http_request(
                port,
                "GET",
                f"/api/v1/uploads/{upload_id}",
                headers={"Authorization": ""},
            )
            assert status == 401
            assert (
                json.loads(hidden_upload)["error"]["code"] == "authentication_required"
            )
            status, content, headers = raw_http_request(
                port,
                "PUT",
                f"/api/v1/uploads/{upload_id}",
                b"{}",
                {"Content-Type": "application/octet-stream", "Upload-Offset": "0"},
            )
            assert status == 200 and headers["upload-offset"] == "2", content
            status, completed = http_request(
                port, "POST", f"/api/v1/uploads/{upload_id}/complete"
            )
            assert status == 200 and completed["data"]["state"] == "ready"
            status, materialized = http_request(
                port,
                "POST",
                f"/api/v1/uploads/{upload_id}/materialize",
                {
                    "destination": {
                        "rootId": "workspace",
                        "relativePath": "saved-manifest.json",
                    }
                },
            )
            assert (
                status == 201
                and materialized["data"]["file"]["relativePath"]
                == "saved-manifest.json"
            )
            assert (root_path / "saved-manifest.json").read_bytes() == b"{}"

            query = urlencode({"rootId": "workspace", "relativePath": "download.bin"})
            status, content, headers = raw_http_request(
                port,
                "GET",
                f"/api/v1/files/content?{query}",
                headers={"Range": "bytes=1-3"},
            )
            assert status == 206 and content == b"bcd", (status, content)
            assert headers["content-range"] == "bytes 1-3/6"
            assert (root_path / "download.bin").read_bytes() == b"abcdef"

            status, archive = http_request(
                port,
                "POST",
                "/api/v1/files/archive",
                {
                    "directory": {
                        "rootId": "workspace",
                        "relativePath": "archive-source",
                    }
                },
            )
            assert status == 201, archive
            assert archive["data"]["filename"] == "archive-source.tar"
            assert archive["data"]["entryCount"] == 2
            archive_path = str(archive["data"]["contentPath"])
            status, content, headers = raw_http_request(port, "GET", archive_path)
            assert status == 200 and headers["content-type"].startswith(
                "application/x-tar"
            ), headers
            with tarfile.open(fileobj=io.BytesIO(content), mode="r:") as downloaded:
                assert downloaded.getnames() == ["alpha.txt", "nested/beta.bin"]
                alpha = downloaded.extractfile("alpha.txt")
                beta = downloaded.extractfile("nested/beta.bin")
                assert alpha is not None and alpha.read() == b"alpha"
                assert beta is not None and beta.read() == b"beta"
            status, _, _ = raw_http_request(port, "DELETE", archive_path)
            assert status == 204
            status, _, _ = raw_http_request(port, "GET", archive_path)
            assert status == 404

            status, opened = http_request(
                port,
                "POST",
                "/api/v1/images",
                {"source": {"rootId": "workspace", "relativePath": "fixture.hds"}},
            )
            assert status == 201, opened
            image_id = str(opened["data"]["imageId"])
            fixture_query = urlencode(
                {"rootId": "workspace", "relativePath": "fixture.hds"}
            )
            status, in_use = http_request(
                port, "DELETE", f"/api/v1/filesystem/entries?{fixture_query}"
            )
            assert status == 409, in_use
            assert in_use["error"]["code"] == "entry_in_use"
            assert opened["data"]["format"] == "sfs"
            assert opened["data"]["availableOperations"] == [
                "images.content",
                "images.objects",
                "images.relationships",
                "images.validation.issues",
                "images.preview",
                "auditions.prepare",
                "images.alter.volumes",
            ]
            assert opened["data"]["objectCount"] > 0
            status, objects = http_request(
                port, "GET", f"/api/v1/images/{image_id}/objects?limit=100"
            )
            assert status == 200 and objects["data"]["items"], objects
            for invalid_page in ("limit=0", "limit=5001", f"cursor={'x' * 513}"):
                status, invalid = http_request(
                    port, "GET", f"/api/v1/images/{image_id}/objects?{invalid_page}"
                )
                assert status == 400, invalid
            assert all(
                item["id"].startswith("object-") for item in objects["data"]["items"]
            )
            assert all(item["sizeBytes"] > 0 for item in objects["data"]["items"])
            assert root not in json.dumps(objects)
            status, waveforms = http_request(
                port, "GET", f"/api/v1/images/{image_id}/objects?limit=100&type=SMPL"
            )
            assert status == 200 and waveforms["data"]["items"], waveforms
            assert all(item["type"] == "SMPL" for item in waveforms["data"]["items"])
            status, missing_objects = http_request(
                port, "GET", f"/api/v1/images/{image_id}/objects?limit=100&type=MISSING"
            )
            assert status == 200 and missing_objects["data"]["items"] == [], (
                missing_objects
            )
            assert missing_objects["data"]["totalCount"] == 0
            waveform = next(
                item for item in objects["data"]["items"] if item["type"] == "SMPL"
            )
            preview_query = urlencode({"objectId": waveform["id"], "bins": 16})
            status, preview = http_request(
                port, "GET", f"/api/v1/images/{image_id}/preview?{preview_query}"
            )
            assert status == 200 and len(preview["data"]["bins"]) == 16, preview
            status, submitted = http_request(
                port,
                "POST",
                "/api/v1/auditions",
                {"imageId": image_id, "objectId": waveform["id"]},
            )
            assert status == 202, submitted
            audition_job = wait_for_job(
                port, str(submitted["data"]["jobId"]), process
            )
            assert audition_job["state"] == "COMPLETED", audition_job
            audition = audition_job["result"]
            audition_id = str(audition["auditionId"])
            assert audition["objectId"] == waveform["id"]
            status, wav_header, headers = raw_http_request(
                port,
                "GET",
                f"/api/v1/auditions/{audition_id}/audio",
                headers={"Range": "bytes=0-43"},
            )
            assert status == 206 and len(wav_header) == 44, (status, headers)
            assert wav_header[:4] == b"RIFF" and wav_header[8:12] == b"WAVE"
            assert headers["accept-ranges"] == "bytes"
            assert headers["content-range"].startswith("bytes 0-43/")
            status, _, _ = raw_http_request(
                port, "DELETE", f"/api/v1/auditions/{audition_id}"
            )
            assert status == 204
            status, _, _ = raw_http_request(
                port,
                "GET",
                f"/api/v1/auditions/{audition_id}/audio",
                headers={"Range": "bytes=0-43"},
            )
            assert status == 404
            status, content_page = http_request(
                port, "GET", f"/api/v1/images/{image_id}/content?limit=2"
            )
            assert status == 200 and content_page["data"]["items"], content_page
            assert content_page["data"]["totalCount"] >= len(
                content_page["data"]["items"]
            ), content_page
            assert all(
                item["parentId"] is None for item in content_page["data"]["items"]
            ), content_page
            parent_id = content_page["data"]["items"][0]["id"]
            child_query = urlencode({"limit": 100, "parentId": parent_id})
            status, child_page = http_request(
                port, "GET", f"/api/v1/images/{image_id}/content?{child_query}"
            )
            assert status == 200 and child_page["data"]["items"], child_page
            assert child_page["data"]["totalCount"] >= len(
                child_page["data"]["items"]
            ), child_page
            assert all(
                item["parentId"] == parent_id for item in child_page["data"]["items"]
            ), child_page
            status, relationships = http_request(
                port, "GET", f"/api/v1/images/{image_id}/relationships?limit=2"
            )
            assert status == 200 and relationships["data"]["items"], relationships
            assert all(
                "receiveChannelDisplay" in item
                for item in relationships["data"]["items"]
            ), relationships
            first_relationship = relationships["data"]["items"][0]
            relationship_query = urlencode(
                {
                    "limit": 100,
                    "sourceObjectId": first_relationship["sourceObjectId"],
                    "type": first_relationship["type"],
                }
            )
            status, filtered_relationships = http_request(
                port,
                "GET",
                f"/api/v1/images/{image_id}/relationships?{relationship_query}",
            )
            assert status == 200 and filtered_relationships["data"]["items"], (
                filtered_relationships
            )
            assert all(
                item["sourceObjectId"] == first_relationship["sourceObjectId"]
                and item["type"] == first_relationship["type"]
                for item in filtered_relationships["data"]["items"]
            ), filtered_relationships
            status, validation = http_request(
                port, "GET", f"/api/v1/images/{image_id}/validation/issues?limit=2"
            )
            assert status == 200 and "items" in validation["data"], validation
            status, closed = http_request(port, "DELETE", f"/api/v1/images/{image_id}")
            assert status == 200 and closed["data"]["closed"] is True
            status, closed_again = http_request(
                port, "DELETE", f"/api/v1/images/{image_id}"
            )
            assert status == 200 and closed_again["data"]["closed"] is True
            status, _ = http_request(port, "GET", f"/api/v1/images/{image_id}")
            assert status == 404

            sources = [
                {"rootId": "workspace", "relativePath": name} for name in source_names
            ]
            sfs_sources = [
                source for source in sources if source["relativePath"] == "fixture.hds"
            ]
            fat12_sources = [
                source for source in sources if source["relativePath"] == "authored.ima"
            ]
            malformed_sources = [
                source
                for source in sources
                if source["relativePath"] == "malformed.bin"
            ]
            valid_sources = [
                source
                for source in sources
                if source["relativePath"] != "malformed.bin"
            ]
            report_requests = (
                ("report.info", "/api/v1/reports/info", {"sources": sources}),
                (
                    "report.objects",
                    "/api/v1/reports/objects",
                    {
                        "sources": sources,
                        "destination": {
                            "rootId": "workspace",
                            "relativePath": "reports/server/objects",
                        },
                        "objectType": "SBNK",
                    },
                ),
                (
                    "report.relationships",
                    "/api/v1/reports/relationships",
                    {
                        "sources": sources,
                        "destination": {
                            "rootId": "workspace",
                            "relativePath": "reports/server/relationships",
                        },
                    },
                ),
                (
                    "report.inventory",
                    "/api/v1/reports/inventory",
                    {
                        "sources": sources,
                        "destination": {
                            "rootId": "workspace",
                            "relativePath": "reports/server/inventory",
                        },
                    },
                ),
                (
                    "report.coverage",
                    "/api/v1/reports/coverage",
                    {
                        "sources": sources,
                        "destination": {
                            "rootId": "workspace",
                            "relativePath": "reports/server/coverage",
                        },
                    },
                ),
                (
                    "report.orphans",
                    "/api/v1/reports/orphans",
                    {
                        "sources": sfs_sources,
                        "destination": {
                            "rootId": "workspace",
                            "relativePath": "reports/server/orphans",
                        },
                    },
                ),
                (
                    "report.validate",
                    "/api/v1/reports/validation",
                    {
                        "sources": valid_sources,
                        "destination": {
                            "rootId": "workspace",
                            "relativePath": "reports/server/validate",
                        },
                        "policy": "NORMAL",
                    },
                ),
                (
                    "corpus.audit",
                    "/api/v1/corpus-audits",
                    {
                        "sources": sources,
                        "destination": {
                            "rootId": "workspace",
                            "relativePath": "reports/server/corpus-audit",
                        },
                        "waveSmokeLimit": 1,
                    },
                ),
            )
            report_results: dict[str, dict[str, Any]] = {}
            for operation_id, route, request in report_requests:
                status, submitted = http_request(port, "POST", route, request)
                assert status == 202, (operation_id, submitted)
                job = wait_for_job(port, str(submitted["data"]["jobId"]), process)
                assert job["state"] == "COMPLETED", (operation_id, job)
                assert job["operationId"] == operation_id
                result = job["result"]
                assert result["operationId"] == operation_id
                report_results[operation_id] = result
                for artifact in result.get("artifacts", []):
                    assert artifact["rootId"] == "workspace"
                    relative_path = Path(artifact["relativePath"])
                    assert (
                        not relative_path.is_absolute()
                        and ".." not in relative_path.parts
                    )
                    assert (root_path / relative_path).is_file(), (
                        operation_id,
                        artifact,
                    )

            cli_report_commands = {
                "report.objects": (["objects"], ["--object-type", "SBNK"]),
                "report.relationships": (["relationships"], []),
                "report.inventory": (["inventory"], []),
                "report.coverage": (["coverage"], []),
                "report.orphans": (["orphans"], []),
                "report.validate": (["validate"], ["--policy", "normal"]),
                "corpus.audit": (["corpus", "audit"], ["--wave-smoke-limit", "1"]),
            }
            for operation_id, (command, options) in cli_report_commands.items():
                report_name = operation_id.removeprefix("report.").replace(".", "-")
                cli_destination = Path("reports") / "cli" / report_name
                if operation_id == "report.orphans":
                    cli_sources = ["fixture.hds"]
                elif operation_id == "report.validate":
                    cli_sources = [
                        name for name in source_names if name != "malformed.bin"
                    ]
                else:
                    cli_sources = source_names
                completed = subprocess.run(
                    [
                        str(cli),
                        *command,
                        *cli_sources,
                        "-o",
                        cli_destination.as_posix(),
                        *options,
                    ],
                    cwd=root_path,
                    capture_output=True,
                    text=True,
                )
                partial_exit_codes = {
                    "report.objects": 3,
                    "report.relationships": 3,
                    "report.inventory": 1,
                    "report.coverage": 3,
                    "corpus.audit": 3,
                }
                expected_exit = partial_exit_codes.get(operation_id, 0)
                assert completed.returncode == expected_exit, (
                    operation_id,
                    completed.returncode,
                    completed.stdout,
                    completed.stderr,
                )
                server_destination = root_path / "reports" / "server" / report_name
                server_hashes = artifact_hashes(server_destination)
                cli_hashes = artifact_hashes(root_path / cli_destination)
                if server_hashes != cli_hashes:
                    data_file = next(
                        (
                            path
                            for path in server_hashes
                            if path.endswith(".json")
                            and not path.startswith("_schemas/")
                        ),
                        None,
                    )
                    data_difference: Any = None
                    if data_file is not None:
                        server_data = json.loads(
                            (server_destination / data_file).read_text(encoding="utf-8")
                        )
                        cli_data = json.loads(
                            (root_path / cli_destination / data_file).read_text(
                                encoding="utf-8"
                            )
                        )
                        if isinstance(server_data, list) and isinstance(cli_data, list):
                            for index, (server_row, cli_row) in enumerate(
                                zip(server_data, cli_data, strict=False)
                            ):
                                if server_row != cli_row:
                                    data_difference = (index, server_row, cli_row)
                                    break
                        elif server_data != cli_data:
                            data_difference = (server_data, cli_data)
                    raise AssertionError(
                        (operation_id, server_hashes, cli_hashes, data_difference)
                    )

            status, unsupported_orphans = http_request(
                port,
                "POST",
                "/api/v1/reports/orphans",
                {
                    "sources": fat12_sources,
                    "destination": {
                        "rootId": "workspace",
                        "relativePath": "reports/server/orphans-fat12",
                    },
                },
            )
            assert status == 202, unsupported_orphans
            unsupported_job = wait_for_job(
                port, str(unsupported_orphans["data"]["jobId"]), process
            )
            assert unsupported_job["state"] == "FAILED", unsupported_job
            assert unsupported_job["error"]["code"] == "unsupported_media", (
                unsupported_job
            )

            status, malformed_validation = http_request(
                port,
                "POST",
                "/api/v1/reports/validation",
                {
                    "sources": malformed_sources,
                    "destination": {
                        "rootId": "workspace",
                        "relativePath": "reports/server/validate-malformed",
                    },
                    "policy": "NORMAL",
                },
            )
            assert status == 202, malformed_validation
            malformed_job = wait_for_job(
                port, str(malformed_validation["data"]["jobId"]), process
            )
            assert malformed_job["state"] == "FAILED", malformed_job
            assert malformed_job["error"]["code"] == "validation_input_failed", (
                malformed_job
            )

            cli_info_result = subprocess.run(
                [str(cli), "info", *source_names, "--format", "json"],
                cwd=root_path,
                capture_output=True,
                text=True,
            )
            assert cli_info_result.returncode == 1, cli_info_result.stderr
            cli_info = json.loads(cli_info_result.stdout)
            assert report_results["report.info"]["loadedCount"] == len(
                cli_info["trees"]
            )
            assert report_results["report.info"]["failedCount"] == len(
                cli_info["load_errors"]
            )
            assert report_results["report.info"]["loadedCount"] == 4
            assert report_results["report.info"]["failedCount"] == 1
            server_trees = {
                tree["sourcePath"]: tree
                for tree in report_results["report.info"]["trees"]
            }
            cli_trees = {tree["source_path"]: tree for tree in cli_info["trees"]}
            assert server_trees.keys() == cli_trees.keys()
            for source_path, server_tree in server_trees.items():
                cli_tree = cli_trees[source_path]
                assert server_tree["containerKind"] == cli_tree["container_kind"]
                assert server_tree["detectedFormat"] == cli_tree["detected_format"]
                assert [canonical_info_node(node) for node in server_tree["roots"]] == [
                    canonical_info_node(node) for node in cli_tree["roots"]
                ]

            cli_package_path = Path("packages/cli/bank.axksbnk")
            cli_export = subprocess.run(
                [
                    str(cli),
                    "package",
                    "export",
                    "fixture.hds",
                    "--root",
                    "sbnk=sine wave",
                    "--partition",
                    "0",
                    "--volume",
                    "New Volume",
                    "-o",
                    "packages/cli/bank",
                    "--format",
                    "json",
                ],
                cwd=root_path,
                capture_output=True,
                text=True,
            )
            assert cli_export.returncode == 0, (cli_export.stdout, cli_export.stderr)
            cli_export_data = json.loads(cli_export.stdout)

            package_export_request = {
                "source": {"rootId": "workspace", "relativePath": "fixture.hds"},
                "output": {
                    "rootId": "workspace",
                    "relativePath": "packages/server/bank",
                },
                "roots": [
                    {
                        "kind": "SBNK",
                        "partitionIndex": 0,
                        "volumeName": "New Volume",
                        "objectName": "sine wave",
                    }
                ],
            }
            status, submitted_export = http_request(
                port,
                "POST",
                "/api/v1/package-exports",
                package_export_request,
                {"Idempotency-Key": "package-export"},
            )
            assert status == 202, submitted_export
            status, replayed_export = http_request(
                port,
                "POST",
                "/api/v1/package-exports",
                package_export_request,
                {"Idempotency-Key": "package-export"},
            )
            assert status == 202, replayed_export
            assert replayed_export["data"]["jobId"] == submitted_export["data"]["jobId"]
            export_job = wait_for_job(
                port, str(submitted_export["data"]["jobId"]), process
            )
            assert export_job["state"] == "COMPLETED", export_job
            export_result = export_job["result"]
            assert export_result["output"] == {
                "rootId": "workspace",
                "relativePath": "packages/server/bank.axksbnk",
            }
            server_package_path = Path(export_result["output"]["relativePath"])
            assert (root_path / server_package_path).read_bytes() == (
                root_path / cli_package_path
            ).read_bytes()
            assert canonical_package_summary(
                export_result
            ) == canonical_package_summary(cli_export_data)

            cli_package_reads: dict[str, dict[str, Any]] = {}
            for command in ("inspect", "verify"):
                completed = subprocess.run(
                    [
                        str(cli),
                        "package",
                        command,
                        cli_package_path.as_posix(),
                        "--format",
                        "json",
                    ],
                    cwd=root_path,
                    capture_output=True,
                    text=True,
                )
                assert completed.returncode == 0, (
                    command,
                    completed.stdout,
                    completed.stderr,
                )
                cli_package_reads[command] = json.loads(completed.stdout)

            file_package_ref = {
                "fileRef": {
                    "rootId": "workspace",
                    "relativePath": server_package_path.as_posix(),
                }
            }
            for command in ("inspection", "verification"):
                status, response = http_request(
                    port,
                    "POST",
                    f"/api/v1/package-{command}s",
                    {"package": file_package_ref},
                )
                assert status == 200, (command, response)
                cli_command = "inspect" if command == "inspection" else "verify"
                assert canonical_package_summary(
                    response["data"]
                ) == canonical_package_summary(cli_package_reads[cli_command])

            package_bytes = (root_path / server_package_path).read_bytes()
            package_upload_id = upload_bytes(
                port, "browser.axksbnk", "package", package_bytes
            )
            upload_package_ref = {"uploadRef": {"uploadId": package_upload_id}}
            for command in ("inspection", "verification"):
                status, response = http_request(
                    port,
                    "POST",
                    f"/api/v1/package-{command}s",
                    {"package": upload_package_ref},
                )
                assert status == 200, (command, response)
                cli_command = "inspect" if command == "inspection" else "verify"
                assert canonical_package_summary(
                    response["data"]
                ) == canonical_package_summary(cli_package_reads[cli_command])

            (root_path / "packages/server/malformed.axksbnk").write_bytes(
                b"not a portable package"
            )
            status, malformed_package = http_request(
                port,
                "POST",
                "/api/v1/package-inspections",
                {
                    "package": {
                        "fileRef": {
                            "rootId": "workspace",
                            "relativePath": "packages/server/malformed.axksbnk",
                        }
                    }
                },
            )
            assert status == 422, malformed_package
            assert malformed_package["error"]["code"] == "package_operation_failed"

            with zipfile.ZipFile(root_path / server_package_path) as source_archive:
                package_entries = {
                    name: source_archive.read(name)
                    for name in source_archive.namelist()
                }
            payload_name = next(
                name for name in package_entries if name.startswith("payloads/")
            )
            changed_payload = bytearray(package_entries[payload_name])
            changed_payload[0] ^= 0x01
            package_entries[payload_name] = bytes(changed_payload)
            corrupt_package_path = root_path / "packages/server/corrupt.axksbnk"
            with zipfile.ZipFile(
                corrupt_package_path, "w", compression=zipfile.ZIP_STORED
            ) as corrupt_archive:
                for name, content in package_entries.items():
                    corrupt_archive.writestr(name, content)
            status, corrupt_package = http_request(
                port,
                "POST",
                "/api/v1/package-verifications",
                {
                    "package": {
                        "fileRef": {
                            "rootId": "workspace",
                            "relativePath": "packages/server/corrupt.axksbnk",
                        }
                    }
                },
            )
            assert status == 422, corrupt_package
            assert corrupt_package["error"]["code"] == "package_operation_failed"

            destination = {
                "packageIndex": 0,
                "rootIndex": 0,
                "partitionIndex": 0,
                "volumeName": "Imported",
            }
            cli_import = subprocess.run(
                [
                    str(cli),
                    "package",
                    "import",
                    "package-target-cli.hds",
                    cli_package_path.as_posix(),
                    "--destination",
                    json.dumps(
                        {
                            "package": 0,
                            "root": 0,
                            "partition": 0,
                            "volume": "Imported",
                        }
                    ),
                    "-o",
                    "packages/cli/imported.hds",
                    "--format",
                    "json",
                ],
                cwd=root_path,
                capture_output=True,
                text=True,
            )
            assert cli_import.returncode == 0, (cli_import.stdout, cli_import.stderr)
            cli_import_data = json.loads(cli_import.stdout)

            package_plans: list[tuple[str, dict[str, Any]]] = []
            for source_kind, package_reference in (
                ("file", file_package_ref),
                ("upload", upload_package_ref),
            ):
                request = {
                    "target": {
                        "rootId": "workspace",
                        "relativePath": f"package-target-{source_kind}.hds",
                    },
                    "output": {
                        "rootId": "workspace",
                        "relativePath": f"packages/server/imported-{source_kind}.hds",
                    },
                    "packages": [package_reference],
                    "destinations": [destination],
                }
                status, response = http_request(
                    port, "POST", "/api/v1/package-import-plans", request
                )
                assert status == 200, (source_kind, response)
                plan = response["data"]
                assert plan["valid"] and plan["actions"] and plan["allocation"], plan
                serialized_plan = json.dumps(plan)
                assert str(root_path) not in serialized_plan
                assert TOKEN not in serialized_plan
                assert canonical_package_plan(plan) == canonical_package_plan(
                    cli_import_data
                )
                package_plans.append((source_kind, plan))

            for source_kind, plan in package_plans:
                status, submitted = http_request(
                    port,
                    "POST",
                    "/api/v1/package-imports",
                    {"planToken": plan["planToken"]},
                    {"Idempotency-Key": f"package-import-{source_kind}"},
                )
                assert status == 202, (source_kind, submitted)
                job = wait_for_job(port, str(submitted["data"]["jobId"]), process)
                assert job["state"] == "COMPLETED", (source_kind, job)
                assert job["result"]["applied"] is True, job
                server_import = (
                    root_path / f"packages/server/imported-{source_kind}.hds"
                )
                assert (
                    server_import.read_bytes()
                    == (root_path / "packages/cli/imported.hds").read_bytes()
                )

            manifest_templates: dict[str, dict[str, Any]] = {}
            for kind in ("HDS", "FLOPPY", "ISO"):
                cli_manifest = Path("manifests/cli") / f"{kind.lower()}.json"
                completed = subprocess.run(
                    [
                        str(cli),
                        "create",
                        "manifest",
                        kind.lower(),
                        "-o",
                        cli_manifest.as_posix(),
                    ],
                    cwd=root_path,
                    capture_output=True,
                    text=True,
                )
                assert completed.returncode == 0, (
                    kind,
                    completed.stdout,
                    completed.stderr,
                )
                status, response = http_request(
                    port,
                    "POST",
                    "/api/v1/manifest-templates",
                    {"kind": kind},
                )
                assert status == 200, (kind, response)
                template = response["data"]
                assert (
                    template["canonicalJson"].encode()
                    == (root_path / cli_manifest).read_bytes()
                )
                assert json.loads(template["canonicalJson"]) == template["manifest"]
                assert template["choices"]["manifestSources"] == [
                    "INLINE",
                    "FILE_REF",
                    "UPLOAD_REF",
                ]
                assert template["documentation"].startswith("/")
                assert str(root_path) not in json.dumps(template)
                manifest_templates[kind] = template

            hds_manifest = json.loads(target_manifest.read_text(encoding="utf-8"))
            build_cases: list[dict[str, Any]] = [
                {
                    "kind": "HDS",
                    "operationId": "create.hds",
                    "manifest": {"inline": hds_manifest},
                    "inputBindings": [],
                    "manifestPath": target_manifest.name,
                    "serverOutput": "builds/server/authored.hds",
                    "cliOutput": "builds/cli/authored.hds",
                },
                {
                    "kind": "FLOPPY",
                    "operationId": "create.floppy",
                    "manifest": {
                        "fileRef": {
                            "rootId": "workspace",
                            "relativePath": "authored.ima.json",
                        }
                    },
                    "inputBindings": [
                        {
                            "manifestPath": "tone.wav",
                            "input": {
                                "fileRef": {
                                    "rootId": "workspace",
                                    "relativePath": "tone.wav",
                                }
                            },
                        }
                    ],
                    "manifestPath": "authored.ima.json",
                    "serverOutput": "builds/server/authored.ima",
                    "cliOutput": "builds/cli/authored.ima",
                },
            ]
            iso_manifest_bytes = (root_path / "authored.iso.json").read_bytes()
            iso_manifest_upload = upload_bytes(
                port, "authored.iso.json", "manifest", iso_manifest_bytes
            )
            tone_upload = upload_bytes(
                port, "tone.wav", "audio", (root_path / "tone.wav").read_bytes()
            )
            build_cases.append(
                {
                    "kind": "ISO",
                    "operationId": "create.iso",
                    "manifest": {"uploadRef": {"uploadId": iso_manifest_upload}},
                    "inputBindings": [
                        {
                            "manifestPath": "tone.wav",
                            "input": {"uploadRef": {"uploadId": tone_upload}},
                        }
                    ],
                    "manifestPath": "authored.iso.json",
                    "serverOutput": "builds/server/authored.iso",
                    "cliOutput": "builds/cli/authored.iso",
                }
            )

            transfer_manifest = {
                "schema_version": "1.0",
                "format": "iso9660",
                "iso": {
                    "volume_id": "AXK_TEST",
                    "raw_group": "00000010",
                    "group_name": "Transfer",
                    "raw_volume": "F001",
                    "volume_name": "Transferred",
                },
                "transfer": {"source_path": "authored.ima", "selection": "all"},
            }
            (root_path / "transfer.json").write_text(
                json.dumps(transfer_manifest), encoding="utf-8"
            )
            build_cases.append(
                {
                    "kind": "ISO",
                    "operationId": "create.iso",
                    "manifest": {"inline": transfer_manifest},
                    "inputBindings": [
                        {
                            "manifestPath": "authored.ima",
                            "input": {
                                "fileRef": {
                                    "rootId": "workspace",
                                    "relativePath": "authored.ima",
                                }
                            },
                        }
                    ],
                    "manifestPath": "transfer.json",
                    "serverOutput": "builds/server/transfer.iso",
                    "cliOutput": "builds/cli/transfer.iso",
                }
            )

            for index, case in enumerate(build_cases):
                completed = subprocess.run(
                    [
                        str(cli),
                        "create",
                        case["operationId"].removeprefix("create."),
                        case["manifestPath"],
                        "-o",
                        case["cliOutput"],
                    ],
                    cwd=root_path,
                    capture_output=True,
                    text=True,
                )
                assert completed.returncode == 0, (
                    case["kind"],
                    completed.stdout,
                    completed.stderr,
                )
                plan_request = {
                    "kind": case["kind"],
                    "manifest": case["manifest"],
                    "inputBindings": case["inputBindings"],
                    "output": {
                        "rootId": "workspace",
                        "relativePath": case["serverOutput"],
                    },
                }
                status, response = http_request(
                    port, "POST", "/api/v1/image-build-plans", plan_request
                )
                assert status == 200, (case["kind"], response)
                plan = response["data"]
                assert plan["kind"] == plan["summary"]["format"] == case["kind"]
                assert str(root_path) not in json.dumps(plan)
                status, submitted = http_request(
                    port,
                    "POST",
                    "/api/v1/image-builds",
                    {
                        "operationId": case["operationId"],
                        "planToken": plan["planToken"],
                    },
                    {"Idempotency-Key": f"image-build-{index}"},
                )
                assert status == 202, (case["kind"], submitted)
                job = wait_for_job(port, str(submitted["data"]["jobId"]), process)
                assert job["state"] == "COMPLETED", (case["kind"], job)
                result = job["result"]
                assert result["schemaVersion"] == "1.0"
                assert result["kind"] == case["kind"]
                assert result["summary"] == plan["summary"]
                assert result["validation"]["valid"] is True
                server_bytes = (root_path / case["serverOutput"]).read_bytes()
                assert server_bytes == (root_path / case["cliOutput"]).read_bytes()
                assert result["sizeBytes"] == len(server_bytes)
                assert result["sha256"] == hashlib.sha256(server_bytes).hexdigest()

            cli_alteration_template = Path("manifests/cli/alteration.json")
            completed = subprocess.run(
                [
                    str(cli),
                    "alter",
                    "manifest",
                    "-o",
                    cli_alteration_template.as_posix(),
                ],
                cwd=root_path,
                capture_output=True,
                text=True,
            )
            assert completed.returncode == 0, (completed.stdout, completed.stderr)
            status, response = http_request(
                port, "POST", "/api/v1/alteration-manifest-templates", {}
            )
            assert status == 200, response
            alteration_template = response["data"]
            assert (
                alteration_template["canonicalJson"].encode()
                == (root_path / cli_alteration_template).read_bytes()
            )
            assert (
                json.loads(alteration_template["canonicalJson"])
                == (alteration_template["manifest"])
            )

            source_before = hashlib.sha256(
                (root_path / "all-actions-source.hds").read_bytes()
            ).hexdigest()
            cli_dry_run = subprocess.run(
                [
                    str(cli),
                    "alter",
                    "hds",
                    "all-actions-source.hds",
                    "all-actions.json",
                ],
                cwd=root_path,
                capture_output=True,
                text=True,
            )
            assert cli_dry_run.returncode == 0, (
                cli_dry_run.stdout,
                cli_dry_run.stderr,
            )
            cli_dry = json.loads(cli_dry_run.stdout)
            assert cli_dry["applied"] is False and len(cli_dry["operations"]) == 13

            cli_apply = subprocess.run(
                [
                    str(cli),
                    "alter",
                    "hds",
                    "all-actions-source.hds",
                    "all-actions.json",
                    "-o",
                    "builds/cli/all-actions.hds",
                ],
                cwd=root_path,
                capture_output=True,
                text=True,
            )
            assert cli_apply.returncode == 0, (cli_apply.stdout, cli_apply.stderr)
            cli_altered = json.loads(cli_apply.stdout)
            assert cli_altered["applied"] is True

            alteration_upload = upload_bytes(
                port,
                "all-actions.json",
                "manifest",
                (root_path / "all-actions.json").read_bytes(),
            )
            alteration_inspection_request = {
                "source": {
                    "rootId": "workspace",
                    "relativePath": "all-actions-source.hds",
                },
                "manifest": {"uploadRef": {"uploadId": alteration_upload}},
                "inputBindings": [
                    {
                        "manifestPath": "tone.wav",
                        "input": {"uploadRef": {"uploadId": tone_upload}},
                    }
                ],
            }
            status, response = http_request(
                port,
                "POST",
                "/api/v1/image-alteration-inspections",
                alteration_inspection_request,
            )
            assert status == 200, response
            alteration_inspection = response["data"]
            assert alteration_inspection["valid"] is True
            assert alteration_inspection["kind"] == "ALTERATION"
            assert alteration_inspection["summary"]["operationCount"] == 13
            assert alteration_inspection.get("warnings") == [], alteration_inspection
            assert alteration_inspection["validation"]["valid"] is True
            assert str(root_path) not in json.dumps(alteration_inspection)
            expected_types = {
                "delete_volume",
                "insert_volume",
                "delete_sbnk",
                "insert_sbnk",
                "insert_waveform",
                "delete_waveform",
                "rename_waveform",
                "rename_sbnk",
                "delete_sbac",
                "insert_sbac",
                "rename_sbac",
                "delete_program",
                "insert_program",
            }
            assert {
                str(operation["type"]).lower()
                for operation in alteration_inspection["operations"]
            } == expected_types
            assert [
                canonical_alteration_operation(operation)
                for operation in alteration_inspection["operations"]
            ] == [
                canonical_alteration_operation(operation)
                for operation in cli_dry["operations"]
            ]

            status, submitted = http_request(
                port,
                "POST",
                "/api/v1/image-alterations",
                {
                    **alteration_inspection_request,
                    "output": {
                        "rootId": "workspace",
                        "relativePath": "builds/server/all-actions.hds",
                    },
                },
                {"Idempotency-Key": "all-action-alteration"},
            )
            assert status == 202, submitted
            job = wait_for_job(port, str(submitted["data"]["jobId"]), process)
            assert job["state"] == "COMPLETED", job
            altered = job["result"]
            assert altered["schemaVersion"] == "1.0"
            assert altered["kind"] == "ALTERATION"
            assert altered["applied"] is True
            assert altered["operations"] == alteration_inspection["operations"]
            assert altered["summary"] == alteration_inspection["summary"]
            assert altered["warnings"] == []
            assert altered["validation"]["valid"] is True
            assert [
                canonical_alteration_operation(operation)
                for operation in altered["operations"]
            ] == [
                canonical_alteration_operation(operation)
                for operation in cli_altered["operations"]
            ]
            server_altered = root_path / "builds/server/all-actions.hds"
            assert (
                server_altered.read_bytes()
                == (root_path / "builds/cli/all-actions.hds").read_bytes()
            )
            assert (
                hashlib.sha256(
                    (root_path / "all-actions-source.hds").read_bytes()
                ).hexdigest()
                == source_before
            )

            direct_request = {
                **alteration_inspection_request,
                "output": {
                    "rootId": "workspace",
                    "relativePath": "builds/server/all-actions.hds",
                },
            }
            status, refused = http_request(
                port,
                "POST",
                "/api/v1/image-alterations",
                direct_request,
                {"Idempotency-Key": "all-action-alteration-refused"},
            )
            assert status == 202, refused
            refused_job = wait_for_job(port, str(refused["data"]["jobId"]), process)
            assert refused_job["state"] == "FAILED", refused_job
            assert refused_job["error"]["code"] == "output_exists", refused_job
            status, submitted = http_request(
                port,
                "POST",
                "/api/v1/image-alterations",
                {**direct_request, "overwrite": True},
                {"Idempotency-Key": "all-action-alteration-overwrite"},
            )
            assert status == 202, submitted
            job = wait_for_job(port, str(submitted["data"]["jobId"]), process)
            assert job["state"] == "COMPLETED", job
            assert (
                server_altered.read_bytes()
                == (root_path / "builds/cli/all-actions.hds").read_bytes()
            )

            missing_binding_request = {
                "kind": "FLOPPY",
                "manifest": {
                    "fileRef": {
                        "rootId": "workspace",
                        "relativePath": "authored.ima.json",
                    }
                },
                "output": {
                    "rootId": "workspace",
                    "relativePath": "builds/server/missing-binding.ima",
                },
            }
            status, missing_binding = http_request(
                port,
                "POST",
                "/api/v1/image-build-plans",
                missing_binding_request,
            )
            assert status == 422, missing_binding
            assert missing_binding["error"]["code"] == "missing_input_binding"

            extraction_index = 0

            def run_extraction(request: dict[str, Any]) -> dict[str, Any]:
                nonlocal extraction_index
                extraction_index += 1
                status, submitted = http_request(
                    port,
                    "POST",
                    "/api/v1/extractions",
                    request,
                    {"Idempotency-Key": f"extraction-{extraction_index}"},
                )
                assert status == 202, (request, submitted)
                job = wait_for_job(port, str(submitted["data"]["jobId"]), process)
                assert job["operationId"] == request["operationId"], job
                return job

            for mode in ("wav", "sfz"):
                server_destination = Path("extractions") / "server" / mode
                cli_destination = Path("extractions") / "cli" / mode
                job = run_extraction(
                    {
                        "operationId": f"extract.{mode}",
                        "sources": [
                            {"rootId": "workspace", "relativePath": "fixture.hds"}
                        ],
                        "destination": {
                            "rootId": "workspace",
                            "relativePath": server_destination.as_posix(),
                        },
                        "scope": "FILE",
                        "stereo": "AUTO",
                        "strict": False,
                        "overwrite": False,
                    }
                )
                assert job["state"] == "COMPLETED", job
                result = job["result"]
                assert result["mode"] == mode.upper(), result
                verify_extraction_result(root_path, result)
                completed = subprocess.run(
                    [
                        str(cli),
                        "extract",
                        mode,
                        "file",
                        "fixture.hds",
                        "-o",
                        cli_destination.as_posix(),
                        "--stereo",
                        "auto",
                        "--progress",
                        "never",
                    ],
                    cwd=root_path,
                    capture_output=True,
                    text=True,
                )
                assert completed.returncode == 0, (completed.stdout, completed.stderr)
                assert artifact_hashes(
                    root_path / server_destination
                ) == artifact_hashes(root_path / cli_destination)

            selector = "partition_00_New_Partition/New Volume/Sample Banks and Samples/sine wave"
            selected_server = Path("extractions/server/selected")
            selected_cli = Path("extractions/cli/selected")
            selected = run_extraction(
                {
                    "operationId": "extract.wav",
                    "sources": [{"rootId": "workspace", "relativePath": "fixture.hds"}],
                    "destination": {
                        "rootId": "workspace",
                        "relativePath": selected_server.as_posix(),
                    },
                    "scope": "SBNK",
                    "selectors": [{"path": selector}],
                    "stereo": "NONE",
                }
            )
            assert selected["state"] == "COMPLETED", selected
            verify_extraction_result(root_path, selected["result"])
            selected_cli_result = subprocess.run(
                [
                    str(cli),
                    "extract",
                    "wav",
                    "sbnk",
                    "fixture.hds",
                    "-o",
                    selected_cli.as_posix(),
                    "--stereo",
                    "none",
                    "--path",
                    selector,
                    "--progress",
                    "never",
                ],
                cwd=root_path,
                capture_output=True,
                text=True,
            )
            assert selected_cli_result.returncode == 0, (
                selected_cli_result.stdout,
                selected_cli_result.stderr,
            )
            assert artifact_hashes(root_path / selected_server) == artifact_hashes(
                root_path / selected_cli
            )

            strict_destination = Path("extractions/server/strict")
            strict = run_extraction(
                {
                    "operationId": "extract.wav",
                    "sources": [
                        {"rootId": "workspace", "relativePath": "malformed.bin"},
                        {"rootId": "workspace", "relativePath": "fixture.hds"},
                    ],
                    "destination": {
                        "rootId": "workspace",
                        "relativePath": strict_destination.as_posix(),
                    },
                    "scope": "FILE",
                    "strict": True,
                }
            )
            assert strict["state"] == "FAILED", strict
            assert not (root_path / strict_destination).exists()

            tolerant_destination = Path("extractions/server/tolerant")
            tolerant = run_extraction(
                {
                    "operationId": "extract.wav",
                    "sources": [
                        {"rootId": "workspace", "relativePath": "malformed.bin"},
                        {"rootId": "workspace", "relativePath": "fixture.hds"},
                    ],
                    "destination": {
                        "rootId": "workspace",
                        "relativePath": tolerant_destination.as_posix(),
                    },
                    "scope": "FILE",
                    "strict": False,
                }
            )
            assert tolerant["state"] == "COMPLETED", tolerant
            assert tolerant["result"]["loadErrorCount"] == 1, tolerant
            assert len(tolerant["result"]["warnings"]) == 1, tolerant
            verify_extraction_result(root_path, tolerant["result"])

            replace_destination = Path("extractions/server/replace")
            (root_path / replace_destination).mkdir(parents=True)
            preserved = root_path / replace_destination / "preserved.txt"
            preserved.write_text("preserved", encoding="utf-8")
            replace_request = {
                "operationId": "extract.wav",
                "sources": [{"rootId": "workspace", "relativePath": "fixture.hds"}],
                "destination": {
                    "rootId": "workspace",
                    "relativePath": replace_destination.as_posix(),
                },
                "scope": "FILE",
                "overwrite": False,
            }
            refused = run_extraction(replace_request)
            assert refused["state"] == "FAILED", refused
            assert preserved.read_text(encoding="utf-8") == "preserved"
            replace_request["overwrite"] = True
            replaced = run_extraction(replace_request)
            assert replaced["state"] == "COMPLETED", replaced
            assert not preserved.exists()
            verify_extraction_result(root_path, replaced["result"])

            status, ticket_response = http_request(
                port, "POST", "/api/v1/event-tickets"
            )
            assert status == 201
            ticket = str(ticket_response["data"]["ticket"])
            status, content, headers = raw_http_request(
                port, "POST", "/api/v1/event-tickets"
            )
            retry = json.loads(content)
            assert status == 429 and headers["retry-after"] == "1", (
                status,
                headers,
                retry,
            )
            assert retry["error"]["code"] == "event_ticket_capacity_exhausted"
            assert retry["error"]["retryable"] is True
            connection, headers, buffered = websocket_handshake(port, ticket)
            assert headers.startswith(b"HTTP/1.1 101"), headers
            assert f"Sec-WebSocket-Protocol: {SUBPROTOCOL}".encode() in headers

            status, rejected = http_request(
                port, "POST", "/api/v1/reports/info", {"unknownField": 7}
            )
            assert status == 400, rejected
            assert rejected["error"]["code"] == "invalid_request"
            status, submitted = http_request(
                port,
                "POST",
                "/api/v1/fixture-jobs",
                {"fixture": 7},
            )
            assert status == 202, submitted
            job_id = str(submitted["data"]["jobId"])
            events: list[dict[str, Any]] = []
            while not events or events[-1]["state"] not in {
                "COMPLETED",
                "FAILED",
                "CANCELLED",
            }:
                opcode, payload = receive_frame(connection, buffered)
                assert opcode == 1, (opcode, payload)
                event = json.loads(payload)
                if event["jobId"] == job_id:
                    events.append(event)
            assert [event["sequence"] for event in events] == list(
                range(1, len(events) + 1)
            )
            assert events[0]["state"] == "QUEUED"
            assert events[-1]["state"] == "COMPLETED"
            assert any(event["type"] == "progress" for event in events)

            status, snapshot = http_request(port, "GET", f"/api/v1/jobs/{job_id}")
            assert status == 200 and snapshot["data"]["result"]["fixture"] is True
            retained_after = events[-3]["sequence"] - 1
            status, replay = http_request(
                port,
                "GET",
                f"/api/v1/jobs/{job_id}/events?afterSequence={retained_after}",
            )
            assert status == 200 and replay["data"]["events"] == events[-3:]
            status, expired_replay = http_request(
                port, "GET", f"/api/v1/jobs/{job_id}/events?afterSequence=0"
            )
            assert status == 409, expired_replay
            assert expired_replay["error"]["code"] == "job_event_replay_expired"
            status, terminal = http_request(port, "DELETE", f"/api/v1/jobs/{job_id}")
            assert status == 200 and terminal["data"]["state"] == "COMPLETED"

            send_text(connection, "{}")
            opcode, payload = receive_frame(connection, buffered)
            assert opcode == 8 and struct.unpack("!H", payload[:2])[0] == 1008
            connection.close()

            status, binary_ticket = http_request(port, "POST", "/api/v1/event-tickets")
            assert status == 201, binary_ticket
            binary, headers, binary_buffered = websocket_handshake(
                port, str(binary_ticket["data"]["ticket"])
            )
            assert headers.startswith(b"HTTP/1.1 101"), headers
            send_client_frame(binary, 2, b"binary")
            opcode, payload = receive_frame(binary, binary_buffered)
            assert opcode == 8 and struct.unpack("!H", payload[:2])[0] == 1008
            binary.close()

            status, oversized_ticket = http_request(
                port, "POST", "/api/v1/event-tickets"
            )
            assert status == 201, oversized_ticket
            oversized, headers, oversized_buffered = websocket_handshake(
                port, str(oversized_ticket["data"]["ticket"])
            )
            assert headers.startswith(b"HTTP/1.1 101"), headers
            send_client_frame(oversized, 1, b"x" * 4097)
            try:
                opcode, payload = receive_frame(oversized, oversized_buffered)
                assert opcode == 8 and struct.unpack("!H", payload[:2])[0] == 1009
            except ConnectionResetError:
                # Crow documents max-payload enforcement as connection shutdown and
                # may reset before its 1009 close frame reaches the peer.
                pass
            oversized.close()

            status, unmasked_ticket = http_request(
                port, "POST", "/api/v1/event-tickets"
            )
            assert status == 201, unmasked_ticket
            unmasked, headers, unmasked_buffered = websocket_handshake(
                port, str(unmasked_ticket["data"]["ticket"])
            )
            assert headers.startswith(b"HTTP/1.1 101"), headers
            unmasked.sendall(b"\x81\x02{}")
            try:
                opcode, payload = receive_frame(unmasked, unmasked_buffered)
                assert opcode == 8 and struct.unpack("!H", payload[:2])[0] == 1002
            except ConnectionResetError:
                pass
            except AssertionError as error:
                assert str(error) == "WebSocket closed before a complete frame"
            unmasked.close()

            reused, headers, _ = websocket_handshake(port, ticket)
            assert headers.startswith(b"HTTP/1.1 401"), headers
            reused.close()

            status, _ = http_request(
                port, "GET", f"/api/v1/jobs/{job_id}/events?afterSequence=bad"
            )
            assert status == 400
            status, _ = http_request(port, "GET", "/api/v1/jobs/unknown")
            assert status == 404
            status, runtime_metrics = http_request(
                port, "GET", "/api/v1/system/metrics"
            )
            assert status == 200, runtime_metrics
            counters = runtime_metrics["data"]
            assert counters["submittedJobs"] >= 10, counters
            assert counters["completedJobs"] >= 9, counters
            assert counters["failedJobs"] >= 1, counters
            assert counters["publishedJobEvents"] >= counters["submittedJobs"] * 2, (
                counters
            )
            assert counters["progressJobEvents"] >= 1, counters
            assert counters["websocketEventsDelivered"] >= len(events), counters
            for name in (
                "queuedJobs",
                "runningJobs",
                "cancelledJobs",
                "totalJobQueueWaitMs",
                "totalJobExecutionMs",
                "totalJobPhaseDurationMs",
                "totalJobCancellationLatencyMs",
                "websocketEventsDropped",
                "websocketEventsPending",
                "websocketClientsEvicted",
            ):
                assert isinstance(counters[name], int) and counters[name] >= 0, (
                    name,
                    counters,
                )
            status, shutdown = http_request(port, "POST", "/api/v1/system/shutdown")
            assert status == 202 and shutdown["data"]["accepted"] is True, shutdown
            shutdown_requested = True
            process.wait(timeout=5)
            assert process.returncode == 0
            assert not connection_path.exists()
            server_log.flush()
            server_output = server_log_path.read_text(
                encoding="utf-8", errors="replace"
            )
            request_logs = [
                json.loads(line)
                for line in server_output.splitlines()
                if line.startswith('{"durationMs"') and '"event":"http_request"' in line
            ]
            assert request_logs
            assert any(
                entry["path"] == "/api/v1/system/metrics" for entry in request_logs
            )
            assert all("?" not in entry["path"] for entry in request_logs)
            preflight_logs = [
                entry for entry in request_logs if entry["method"] == "OPTIONS"
            ]
            assert preflight_logs
            assert all(
                0 <= entry["durationMs"] < 10_000 for entry in preflight_logs
            ), preflight_logs
            audit_logs = [
                json.loads(line)
                for line in server_output.splitlines()
                if '"event":"security_audit"' in line
            ]
            assert any(
                entry["action"] == "authentication" and entry["outcome"] == "denied"
                for entry in audit_logs
            )
            assert any(
                entry["action"] == "upload_materialize"
                and entry["outcome"] == "allowed"
                for entry in audit_logs
            )
            assert any(
                entry["action"] == "file_download" and entry["outcome"] == "allowed"
                for entry in audit_logs
            )
            assert TOKEN not in server_output
            assert str(root_path) not in server_output
            assert '"relativePath"' not in server_output
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            if sys.exc_info()[0] is not None:
                server_log.flush()
                print(
                    server_log_path.read_text(encoding="utf-8", errors="replace"),
                    file=sys.stderr,
                )
            server_log.close()
            if shutdown_requested:
                assert not connection_path.exists()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    arguments = parser.parse_args()
    exercise(arguments.server, arguments.cli, arguments.fixture)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
