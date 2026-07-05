from __future__ import annotations

import json
import wave
from dataclasses import replace
from pathlib import Path

import pytest

from axklib.audio import (
    WaveformPlacement,
    WaveformRelationship,
    WavExportRequest,
    decode_container_waveforms,
    decode_waveform,
    export_waveforms,
)
from axklib.model import AxklibObject, DataQuality


def _put_be16(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 2] = value.to_bytes(2, "big")


def _put_be32(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 4] = value.to_bytes(4, "big")


def _sample_object(pcm_be: bytes = b"\x00\x01\x00\x02") -> AxklibObject:
    payload = bytearray(0x200 + len(pcm_be))
    payload[0:12] = b"FSFSDEV3SPLX"
    payload[0x0C:0x10] = b"SMPL"
    _put_be32(payload, 0x10, 0x200)
    _put_be32(payload, 0x18, 0x7C)
    _put_be32(payload, 0x1C, len(pcm_be))
    _put_be32(payload, 0x20, len(pcm_be))
    _put_be16(payload, 0x28, 44100)
    _put_be16(payload, 0x2A, 2)
    payload[0x32 : 0x32 + 3] = b"S01"
    _put_be16(payload, 0x7C, 44100)
    payload[0x7E] = 64
    payload[0x7F] = 0
    payload[0x85] = 4
    _put_be32(payload, 0x92, 2)
    _put_be32(payload, 0x96, 0)
    _put_be32(payload, 0x9A, 2)
    payload[0x200 : 0x200 + len(pcm_be)] = pcm_be
    return AxklibObject(
        image="fixture.hds",
        container_kind="sfs",
        scope_key="fixture:partition:0",
        object_key="p0:sfs1",
        partition_index=0,
        sfs_id=1,
        fat_file="",
        payload_offset=0,
        payload_size=len(payload),
        type="SMPL",
        name="S01",
        payload=bytes(payload),
    )


def _graph(path: Path) -> dict[str, object]:
    return json.loads(path.read_text(encoding="utf-8"))


def _volume_graphs(root: Path) -> list[dict[str, object]]:
    return [_graph(path) for path in sorted(root.rglob("volume.axklib.json"))]


def test_decode_waveform_is_pure_and_byteswaps_current_16_bit_payload() -> None:
    waveform = decode_waveform(_sample_object())

    assert waveform.sample_rate == 44100
    assert waveform.sample_width_bytes == 2
    assert waveform.frame_count == 2
    assert waveform.pcm == b"\x01\x00\x02\x00"
    assert waveform.exactness_status == "exact-current-mono"
    assert waveform.field_quality["stored_payload_size"]["quality"] == "Known"


def test_export_waveforms_writes_object_graph_and_wav_only_smpl(tmp_path: Path) -> None:
    waveform = decode_waveform(_sample_object())
    result = export_waveforms(WavExportRequest(output_dir=tmp_path, waveforms=(waveform,)))

    wav_paths = [path for path in result.written_files if path.suffix == ".wav"]
    assert [path.relative_to(tmp_path).as_posix() for path in wav_paths] == [
        "_unplaced/SMPL/S01.wav"
    ]
    assert not list((tmp_path / "_unplaced" / "SMPL").glob("*.json"))
    graph_path = tmp_path / "_unplaced" / "volume.axklib.json"
    graph = _graph(graph_path)
    assert graph["schema"] == "axklib.volume_graph.v1"
    assert graph["objects"]["smpl"][0]["wav_path"] == "SMPL/S01.wav"
    assert graph["objects"]["smpl"][0]["playback"]["root_key_midi"] == 64
    with wave.open(str(wav_paths[0]), "rb") as wav:
        assert wav.getframerate() == 44100
        assert wav.getsampwidth() == 2
        assert wav.getnframes() == 2


def test_export_waveforms_rejects_existing_targets_without_overwrite(tmp_path: Path) -> None:
    waveform = decode_waveform(_sample_object())
    export_waveforms(WavExportRequest(output_dir=tmp_path, waveforms=(waveform,)))

    with pytest.raises(FileExistsError):
        export_waveforms(WavExportRequest(output_dir=tmp_path, waveforms=(waveform,)))


def test_export_waveforms_replaces_existing_targets_with_explicit_overwrite(tmp_path: Path) -> None:
    first_waveform = decode_waveform(_sample_object(b"\x00\x01\x00\x02"))
    second_waveform = decode_waveform(_sample_object(b"\x00\x03\x00\x04"))
    export_waveforms(WavExportRequest(output_dir=tmp_path, waveforms=(first_waveform,)))
    result = export_waveforms(
        WavExportRequest(
            output_dir=tmp_path,
            waveforms=(second_waveform,),
            overwrite_policy="replace",
        )
    )

    wav_path = next(path for path in result.written_files if path.suffix == ".wav")
    with wave.open(str(wav_path), "rb") as wav:
        assert wav.readframes(wav.getnframes()) == b"\x03\x00\x04\x00"


