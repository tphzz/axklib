from __future__ import annotations

import json
import wave
from pathlib import Path

import pytest

from axklib.containers import AxklibContainer, AxklibContainerLoadResult, OpenOptions, open_many
from axklib.containers.sfs_allocation import (
    AllocationMismatchRange,
    AllocationPartitionSummary,
    AllocationReport,
)
from axklib.model import AxklibObject, AxklibObjectHeader
from axklib.validation import (
    ValidationIssue,
    ValidationScope,
    ValidationSeverity,
    _sfs_allocation_issues,
    validate_container,
    validate_export_sidecars,
    validation_failed,
)


def _object(payload: bytes, payload_size: int | None = None) -> AxklibObject:
    return AxklibObject(
        image="fixture.hds",
        container_kind="sfs",
        scope_key="fixture:partition:0",
        object_key="p0:sfs1",
        partition_index=0,
        sfs_id=1,
        fat_file="",
        payload_offset=0,
        payload_size=len(payload) if payload_size is None else payload_size,
        type="SMPL",
        name="S01",
        payload=payload,
    )


def _container(*objects: AxklibObject) -> AxklibContainer:
    return AxklibContainer(
        source_path=__import__("pathlib").Path("fixture.hds"),
        kind="sfs",
        detected_format="sfs",
        objects=objects,
        recovery_quality_summary={},
    )


def test_validation_emits_stable_bad_magic_issue_code() -> None:
    report = validate_container(_container(_object(b"not-an-object")))

    assert any(issue.code == "OBJECT_BAD_MAGIC" for issue in report.issues)
    assert report.failed is True


def test_validation_emits_stable_payload_truncated_issue_code() -> None:
    payload = bytearray(0x200)
    payload[0:12] = b"FSFSDEV3SPLX"
    payload[0x0C:0x10] = b"SMPL"
    payload[0x10:0x14] = (0x200).to_bytes(4, "big")
    payload[0x1C:0x20] = (16).to_bytes(4, "big")
    item = _object(bytes(payload[:0x200]))

    report = validate_container(_container(item))

    assert any(issue.code == "OBJECT_PAYLOAD_TRUNCATED" for issue in report.issues)



def _partial_fat_smpl(path: Path, object_key: str, available_payload: int) -> AxklibObject:
    header_size = 0x42
    stored_payload_size = 10
    payload = bytearray(header_size + available_payload)
    payload[0:12] = b"FSFSDEV3SPLX"
    payload[0x0C:0x10] = b"SMPL"
    payload[0x10:0x14] = header_size.to_bytes(4, "big")
    payload[0x1C:0x20] = stored_payload_size.to_bytes(4, "big")
    payload[0x32 : 0x32 + 4] = b"S134"
    return AxklibObject(
        image=str(path),
        container_kind="fat12_floppy",
        scope_key=f"{path}:fat-root",
        object_key=object_key,
        partition_index=None,
        sfs_id=None,
        fat_file="SMP_0134.002",
        payload_offset=0,
        payload_size=len(payload),
        type="SMPL",
        name="S134",
        payload=bytes(payload),
        header=AxklibObjectHeader(
            header_size=header_size,
            stored_payload_size=stored_payload_size,
            raw_prefix_hex=bytes(payload[:64]).hex(),
        ),
    )


