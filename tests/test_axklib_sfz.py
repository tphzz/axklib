from __future__ import annotations

import json
from pathlib import Path

import pytest

from axklib.sfz import SfzExportRequest, export_sfz


def _graph() -> dict[str, object]:
    return {
        "schema": "axklib.volume_graph.v1",
        "volume": {"path": "partition_00_hd1/Vol 1", "name": "Vol 1"},
        "objects": {
            "smpl": [
                {
                    "id": "SMPL::left",
                    "display_name": "Left",
                    "wav_path": "SMPL/Left.wav",
                    "playback": {
                        "root_key_midi": 60,
                        "fine_tune_cents": 3,
                        "loop_mode_label": "Forward",
                        "loop_start_frame": 10,
                        "loop_length_frames": 80,
                        "loop_end_frame_a4000_ui": 90,
                    },
                },
                {
                    "id": "SMPL::right",
                    "display_name": "Right",
                    "wav_path": "SMPL/Right.wav",
                    "playback": {"root_key_midi": 61, "fine_tune_cents": -4},
                },
                {
                    "id": "SMPL::mono",
                    "display_name": "Mono",
                    "wav_path": "SMPL/Mono.wav",
                    "playback": {
                        "root_key_midi": 64,
                        "fine_tune_cents": 0,
                        "loop_mode_label": "One Shot",
                    },
                },
            ],
            "sbnk": [
                {
                    "id": "SBNK::stereo",
                    "display_name": "Stereo Member",
                    "physical_waveforms": [
                        {
                            "role": "left",
                            "smpl_id": "SMPL::left",
                            "wav_path": "SMPL/Left.wav",
                        },
                        {
                            "role": "right",
                            "smpl_id": "SMPL::right",
                            "wav_path": "SMPL/Right.wav",
                        },
                    ],
                    "rendered_audio": {"wav_path": "RENDERED/Stereo Member.wav"},
                    "parameters": {
                        "decoded_current_sbnk_member_parameters": {
                            "key_range_low_0x0e3": 48,
                            "key_range_high_0x0e2": 72,
                            "left_root_key_0x0d6": 60,
                            "right_root_key_0x0d7": 61,
                            "left_fine_tune_cents_0x0dc": 3,
                            "right_fine_tune_cents_0x0dd": -4,
                            "coarse_tune_0x0d5": 1,
                        }
                    },
                },
                {
                    "id": "SBNK::mono",
                    "display_name": "Standalone Mono",
                    "physical_waveforms": [
                        {
                            "role": "left",
                            "smpl_id": "SMPL::mono",
                            "wav_path": "SMPL/Mono.wav",
                        }
                    ],
                    "rendered_audio": None,
                    "parameters": {"decoded_current_sbnk_member_parameters": {}},
                },
            ],
            "sbac": [
                {
                    "id": "SBAC::bank",
                    "display_name": "Bank",
                    "members": [{"sbnk_id": "SBNK::stereo"}],
                }
            ],
        },
    }


def test_export_sfz_prefers_rendered_audio_and_relative_paths(tmp_path: Path) -> None:
    volume_root = tmp_path / "partition_00_hd1" / "Vol 1"
    (volume_root / "SMPL").mkdir(parents=True)
    (volume_root / "RENDERED").mkdir()
    for rel in ("SMPL/Left.wav", "SMPL/Right.wav", "SMPL/Mono.wav", "RENDERED/Stereo Member.wav"):
        (volume_root / rel).write_bytes(b"wav")

    result = export_sfz(SfzExportRequest(output_dir=tmp_path, volume_graphs=[_graph()]))

    sfz_files = sorted(path.relative_to(volume_root).as_posix() for path in result.written_files)
    assert "SFZ/B Bank.sfz" in sfz_files
    assert "SFZ/Standalone Mono.sfz" in sfz_files
    bank_text = (volume_root / "SFZ" / "B Bank.sfz").read_text(encoding="utf-8")
    assert "sample=../RENDERED/Stereo Member.wav" in bank_text
    assert "sample=../SMPL/Left.wav" not in bank_text
    region_line = next(line for line in bank_text.splitlines() if line.startswith("<region>"))
    assert region_line.endswith("sample=../RENDERED/Stereo Member.wav")
    assert "lokey=48" in region_line
    assert "hikey=72" in region_line
    assert "pitch_keycenter=60" in region_line
    assert "transpose=1" in region_line
    assert "tune=3" in region_line
    assert "loop_mode=loop_continuous" in region_line
    assert "loop_start=10" in region_line
    assert "loop_end=89" in region_line
    mono_text = (volume_root / "SFZ" / "Standalone Mono.sfz").read_text(encoding="utf-8")
    mono_region = next(line for line in mono_text.splitlines() if line.startswith("<region>"))
    assert "sample=../SMPL/Mono.wav" in mono_region
    assert "pan=" not in mono_region
    manifest = json.loads((volume_root / "sfz_exports.json").read_text(encoding="utf-8"))
    assert [row["instrument_name"] for row in manifest] == ["B Bank", "Standalone Mono"]