def test_export_waveforms_reports_structured_progress(tmp_path: Path) -> None:
    waveform = decode_waveform(_sample_object())
    events = []

    export_waveforms(
        WavExportRequest(
            output_dir=tmp_path,
            waveforms=(waveform,),
            progress_callback=events.append,
        )
    )

    assert events[0].stage == "exporting"
    assert events[0].completed == 0
    assert events[0].total == 2
    assert any(event.message.endswith("SMPL/S01.wav") for event in events)
    assert any(event.message.startswith("building ") for event in events)
    assert events[-1].completed == events[-1].total == 2
    assert events[-1].message.endswith("volume.axklib.json")


def test_decode_container_waveforms_preserves_decode_issue_for_bad_sample() -> None:
    from axklib.containers import AxklibContainer

    bad = _sample_object()
    truncated = AxklibObject(
        image=bad.image,
        container_kind=bad.container_kind,
        scope_key=bad.scope_key,
        object_key=bad.object_key,
        partition_index=bad.partition_index,
        sfs_id=bad.sfs_id,
        fat_file=bad.fat_file,
        payload_offset=bad.payload_offset,
        payload_size=0x200,
        type=bad.type,
        name=bad.name,
        payload=bad.payload[:0x200],
    )
    container = AxklibContainer(
        source_path=Path("fixture.hds"),
        kind="sfs",
        detected_format="sfs",
        objects=(truncated,),
        recovery_quality_summary={},
    )

    result = decode_container_waveforms(container)

    assert result.waveforms == ()
    assert [issue.code for issue in result.issues] == ["WAVEFORM_DECODE_FAILED"]


def test_stereo_interleave_lives_in_audio_library() -> None:
    from axklib.audio.stereo import interleave

    assert interleave(b"\x01\x00\x02\x00", b"\x03\x00\x04\x00", 2) == (
        b"\x01\x00\x03\x00\x02\x00\x04\x00"
    )


def test_object_graph_references_known_stereo_from_sbnk(tmp_path: Path) -> None:
    left = decode_waveform(_sample_object(b"\x00\x01\x00\x02"))
    right = replace(
        left, object_key="p0:sfs2", object_offset=512, sample_name="S01 R", pcm=b"\x03\x00\x04\x00"
    )
    placements = {
        "p0:sfs10": WaveformPlacement(
            partition_index=0,
            partition_name="hd1",
            volume_name="Vol 1",
            category_name="Samples",
            display_name="S01 Stereo",
            quality=DataQuality.KNOWN,
            source="SFS volume inventory",
            relationship_path="SBNK-category-entry",
        ),
    }
    relationships = (
        WaveformRelationship(
            source_key="p0:sfs10",
            target_key=left.object_key,
            relationship_type="SBNK_LEFT_MEMBER_TO_SMPL",
            quality="Known",
            basis="sbnk-member-link+name",
            source_image=left.source_image,
        ),
        WaveformRelationship(
            source_key="p0:sfs10",
            target_key=right.object_key,
            relationship_type="SBNK_RIGHT_MEMBER_TO_SMPL",
            quality="Known",
            basis="sbnk-member-link+name",
            source_image=right.source_image,
        ),
    )

    result = export_waveforms(
        WavExportRequest(
            output_dir=tmp_path,
            waveforms=(left, right),
            stereo_policy="auto",
            placements=placements,
            relationships=relationships,
        )
    )

    wavs = sorted(
        path.relative_to(tmp_path).as_posix()
        for path in result.written_files
        if path.suffix == ".wav"
    )
    assert wavs == [
        "partition_00_hd1/Vol 1/RENDERED/S01 Stereo.wav",
        "partition_00_hd1/Vol 1/SMPL/S01 R.wav",
        "partition_00_hd1/Vol 1/SMPL/S01.wav",
    ]
    graph = _graph(tmp_path / "partition_00_hd1" / "Vol 1" / "volume.axklib.json")
    sbnk = graph["objects"]["sbnk"][0]
    assert sbnk["display_name"] == "S01 Stereo"
    assert [row["role"] for row in sbnk["physical_waveforms"]] == ["left", "right"]
    assert sbnk["rendered_audio"]["wav_path"] == "RENDERED/S01 Stereo.wav"
    assert graph["rendered_audio"][0]["wav_path"] == "RENDERED/S01 Stereo.wav"
    with wave.open(
        str(tmp_path / "partition_00_hd1" / "Vol 1" / "RENDERED" / "S01 Stereo.wav"), "rb"
    ) as wav:
        assert wav.getnchannels() == 2
        assert wav.getnframes() == 2