def test_validate_paths_reports_probable_multi_floppy_span(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    disk1 = tmp_path / "hardcore-sounds-disk1.ima"
    disk2 = tmp_path / "hardcore-sounds-disk2.ima"
    disk1.write_bytes(b"disk1")
    disk2.write_bytes(b"disk2")
    container1 = AxklibContainer(
        source_path=disk1,
        kind="fat12_floppy",
        detected_format="fat12_floppy",
        objects=(_partial_fat_smpl(disk1, "disk1:SMP_0134.002", 4),),
        recovery_quality_summary={},
    )
    container2 = AxklibContainer(
        source_path=disk2,
        kind="fat12_floppy",
        detected_format="fat12_floppy",
        objects=(_partial_fat_smpl(disk2, "disk2:SMP_0134.002", 6),),
        recovery_quality_summary={},
    )
    monkeypatch.setattr(
        "axklib.validation.open_many",
        lambda _paths, options=None: [
            AxklibContainerLoadResult(path=disk1, container=container1),
            AxklibContainerLoadResult(path=disk2, container=container2),
        ],
    )

    report = __import__("axklib.validation", fromlist=["validate_paths"]).validate_paths([disk1, disk2])

    codes = {issue.code for issue in report.issues}
    assert "OBJECT_FLOPPY_SPANNED_PAYLOAD_PARTIAL" in codes
    assert "OBJECT_PAYLOAD_TRUNCATED" not in codes

def test_validation_policy_strict_fails_on_warnings() -> None:
    warning = [
        ValidationIssue(
            severity=ValidationSeverity.WARNING,
            code="REL_AMBIGUOUS_TARGET",
            message="fixture",
            scope=ValidationScope.RELATIONSHIP,
            source_path="fixture.hds",
        )
    ]

    assert validation_failed(warning, "normal") is False
    assert validation_failed(warning, "strict") is True


def test_validate_export_sidecars_checks_wav_header(tmp_path) -> None:
    wav_path = tmp_path / "sample.wav"
    with wave.open(str(wav_path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(44100)
        wav.writeframes(b"\x00\x00\x01\x00")
    sidecar = {
        "source_container": "fixture.hds",
        "object_key": "p0:sfs1",
        "wav_path": str(wav_path),
        "sample_rate": 44100,
        "channels": 1,
        "sample_width_bytes": 2,
        "frames": 2,
        "stored_payload_size": 4,
        "extraction_quality": "Known",
        "extraction_basis": "test",
        "field_quality": {},
    }
    (tmp_path / "sample.json").write_text(json.dumps(sidecar), encoding="utf-8")

    report = validate_export_sidecars(tmp_path)

    assert report.issue_count == 0
    assert report.failed is False


def test_validation_issue_codes_use_stable_namespaces() -> None:
    report = validate_container(_container(_object(b"not-an-object")))
    allowed_prefixes = ("CONTAINER_", "SFS_", "OBJECT_", "REL_", "AUDIO_", "REPORT_", "EXPORT_", "SIDECAR_", "CLI_", "ISO_")

    assert report.issues
    assert all(issue.code.startswith(allowed_prefixes) for issue in report.issues)


def test_open_many_returns_structured_load_failure(tmp_path) -> None:
    missing = tmp_path / "missing.hds"

    results = open_many([missing], options=OpenOptions(strict=True))

    assert len(results) == 1
    result = results[0]
    assert result.container is None
    assert result.error is not None
    assert result.error.error_code == "AXKLIB_CONTAINER_OPEN_FAILED"
    assert result.error.path == missing
    assert result.error.original_exception
    assert result.error.recoverable is False


def test_sfs_allocation_analysis_is_bridged_to_stable_validation_issues(
    tmp_path, monkeypatch
) -> None:
    summary = AllocationPartitionSummary(
        source_image="fixture.hds",
        partition_index=0,
        partition_name="hd0",
        start_sector=0,
        sectors_per_cluster=2,
        cluster_count=100,
        bitmap_offset=0,
        index_offset=0,
        scanned_index_bytes=0,
        valid_index_record_count=0,
        invalid_extent_record_count=0,
        direct_extent_record_count=0,
        continuation_extent_record_count=0,
        data_extent_count=0,
        continuation_list_cluster_count=0,
        stored_used_cluster_count=10,
        reconstructed_used_cluster_count=9,
        stored_used_not_reconstructed_count=1,
        reconstructed_used_not_stored_count=2,
        extent_total_mismatch_count=3,
        warning_count=1,
        warnings="fixture warning",
    )
    mismatch = AllocationMismatchRange(
        source_image="fixture.hds",
        partition_index=0,
        direction="stored_used_not_reconstructed",
        start_cluster=4,
        end_cluster=5,
        cluster_count=2,
    )

    from axklib.containers import sfs_allocation

    monkeypatch.setattr(
        sfs_allocation,
        "analyze_image",
        lambda _path: AllocationReport((summary,), (), (mismatch,)),
    )

    issues = _sfs_allocation_issues(tmp_path / "fixture.hds")
    codes = {issue.code for issue in issues}

    assert "SFS_ALLOCATION_STORED_USED_NOT_RECONSTRUCTED" in codes
    assert "SFS_ALLOCATION_RECONSTRUCTED_USED_NOT_STORED" in codes
    assert "SFS_EXTENT_TOTAL_MISMATCH" in codes
    assert "SFS_ALLOCATION_WARNING" in codes
    assert "SFS_ALLOCATION_MISMATCH_RANGE" in codes
    assert any(issue.severity == ValidationSeverity.ERROR for issue in issues)


def test_validate_export_sidecars_accepts_v2_structured_sidecar(tmp_path) -> None:
    wav_path = tmp_path / "src0001_fixture" / "Samples" / "sample.wav"
    wav_path.parent.mkdir(parents=True)
    with wave.open(str(wav_path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(44100)
        wav.writeframes(b"\x00\x00\x01\x00")
    sidecar = {
        "schema": "axklib.wave_sidecar.v2",
        "identity": {"object_key": "p0:sfs1"},
        "audio": {
            "wav_path": "src0001_fixture/Samples/sample.wav",
            "sample_rate": 44100,
            "channels": 1,
            "sample_width_bytes": 2,
            "frames": 2,
        },
        "playback": {},
        "relationships": {},
        "parameters": {},
        "conversion": {},
        "origin": {},
    }
    (wav_path.with_suffix(".json")).write_text(json.dumps(sidecar), encoding="utf-8")

    report = validate_export_sidecars(tmp_path)

    assert report.issue_count == 0


def test_validate_export_sidecars_rejects_v2_path_escape(tmp_path) -> None:
    sidecar = {
        "schema": "axklib.wave_sidecar.v2",
        "identity": {"object_key": "p0:sfs1"},
        "audio": {"wav_path": "../outside.wav"},
        "playback": {},
        "relationships": {},
        "parameters": {},
        "conversion": {},
        "origin": {},
    }
    (tmp_path / "bad.json").write_text(json.dumps(sidecar), encoding="utf-8")

    report = validate_export_sidecars(tmp_path)

    assert any(issue.code == "EXPORT_SIDECAR_PATH_ESCAPE" for issue in report.issues)

def _write_test_wav(path, *, channels: int = 1) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(channels)
        wav.setsampwidth(2)
        wav.setframerate(44100)
        frame = b"\x00\x00" * channels
        wav.writeframes(frame * 2)


def test_validate_export_sidecars_checks_stereo_decision_outputs(tmp_path) -> None:
    _write_test_wav(tmp_path / "Samples" / "Stereo.wav", channels=2)
    (tmp_path / "_schemas").mkdir()
    (tmp_path / "_schemas" / "stereo_decisions.schema.json").write_text("{}", encoding="utf-8")
    (tmp_path / "samples.json").write_text(
        json.dumps([
            {"wav_path": "Samples/Stereo.wav"},
        ]),
        encoding="utf-8",
    )
    (tmp_path / "stereo_decisions.json").write_text(
        json.dumps([
            {
                "decision": "interleaved_stereo_written",
                "sbnk_object_key": "p0:sfs10",
                "stereo_wav_path": "Samples/Stereo.wav",
                "left_mono_wav_path": "",
                "right_mono_wav_path": "",
            }
        ]),
        encoding="utf-8",
    )

    report = validate_export_sidecars(tmp_path)

    assert not any(issue.code.startswith("EXPORT_STEREO_") for issue in report.issues)


def test_validate_export_sidecars_rejects_stale_suppressed_mono_listing(tmp_path) -> None:
    _write_test_wav(tmp_path / "Samples" / "Stereo.wav", channels=2)
    _write_test_wav(tmp_path / "Waveforms" / "Left.wav", channels=1)
    (tmp_path / "_schemas").mkdir()
    (tmp_path / "_schemas" / "stereo_decisions.schema.json").write_text("{}", encoding="utf-8")
    (tmp_path / "samples.json").write_text(
        json.dumps([
            {"wav_path": "Samples/Stereo.wav"},
            {"wav_path": "Waveforms/Left.wav"},
        ]),
        encoding="utf-8",
    )
    (tmp_path / "stereo_decisions.json").write_text(
        json.dumps([
            {
                "decision": "interleaved_stereo_written",
                "sbnk_object_key": "p0:sfs10",
                "stereo_wav_path": "Samples/Stereo.wav",
                "left_mono_wav_path": "Waveforms/Left.wav",
                "right_mono_wav_path": "",
            }
        ]),
        encoding="utf-8",
    )

    report = validate_export_sidecars(tmp_path)

    assert any(issue.code == "EXPORT_STEREO_SUPPRESSED_MONO_STILL_LISTED" for issue in report.issues)


def test_validate_export_sidecars_rejects_nonexact_missing_mono(tmp_path) -> None:
    (tmp_path / "_schemas").mkdir()
    (tmp_path / "_schemas" / "stereo_decisions.schema.json").write_text("{}", encoding="utf-8")
    (tmp_path / "samples.json").write_text("[]", encoding="utf-8")
    (tmp_path / "stereo_decisions.json").write_text(
        json.dumps([
            {
                "decision": "kept_mono_not_exact_representable",
                "sbnk_object_key": "p0:sfs10",
                "stereo_wav_path": "",
                "left_mono_wav_path": "Waveforms/Left.wav",
                "right_mono_wav_path": "Waveforms/Right.wav",
            }
        ]),
        encoding="utf-8",
    )

    report = validate_export_sidecars(tmp_path)

    assert any(issue.code == "EXPORT_STEREO_MONO_MISSING_FOR_NONEXACT" for issue in report.issues)


def test_validate_export_sidecars_rejects_stereo_decision_without_schema(tmp_path) -> None:
    _write_test_wav(tmp_path / "Samples" / "Stereo.wav", channels=2)
    (tmp_path / "samples.json").write_text(json.dumps([{"wav_path": "Samples/Stereo.wav"}]), encoding="utf-8")
    (tmp_path / "stereo_decisions.json").write_text(
        json.dumps([
            {
                "decision": "interleaved_stereo_written",
                "sbnk_object_key": "p0:sfs10",
                "stereo_wav_path": "Samples/Stereo.wav",
            }
        ]),
        encoding="utf-8",
    )

    report = validate_export_sidecars(tmp_path)

    assert any(issue.code == "EXPORT_STEREO_DECISION_SCHEMA_MISSING" for issue in report.issues)



def test_validate_export_sidecars_rejects_volume_graph_path_escape(tmp_path) -> None:
    graph = {
        "schema": "axklib.volume_graph.v1",
        "objects": {
            "smpl": [
                {
                    "id": "SMPL:p0_sfs1",
                    "source_ref": "fixture.hds␟p0:sfs1",
                    "wav_path": "../escape.wav",
                    "audio": {"channels": 1},
                }
            ]
        },
        "rendered_audio": [],
    }
    (tmp_path / "volume.axklib.json").write_text(json.dumps(graph), encoding="utf-8")

    report = validate_export_sidecars(tmp_path)

    assert any(issue.code == "EXPORT_GRAPH_PATH_ESCAPE" for issue in report.issues)