def test_export_sfz_uses_physical_fallback_with_pan(tmp_path: Path) -> None:
    graph = _graph()
    sbnk = graph["objects"]["sbnk"][0]  # type: ignore[index]
    assert isinstance(sbnk, dict)
    sbnk["rendered_audio"] = None

    result = export_sfz(SfzExportRequest(output_dir=tmp_path, volume_graphs=[graph]))

    bank_text = (tmp_path / "partition_00_hd1" / "Vol 1" / "SFZ" / "B Bank.sfz").read_text(
        encoding="utf-8"
    )
    assert "sample=../SMPL/Left.wav" in bank_text
    assert "sample=../SMPL/Right.wav" in bank_text
    assert "pan=-100" in bank_text
    assert "pan=100" in bank_text
    assert sum(1 for path in result.written_files if path.suffix == ".sfz") == 2




def test_export_sfz_resolves_sampler_orig_key_limit_from_graph(tmp_path: Path) -> None:
    graph = _graph()
    sbnk = graph["objects"]["sbnk"][0]  # type: ignore[index]
    assert isinstance(sbnk, dict)
    params = sbnk["parameters"]["decoded_current_sbnk_member_parameters"]  # type: ignore[index]
    assert isinstance(params, dict)
    params["key_range_low_0x0e3"] = 0
    params["key_range_high_0x0e2"] = 128
    params["left_root_key_0x0d6"] = 60
    sbnk["parameters"]["resolved_key_range"] = {  # type: ignore[index]
        "low_midi": 0,
        "high_midi": 60,
        "low_display": 0,
        "high_display": "Orig",
        "low_raw": 0,
        "high_raw": 128,
        "basis": "sampler-orig-key-limit",
    }
    volume_root = tmp_path / "partition_00_hd1" / "Vol 1"
    (volume_root / "RENDERED").mkdir(parents=True)
    (volume_root / "RENDERED" / "Stereo Member.wav").write_bytes(b"wav")

    export_sfz(SfzExportRequest(output_dir=tmp_path, volume_graphs=[graph]))

    text = (volume_root / "SFZ" / "B Bank.sfz").read_text(encoding="utf-8")
    region_line = next(line for line in text.splitlines() if line.startswith("<region>"))
    assert "lokey=0" in region_line
    assert "hikey=60" in region_line
    assert "pitch_keycenter=60" in region_line


def test_export_sfz_falls_back_to_raw_orig_key_limit_for_older_graphs(tmp_path: Path) -> None:
    graph = _graph()
    sbnk = graph["objects"]["sbnk"][0]  # type: ignore[index]
    assert isinstance(sbnk, dict)
    params = sbnk["parameters"]["decoded_current_sbnk_member_parameters"]  # type: ignore[index]
    assert isinstance(params, dict)
    params["key_range_low_0x0e3"] = 255
    params["key_range_high_0x0e2"] = 128
    params["left_root_key_0x0d6"] = 38
    volume_root = tmp_path / "partition_00_hd1" / "Vol 1"
    (volume_root / "RENDERED").mkdir(parents=True)
    (volume_root / "RENDERED" / "Stereo Member.wav").write_bytes(b"wav")

    export_sfz(SfzExportRequest(output_dir=tmp_path, volume_graphs=[graph]))

    text = (volume_root / "SFZ" / "B Bank.sfz").read_text(encoding="utf-8")
    region_line = next(line for line in text.splitlines() if line.startswith("<region>"))
    assert "lokey=38" in region_line
    assert "hikey=38" in region_line


def test_export_sfz_warns_when_orig_key_limit_has_no_root(tmp_path: Path) -> None:
    graph = _graph()
    smpl = graph["objects"]["smpl"][0]  # type: ignore[index]
    sbnk = graph["objects"]["sbnk"][0]  # type: ignore[index]
    assert isinstance(smpl, dict)
    assert isinstance(sbnk, dict)
    playback = smpl["playback"]
    assert isinstance(playback, dict)
    playback.pop("root_key_midi", None)
    params = sbnk["parameters"]["decoded_current_sbnk_member_parameters"]  # type: ignore[index]
    assert isinstance(params, dict)
    params.pop("left_root_key_0x0d6", None)
    params.pop("right_root_key_0x0d7", None)
    params["key_range_low_0x0e3"] = 255
    params["key_range_high_0x0e2"] = 128
    volume_root = tmp_path / "partition_00_hd1" / "Vol 1"
    (volume_root / "RENDERED").mkdir(parents=True)
    (volume_root / "RENDERED" / "Stereo Member.wav").write_bytes(b"wav")

    result = export_sfz(SfzExportRequest(output_dir=tmp_path, volume_graphs=[graph]))

    text = (volume_root / "SFZ" / "B Bank.sfz").read_text(encoding="utf-8")
    region_line = next(line for line in text.splitlines() if line.startswith("<region>"))
    assert "lokey=" not in region_line
    assert "hikey=" not in region_line
    assert result.warnings == (
        "partition_00_hd1/Vol 1: B Bank: Stereo Member: "
        "sampler Orig key limit could not be resolved without a root key",
    )


def test_export_sfz_fails_existing_target_without_overwrite(tmp_path: Path) -> None:
    export_sfz(SfzExportRequest(output_dir=tmp_path, volume_graphs=[_graph()]))

    with pytest.raises(FileExistsError):
        export_sfz(SfzExportRequest(output_dir=tmp_path, volume_graphs=[_graph()]))