def test_paired_sbnk_siblings_render_additive_stereo(tmp_path: Path) -> None:
    left = decode_waveform(_sample_object(b"\x00\x01\x00\x02"))
    right = replace(
        left,
        object_key="p0:sfs2",
        object_offset=512,
        sample_name="SMP 002411 -L",
        pcm=b"\x03\x00\x04\x00",
    )
    placements = {
        "p0:sbac1": WaveformPlacement(
            partition_index=0,
            partition_name="hd1",
            volume_name="Ap11 Grand 1",
            category_name="Sample Banks",
            display_name="B 01 Se_ff<St",
            quality=DataQuality.KNOWN,
            source="test placement",
        ),
        "p0:sbnk-l": WaveformPlacement(
            partition_index=0,
            partition_name="hd1",
            volume_name="Ap11 Grand 1",
            category_name="Sample Banks",
            display_name="Se_ff_024 -L",
            quality=DataQuality.KNOWN,
            source="test placement",
        ),
        "p0:sbnk-r": WaveformPlacement(
            partition_index=0,
            partition_name="hd1",
            volume_name="Ap11 Grand 1",
            category_name="Sample Banks",
            display_name="Se_ff_024 -R",
            quality=DataQuality.KNOWN,
            source="test placement",
        ),
    }
    relationships = (
        WaveformRelationship(
            "p0:sbnk-l",
            left.object_key,
            "SBNK_LEFT_MEMBER_TO_SMPL",
            "Known",
            "sbnk-member-link+name",
            source_image=left.source_image,
        ),
        WaveformRelationship(
            "p0:sbnk-r",
            right.object_key,
            "SBNK_LEFT_MEMBER_TO_SMPL",
            "Known",
            "sbnk-member-link+name",
            source_image=right.source_image,
        ),
        WaveformRelationship(
            "p0:sbac1",
            "p0:sbnk-l",
            "SBAC_SLOT_TO_SBNK",
            "Known",
            "sample-bank-slot",
            source_image=left.source_image,
        ),
        WaveformRelationship(
            "p0:sbac1",
            "p0:sbnk-r",
            "SBAC_SLOT_TO_SBNK",
            "Known",
            "sample-bank-slot",
            source_image=right.source_image,
        ),
    )

    result = export_waveforms(
        WavExportRequest(
            output_dir=tmp_path,
            waveforms=(left, right),
            placements=placements,
            relationships=relationships,
        )
    )

    wavs = sorted(
        path.relative_to(tmp_path).as_posix()
        for path in result.written_files
        if path.suffix == ".wav"
    )
    assert wavs == [
        "partition_00_hd1/Ap11 Grand 1/RENDERED/Se_ff_024.wav",
        "partition_00_hd1/Ap11 Grand 1/SMPL/S01.wav",
        "partition_00_hd1/Ap11 Grand 1/SMPL/SMP 002411 -L.wav",
    ]
    rendered_path = tmp_path / "partition_00_hd1" / "Ap11 Grand 1" / "RENDERED" / "Se_ff_024.wav"
    with wave.open(str(rendered_path), "rb") as wav:
        assert wav.getnchannels() == 2
        assert wav.getnframes() == 2
        assert wav.readframes(2) == b"\x01\x00\x03\x00\x02\x00\x04\x00"
    graph = _graph(tmp_path / "partition_00_hd1" / "Ap11 Grand 1" / "volume.axklib.json")
    assert sorted(row["display_name"] for row in graph["objects"]["sbnk"]) == [
        "Se_ff_024 -L",
        "Se_ff_024 -R",
    ]
    assert sorted(row["display_name"] for row in graph["objects"]["smpl"]) == [
        "S01",
        "SMP 002411 -L",
    ]
    assert graph["rendered_audio"][0]["wav_path"] == "RENDERED/Se_ff_024.wav"
    assert graph["stereo_decisions"][0]["sample_name"] == "Se_ff_024"
    assert graph["stereo_decisions"][0]["reason_code"] == "STEREO_EXACT_INTERLEAVED"
    assert "same-sbac-sbnk-name-lr-pair" in graph["stereo_decisions"][0]["basis"]


def test_duplicate_paired_sbnk_render_decisions_reuse_one_wav(tmp_path: Path) -> None:
    left = decode_waveform(_sample_object(b"\x00\x01\x00\x02"))
    right = replace(
        left,
        object_key="p0:sfs2",
        object_offset=512,
        sample_name="SMP 002411 -L",
        pcm=b"\x03\x00\x04\x00",
    )
    placements = {
        key: WaveformPlacement(
            partition_index=0,
            partition_name="hd1",
            volume_name="Ap11 Grand 1",
            category_name="Sample Banks",
            display_name=display,
            quality=DataQuality.KNOWN,
            source="test placement",
        )
        for key, display in {
            "p0:sbac1": "B 01 Se_ff<St",
            "p0:sbac2": "B 01 Se_ff<St *",
            "p0:sbnk-l1": "Se_ff_024 -L",
            "p0:sbnk-r1": "Se_ff_024 -R",
            "p0:sbnk-l2": "Se_ff_024 -L*",
            "p0:sbnk-r2": "Se_ff_024 -R*",
        }.items()
    }
    relationships = (
        WaveformRelationship(
            "p0:sbnk-l1",
            left.object_key,
            "SBNK_LEFT_MEMBER_TO_SMPL",
            "Known",
            "sbnk-member-link+name",
            source_image=left.source_image,
        ),
        WaveformRelationship(
            "p0:sbnk-r1",
            right.object_key,
            "SBNK_LEFT_MEMBER_TO_SMPL",
            "Known",
            "sbnk-member-link+name",
            source_image=right.source_image,
        ),
        WaveformRelationship(
            "p0:sbnk-l2",
            left.object_key,
            "SBNK_LEFT_MEMBER_TO_SMPL",
            "Known",
            "sbnk-member-link+name",
            source_image=left.source_image,
        ),
        WaveformRelationship(
            "p0:sbnk-r2",
            right.object_key,
            "SBNK_LEFT_MEMBER_TO_SMPL",
            "Known",
            "sbnk-member-link+name",
            source_image=right.source_image,
        ),
        WaveformRelationship(
            "p0:sbac1",
            "p0:sbnk-l1",
            "SBAC_SLOT_TO_SBNK",
            "Known",
            "sample-bank-slot",
            source_image=left.source_image,
        ),
        WaveformRelationship(
            "p0:sbac1",
            "p0:sbnk-r1",
            "SBAC_SLOT_TO_SBNK",
            "Known",
            "sample-bank-slot",
            source_image=right.source_image,
        ),
        WaveformRelationship(
            "p0:sbac2",
            "p0:sbnk-l2",
            "SBAC_SLOT_TO_SBNK",
            "Known",
            "sample-bank-slot",
            source_image=left.source_image,
        ),
        WaveformRelationship(
            "p0:sbac2",
            "p0:sbnk-r2",
            "SBAC_SLOT_TO_SBNK",
            "Known",
            "sample-bank-slot",
            source_image=right.source_image,
        ),
    )

    result = export_waveforms(
        WavExportRequest(
            output_dir=tmp_path,
            waveforms=(left, right),
            placements=placements,
            relationships=relationships,
        )
    )

    rendered_wavs = sorted(
        path.relative_to(tmp_path).as_posix()
        for path in result.written_files
        if path.suffix == ".wav" and path.parent.name == "RENDERED"
    )
    assert rendered_wavs == ["partition_00_hd1/Ap11 Grand 1/RENDERED/Se_ff_024.wav"]
    graph = _graph(tmp_path / "partition_00_hd1" / "Ap11 Grand 1" / "volume.axklib.json")
    decision_paths = {row["stereo_wav_path"] for row in graph["stereo_decisions"]}
    assert decision_paths == {"partition_00_hd1/Ap11 Grand 1/RENDERED/Se_ff_024.wav"}
    assert {row["wav_path"] for row in graph["rendered_audio"]} == {"RENDERED/Se_ff_024.wav"}


def test_physical_smpl_duplicate_marker_keeps_exact_path_and_records_alias(
    tmp_path: Path,
) -> None:
    waveform = replace(
        decode_waveform(_sample_object(b"\x00\x01\x00\x02")),
        sample_name="JP6 FatBs1b036 *",
    )
    placements = {
        "p0:sbnk1": WaveformPlacement(
            partition_index=0,
            partition_name="hd1",
            volume_name="Sy42 Ana4Bass",
            category_name="Sample Banks",
            display_name="J FatBs1b036",
            quality=DataQuality.KNOWN,
            source="test placement",
        ),
    }
    relationships = (
        WaveformRelationship(
            "p0:sbnk1",
            waveform.object_key,
            "SBNK_LEFT_MEMBER_TO_SMPL",
            "Known",
            "sbnk-member-link+name",
            source_image=waveform.source_image,
        ),
    )

    result = export_waveforms(
        WavExportRequest(
            output_dir=tmp_path,
            waveforms=(waveform,),
            placements=placements,
            relationships=relationships,
        )
    )

    assert [
        path.relative_to(tmp_path).as_posix()
        for path in result.written_files
        if path.suffix == ".wav"
    ] == ["partition_00_hd1/Sy42 Ana4Bass/SMPL/JP6 FatBs1b036 (2).wav"]
    graph = _graph(tmp_path / "partition_00_hd1" / "Sy42 Ana4Bass" / "volume.axklib.json")
    smpl = graph["objects"]["smpl"][0]
    assert smpl["display_name"] == "JP6 FatBs1b036 *"
    assert smpl["wav_path"] == "SMPL/JP6 FatBs1b036 (2).wav"
    aliases = smpl["user_facing_aliases"]
    assert len(aliases) == 1
    assert aliases[0]["id"]
    assert aliases[0]["source_ref"]
    assert {
        key: aliases[0][key]
        for key in (
            "object_type",
            "object_key",
            "display_name",
            "sample_bank_name",
            "sample_bank_object_key",
            "relationship_quality",
        )
    } == {
        "object_type": "SBNK",
        "object_key": "p0:sbnk1",
        "display_name": "J FatBs1b036",
        "sample_bank_name": "",
        "sample_bank_object_key": "",
        "relationship_quality": "Known",
    }


def test_paired_sbnk_duplicate_marker_uses_owner_label_for_rendered_stem(
    tmp_path: Path,
) -> None:
    left = decode_waveform(_sample_object(b"\x00\x01\x00\x02"))
    right = replace(
        left,
        object_key="p0:sfs2",
        object_offset=512,
        sample_name="Harpsichrd 031-R",
        pcm=b"\x03\x00\x04\x00",
    )
    left = replace(left, sample_name="Harpsichrd 031-L")
    placements = {
        key: WaveformPlacement(
            partition_index=0,
            partition_name="hd1",
            volume_name="Ky22 Harpsi2",
            category_name="Sample Banks",
            display_name=display,
            quality=DataQuality.KNOWN,
            source="test placement",
        )
        for key, display in {
            "p0:sbac1": "Harpsi 2.1N",
            "p0:sbnk-l": "Harpsich031 -L*",
            "p0:sbnk-r": "Harpsich031 -R*",
        }.items()
    }
    relationships = (
        WaveformRelationship(
            "p0:sbnk-l",
            left.object_key,
            "SBNK_LEFT_MEMBER_TO_SMPL",
            "Known",
            "sbnk-member-link+name",
            source_image=left.source_image,
        ),
        WaveformRelationship(
            "p0:sbnk-r",
            right.object_key,
            "SBNK_LEFT_MEMBER_TO_SMPL",
            "Known",
            "sbnk-member-link+name",
            source_image=right.source_image,
        ),
        WaveformRelationship(
            "p0:sbac1",
            "p0:sbnk-l",
            "SBAC_SLOT_TO_SBNK",
            "Known",
            "sample-bank-slot",
            source_image=left.source_image,
        ),
        WaveformRelationship(
            "p0:sbac1",
            "p0:sbnk-r",
            "SBAC_SLOT_TO_SBNK",
            "Known",
            "sample-bank-slot",
            source_image=right.source_image,
        ),
    )

    result = export_waveforms(
        WavExportRequest(
            output_dir=tmp_path,
            waveforms=(left, right),
            placements=placements,
            relationships=relationships,
        )
    )

    rendered_wavs = sorted(
        path.relative_to(tmp_path).as_posix()
        for path in result.written_files
        if path.suffix == ".wav" and path.parent.name == "RENDERED"
    )
    assert rendered_wavs == ["partition_00_hd1/Ky22 Harpsi2/RENDERED/Harpsi 2.1N - Harpsich031.wav"]
    assert not list((tmp_path / "partition_00_hd1" / "Ky22 Harpsi2" / "RENDERED").glob("*(2).wav"))
    graph = _graph(tmp_path / "partition_00_hd1" / "Ky22 Harpsi2" / "volume.axklib.json")
    assert graph["stereo_decisions"][0]["sample_name"] == "Harpsi 2.1N - Harpsich031"
    assert graph["rendered_audio"][0]["wav_path"] == "RENDERED/Harpsi 2.1N - Harpsich031.wav"


def test_object_graph_records_program_assignment_active_state(tmp_path: Path) -> None:
    waveform = decode_waveform(_sample_object())
    placements = {
        "p0:sfs10": WaveformPlacement(
            partition_index=0,
            partition_name="hd1",
            volume_name="Vol 1",
            category_name="Samples",
            display_name="S01 Bank",
            quality=DataQuality.KNOWN,
            source="test placement",
        ),
    }
    relationships = (
        WaveformRelationship(
            "p0:sfs10",
            waveform.object_key,
            "SBNK_LEFT_MEMBER_TO_SMPL",
            "Known",
            "test member",
            source_image=waveform.source_image,
        ),
        WaveformRelationship(
            "p0:prog1",
            "p0:sfs10",
            "PROG_ASSIGNMENT_TO_SBNK",
            "Known",
            "test assignment",
            source_image=waveform.source_image,
            assignment_index=3,
            assignment_name="S01 Bank",
            assignment_row_state="decoded-row",
            active_assignment_state="confirmed-active",
            assignment_rch_assign_display="01",
        ),
    )

    export_waveforms(
        WavExportRequest(
            output_dir=tmp_path,
            waveforms=(waveform,),
            placements=placements,
            relationships=relationships,
        )
    )

    graph = _graph(tmp_path / "partition_00_hd1" / "Vol 1" / "volume.axklib.json")
    assignment = graph["objects"]["prog"][0]["assignments"][0]
    assert assignment["assignment_index"] == 3
    assert assignment["assignment_name"] == "S01 Bank"
    assert assignment["assignment_row_state"] == "decoded-row"
    assert assignment["active_assignment_state"] == "confirmed-active"
    assert assignment["assignment_rch_assign_display"] == "01"
    relationship = next(
        row
        for row in graph["relationships"]
        if row["relationship_type"] == "PROG_ASSIGNMENT_TO_SBNK"
    )
    assert relationship["active_assignment_state"] == "confirmed-active"
    assert relationship["assignment_rch_assign_display"] == "01"


def test_object_graph_scopes_multi_source_exports(tmp_path: Path) -> None:
    from axklib.audio.structured import scoped_object_key

    base = decode_waveform(_sample_object(b"\x00\x01\x00\x02"))
    left_a = replace(base, source_image="a.hds", sample_name="A L")
    right_a = replace(
        base, source_image="a.hds", object_key="p0:sfs2", sample_name="A R", pcm=b"\x03\x00\x04\x00"
    )
    left_b = replace(base, source_image="b.hds", sample_name="B L", pcm=b"\x05\x00\x06\x00")
    right_b = replace(
        base, source_image="b.hds", object_key="p0:sfs2", sample_name="B R", pcm=b"\x07\x00\x08\x00"
    )
    placements = {
        scoped_object_key("a.hds", "p0:sfs10"): WaveformPlacement(
            partition_index=0,
            partition_name="hdA",
            volume_name="Vol A",
            category_name="Samples",
            display_name="Stereo A",
            quality=DataQuality.KNOWN,
            source="test placement A",
        ),
        scoped_object_key("b.hds", "p0:sfs10"): WaveformPlacement(
            partition_index=0,
            partition_name="hdB",
            volume_name="Vol B",
            category_name="Samples",
            display_name="Stereo B",
            quality=DataQuality.KNOWN,
            source="test placement B",
        ),
    }
    relationships = (
        WaveformRelationship(
            "p0:sfs10",
            "p0:sfs1",
            "SBNK_LEFT_MEMBER_TO_SMPL",
            "Known",
            "test A left",
            source_image="a.hds",
            scope_key="a:p0",
        ),
        WaveformRelationship(
            "p0:sfs10",
            "p0:sfs2",
            "SBNK_RIGHT_MEMBER_TO_SMPL",
            "Known",
            "test A right",
            source_image="a.hds",
            scope_key="a:p0",
        ),
        WaveformRelationship(
            "p0:sfs10",
            "p0:sfs1",
            "SBNK_LEFT_MEMBER_TO_SMPL",
            "Known",
            "test B left",
            source_image="b.hds",
            scope_key="b:p0",
        ),
        WaveformRelationship(
            "p0:sfs10",
            "p0:sfs2",
            "SBNK_RIGHT_MEMBER_TO_SMPL",
            "Known",
            "test B right",
            source_image="b.hds",
            scope_key="b:p0",
        ),
    )

    export_waveforms(
        WavExportRequest(
            output_dir=tmp_path,
            waveforms=(left_a, right_a, left_b, right_b),
            stereo_policy="auto",
            placements=placements,
            relationships=relationships,
        )
    )

    graph_paths = sorted(
        path.relative_to(tmp_path).as_posix() for path in tmp_path.rglob("volume.axklib.json")
    )
    assert graph_paths == [
        "src0001_a/partition_00_hdA/Vol A/volume.axklib.json",
        "src0002_b/partition_00_hdB/Vol B/volume.axklib.json",
    ]
    graphs = _volume_graphs(tmp_path)
    assert [graph["volume"]["name"] for graph in graphs] == ["Vol A", "Vol B"]
    assert [graph["objects"]["sbnk"][0]["rendered_audio"]["wav_path"] for graph in graphs] == [
        "RENDERED/Stereo A.wav",
        "RENDERED/Stereo B.wav",
    ]


def test_object_graph_marks_missing_placement_unknown(tmp_path: Path) -> None:
    waveform = decode_waveform(_sample_object())

    export_waveforms(WavExportRequest(output_dir=tmp_path, waveforms=(waveform,)))

    graph = _graph(tmp_path / "_unplaced" / "volume.axklib.json")
    assert graph["volume"]["name"] == "_unplaced"
    assert graph["volume"]["placement_quality"] == "Unknown"
    assert graph["unresolved"][0]["reason"] == "no known volume/category placement"


def test_object_graph_uses_container_scope_placement_without_fake_partition(tmp_path: Path) -> None:
    waveform = replace(
        decode_waveform(_sample_object()),
        source_image="fixture.iso",
        container_kind="iso",
        partition_index=None,
        object_key="fixture.iso:iso9660:F001:SMPL:F001",
        sample_name="ISO Wave",
    )
    placements = {
        waveform.object_key: WaveformPlacement(
            partition_index=None,
            partition_name="",
            volume_name="ISO objects",
            category_name="Waveforms",
            display_name="ISO Wave",
            quality=DataQuality.KNOWN,
            source="iso container object metadata",
            relationship_path="Waveforms-category-entry",
        )
    }

    export_waveforms(
        WavExportRequest(output_dir=tmp_path, waveforms=(waveform,), placements=placements)
    )

    assert (tmp_path / "ISO objects" / "volume.axklib.json").exists()
    assert not list(tmp_path.glob("partition_unknown*"))
    graph = _graph(tmp_path / "ISO objects" / "volume.axklib.json")
    assert graph["volume"]["name"] == "ISO objects"
    assert graph["objects"]["smpl"][0]["wav_path"] == "SMPL/ISO Wave.wav"


def test_structured_export_disambiguates_duplicate_iso_volume_labels(tmp_path: Path) -> None:
    first = replace(
        decode_waveform(_sample_object(b"\x00\x01\x00\x02")),
        source_image="fixture.iso",
        container_kind="iso",
        partition_index=None,
        object_key="fixture.iso:iso9660:G001/F001/SMPL/F001",
        sample_name="Same",
    )
    second = replace(
        decode_waveform(_sample_object(b"\x00\x03\x00\x04")),
        source_image="fixture.iso",
        container_kind="iso",
        partition_index=None,
        object_key="fixture.iso:iso9660:G001/F002/SMPL/F001",
        sample_name="Same",
    )
    placements = {
        first.object_key: WaveformPlacement(
            partition_index=None,
            partition_name="ORGANS",
            volume_name="Or11 Argent",
            category_name="Waveforms",
            display_name="Same",
            quality=DataQuality.KNOWN,
            source="ISO Yamaha CD-ROM menu label metadata",
            raw_volume_path="G001/F001",
        ),
        second.object_key: WaveformPlacement(
            partition_index=None,
            partition_name="ORGANS",
            volume_name="Or11 Argent",
            category_name="Waveforms",
            display_name="Same",
            quality=DataQuality.KNOWN,
            source="ISO Yamaha CD-ROM menu label metadata",
            raw_volume_path="G001/F002",
        ),
    }

    result = export_waveforms(
        WavExportRequest(output_dir=tmp_path, waveforms=(first, second), placements=placements)
    )

    assert sorted(
        path.relative_to(tmp_path).as_posix()
        for path in result.written_files
        if path.suffix == ".wav"
    ) == [
        "ORGANS/Or11 Argent (F001)/SMPL/Same.wav",
        "ORGANS/Or11 Argent (F002)/SMPL/Same.wav",
    ]
    assert (tmp_path / "ORGANS" / "Or11 Argent (F001)" / "volume.axklib.json").exists()
    assert (tmp_path / "ORGANS" / "Or11 Argent (F002)" / "volume.axklib.json").exists()


def test_object_graph_keeps_duplicate_sbnk_references_without_duplicate_wav(tmp_path: Path) -> None:
    waveform = decode_waveform(_sample_object(b"\x00\x01\x00\x02"))
    placements = {
        "p0:sfs10": WaveformPlacement(
            volume_name="Vol 1",
            category_name="Sample Banks",
            display_name="Smp",
            quality=DataQuality.KNOWN,
            source="test sample placement",
        ),
        "p0:sfs11": WaveformPlacement(
            volume_name="Vol 1",
            category_name="Sample Banks",
            display_name="Smp *",
            quality=DataQuality.KNOWN,
            source="test duplicate sample placement",
        ),
        "p0:sbac1": WaveformPlacement(
            volume_name="Vol 1",
            category_name="Sample Banks",
            display_name="Bank",
            quality=DataQuality.KNOWN,
            source="test bank placement",
        ),
        "p0:sbac2": WaveformPlacement(
            volume_name="Vol 1",
            category_name="Sample Banks",
            display_name="Bank *",
            quality=DataQuality.KNOWN,
            source="test duplicate bank placement",
        ),
    }
    relationships = (
        WaveformRelationship(
            "p0:sfs10",
            waveform.object_key,
            "SBNK_LEFT_MEMBER_TO_SMPL",
            "Known",
            "test",
            source_image=waveform.source_image,
        ),
        WaveformRelationship(
            "p0:sfs11",
            waveform.object_key,
            "SBNK_LEFT_MEMBER_TO_SMPL",
            "Known",
            "test",
            source_image=waveform.source_image,
        ),
        WaveformRelationship(
            "p0:sbac1",
            "p0:sfs10",
            "SBAC_SLOT_TO_SBNK",
            "Known",
            "test",
            source_image=waveform.source_image,
        ),
        WaveformRelationship(
            "p0:sbac2",
            "p0:sfs11",
            "SBAC_SLOT_TO_SBNK",
            "Known",
            "test",
            source_image=waveform.source_image,
        ),
    )

    result = export_waveforms(
        WavExportRequest(
            output_dir=tmp_path,
            waveforms=(waveform,),
            placements=placements,
            relationships=relationships,
        )
    )

    assert [
        path.relative_to(tmp_path).as_posix()
        for path in result.written_files
        if path.suffix == ".wav"
    ] == ["Vol 1/SMPL/S01.wav"]
    graph = _graph(tmp_path / "Vol 1" / "volume.axklib.json")
    assert len(graph["objects"]["smpl"]) == 1
    assert sorted(row["display_name"] for row in graph["objects"]["sbnk"]) == ["Smp", "Smp *"]
    assert sorted(row["display_name"] for row in graph["objects"]["sbac"]) == ["Bank", "Bank *"]


def test_object_graph_records_padding_for_rendered_stereo(tmp_path: Path) -> None:
    left = decode_waveform(_sample_object(b"\x00\x01\x00\x02"))
    right = replace(
        left,
        object_key="p0:sfs2",
        object_offset=512,
        sample_name="S01 R",
        pcm=b"\x03\x00\x04\x00\x05\x00",
        frame_count=3,
    )
    placements = {
        "p0:sfs10": WaveformPlacement(
            partition_index=0,
            partition_name="hd1",
            volume_name="Vol 1",
            category_name="Samples",
            display_name="S01 Stereo",
            quality=DataQuality.KNOWN,
            source="test placement",
        ),
    }
    relationships = (
        WaveformRelationship(
            "p0:sfs10",
            left.object_key,
            "SBNK_LEFT_MEMBER_TO_SMPL",
            "Known",
            "test",
            source_image=left.source_image,
        ),
        WaveformRelationship(
            "p0:sfs10",
            right.object_key,
            "SBNK_RIGHT_MEMBER_TO_SMPL",
            "Known",
            "test",
            source_image=right.source_image,
        ),
    )

    result = export_waveforms(
        WavExportRequest(
            output_dir=tmp_path,
            waveforms=(left, right),
            placements=placements,
            relationships=relationships,
        )
    )

    rendered = next(path for path in result.written_files if path.parent.name == "RENDERED")
    with wave.open(str(rendered), "rb") as wav:
        assert wav.getnchannels() == 2
        assert wav.getnframes() == 3
        assert wav.readframes(3) == b"\x01\x00\x03\x00\x02\x00\x04\x00\x00\x00\x05\x00"
    graph = _graph(tmp_path / "partition_00_hd1" / "Vol 1" / "volume.axklib.json")
    assert graph["rendered_audio"][0]["padding"] == {"left_frames": 1, "right_frames": 0}
    assert graph["stereo_decisions"][0]["reason_code"] == "STEREO_PADDED_SHORTER"


def test_filtered_export_suppresses_stereo_warning_when_neither_side_selected(
    tmp_path: Path,
) -> None:
    left = decode_waveform(_sample_object(b"\x00\x01\x00\x02"))
    right = replace(
        left, object_key="p0:sfs2", object_offset=512, sample_name="S01 R", pcm=b"\x03\x00\x04\x00"
    )
    unrelated = replace(left, object_key="p0:sfs99", object_offset=2048, sample_name="Other")
    relationships = (
        WaveformRelationship(
            "p0:sfs10",
            left.object_key,
            "SBNK_LEFT_MEMBER_TO_SMPL",
            "Known",
            "test",
            source_image=left.source_image,
        ),
        WaveformRelationship(
            "p0:sfs10",
            right.object_key,
            "SBNK_RIGHT_MEMBER_TO_SMPL",
            "Known",
            "test",
            source_image=right.source_image,
        ),
    )

    result = export_waveforms(
        WavExportRequest(
            output_dir=tmp_path, waveforms=(unrelated,), placements={}, relationships=relationships
        )
    )

    assert result.warnings == ()
    graph = _graph(tmp_path / "_unplaced" / "volume.axklib.json")
    assert graph["stereo_decisions"] == []


def test_filtered_export_warns_when_only_one_stereo_side_is_selected(tmp_path: Path) -> None:
    left = decode_waveform(_sample_object(b"\x00\x01\x00\x02"))
    right = replace(
        left, object_key="p0:sfs2", object_offset=512, sample_name="S01 R", pcm=b"\x03\x00\x04\x00"
    )
    relationships = (
        WaveformRelationship(
            "p0:sfs10",
            left.object_key,
            "SBNK_LEFT_MEMBER_TO_SMPL",
            "Known",
            "test",
            source_image=left.source_image,
        ),
        WaveformRelationship(
            "p0:sfs10",
            right.object_key,
            "SBNK_RIGHT_MEMBER_TO_SMPL",
            "Known",
            "test",
            source_image=right.source_image,
        ),
    )

    result = export_waveforms(
        WavExportRequest(
            output_dir=tmp_path, waveforms=(left,), placements={}, relationships=relationships
        )
    )

    assert len(result.warnings) == 1
    assert "Stereo companion not selected/exported" in result.warnings[0]
    graph = _graph(tmp_path / "_unplaced" / "volume.axklib.json")
    assert graph["stereo_decisions"][0]["reason_code"] == "STEREO_MISSING_LEFT_OR_RIGHT"
    assert graph["stereo_decisions"][0]["reason"] == "Stereo companion not selected/exported"
    assert graph["stereo_decisions"][0]["left_waveform_object_key"] == left.object_key
    assert graph["stereo_decisions"][0]["right_waveform_object_key"] == ""


def test_object_graph_keeps_non_interleavable_stereo_as_physical_waveforms(tmp_path: Path) -> None:
    left = decode_waveform(_sample_object(b"\x00\x01\x00\x02"))
    right = replace(
        left,
        object_key="p0:sfs2",
        object_offset=512,
        sample_name="S01 R",
        sample_rate=48000,
        pcm=b"\x03\x00\x04\x00",
    )
    placements = {
        "p0:sfs10": WaveformPlacement(
            partition_index=0,
            partition_name="hd1",
            volume_name="Vol 1",
            category_name="Samples",
            display_name="S01 Stereo",
            quality=DataQuality.KNOWN,
            source="test placement",
        ),
    }
    relationships = (
        WaveformRelationship(
            "p0:sfs10",
            left.object_key,
            "SBNK_LEFT_MEMBER_TO_SMPL",
            "Known",
            "test",
            source_image=left.source_image,
        ),
        WaveformRelationship(
            "p0:sfs10",
            right.object_key,
            "SBNK_RIGHT_MEMBER_TO_SMPL",
            "Known",
            "test",
            source_image=right.source_image,
        ),
    )

    result = export_waveforms(
        WavExportRequest(
            output_dir=tmp_path,
            waveforms=(left, right),
            placements=placements,
            relationships=relationships,
        )
    )

    assert result.warnings
    assert "sample rates differ" in result.warnings[0]
    assert not (tmp_path / "partition_00_hd1" / "Vol 1" / "RENDERED").exists()
    graph = _graph(tmp_path / "partition_00_hd1" / "Vol 1" / "volume.axklib.json")
    sbnk = graph["objects"]["sbnk"][0]
    assert sbnk["rendered_audio"] is None
    assert [row["role"] for row in sbnk["physical_waveforms"]] == ["left", "right"]
    assert graph["stereo_decisions"][0]["reason_code"] == "STEREO_SAMPLE_RATE_MISMATCH"
