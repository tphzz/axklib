from __future__ import annotations

import json
import wave
from collections import Counter
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from axklib import cli as axklibtool
from axklib import content_tree as axklib_content_tree
from axklib.audio.importing import import_sampler_audio
from axklib.containers import load_sfs_objects
from axklib.relationships import build_relationship_graph


def _put_be16(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 2] = value.to_bytes(2, "big")


def _put_be32(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 4] = value.to_bytes(4, "big")


def _write_standalone_smpl(path: Path) -> None:
    pcm_be = b"\x00\x01\x00\x02"
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
    payload[0x85] = 4
    _put_be32(payload, 0x92, 2)
    _put_be32(payload, 0x96, 0)
    _put_be32(payload, 0x9A, 2)
    payload[0x200 : 0x200 + len(pcm_be)] = pcm_be
    path.write_bytes(payload)


def _write_mono_wav(path: Path, samples: tuple[int, ...]) -> bytes:
    pcm = b"".join(sample.to_bytes(2, "little", signed=True) for sample in samples)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(44_100)
        wav.writeframes(pcm)
    return pcm


def _read_mono_wav_pcm(path: Path) -> bytes:
    with wave.open(str(path), "rb") as wav:
        assert wav.getnchannels() == 1
        assert wav.getsampwidth() == 2
        assert wav.getframerate() == 44_100
        return wav.readframes(wav.getnframes())


def _generated_exact_export_pcm(logical_pcm: bytes) -> bytes:
    return logical_pcm + logical_pcm[: min(len(logical_pcm), 8)]


def test_help_output_is_available(capsys: pytest.CaptureFixture[str]) -> None:
    with pytest.raises(SystemExit) as exc_info:
        axklibtool.main(["--help"])

    captured = capsys.readouterr()
    assert exc_info.value.code == 0
    assert "Unified Yamaha A-series" in captured.out
    assert "command" in captured.out


def test_info_reports_bad_path_without_internal_error(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    missing = tmp_path / "missing.hds"

    non_strict = axklibtool.main(["info", str(missing)])
    strict = axklibtool.main(["info", "--strict", str(missing)])

    captured = capsys.readouterr()
    assert non_strict == 1
    assert strict == 1
    assert "ERROR\tAXKLIB_CONTAINER_OPEN_FAILED" in captured.out
    assert "internal error" not in captured.err
    assert "Traceback" not in captured.err


def test_info_show_default_programs_forwards_content_tree_option(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    seen: list[bool] = []

    def fake_build_content_trees_for_paths(*args: object, **kwargs: object) -> object:
        seen.append(bool(kwargs.get("include_default_programs")))
        return axklib_content_tree.ContentTreeLoadResult(
            trees=(
                axklib_content_tree.ContentTree(
                    source_path="fixture.hds",
                    container_kind="sfs",
                    detected_format="sfs",
                    roots=(),
                ),
            ),
            load_errors=(),
        )

    monkeypatch.setattr(
        axklibtool, "build_content_trees_for_paths", fake_build_content_trees_for_paths
    )

    default_code = axklibtool.main(["info", "fixture.hds"])
    verbose_code = axklibtool.main(["info", "--show-default-programs", "fixture.hds"])

    capsys.readouterr()
    assert default_code == 0
    assert verbose_code == 0
    assert seen == [False, True]


def test_content_label_map_option_is_not_public() -> None:
    with pytest.raises(SystemExit) as exc_info:
        axklibtool.main(["info", "--content-label-map", "labels.json", "fixture.iso"])

    assert exc_info.value.code == 2


def test_create_hds_builds_manifest_and_reports_layout(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    manifest_path = tmp_path / "manifest.json"
    manifest_path.write_text(
        json.dumps(
            {
                "schema_version": "1.0",
                "size_bytes": 256 * 1024 * 1024,
                "partitions": [
                    {
                        "name": "hd1",
                        "volumes": [
                            {
                                "name": "New Volume",
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
    output = tmp_path / "HD00_512_manifest.hds"

    code = axklibtool.main(["create", "hds", str(manifest_path), "-o", str(output)])

    captured = capsys.readouterr()
    assert code == 0
    assert output.stat().st_size == 256 * 1024 * 1024
    assert "partitions=1 objects=0 unused_tail_sectors=0" in captured.out
    assert "partition=0 name='hd1' start_sector=3 sector_count=524285" in captured.out


def test_create_hds_supports_general_partition_geometry(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    manifest_path = tmp_path / "manifest.json"
    manifest_path.write_text(
        json.dumps(
            {
                "schema_version": "1.0",
                "size_bytes": 6 * 1024 * 1024,
                "partitions": [
                    {
                        "name": f"hd{index + 1}",
                        "volumes": [
                            {
                                "name": f"Volume {index + 1}",
                                "waveforms": [],
                                "sample_banks": [],
                            }
                        ],
                    }
                    for index in range(5)
                ],
            }
        ),
        encoding="utf-8",
    )
    output = tmp_path / "HD00_512_general.hds"

    code = axklibtool.main(["create", "hds", str(manifest_path), "-o", str(output)])

    captured = capsys.readouterr()
    assert code == 0
    assert output.stat().st_size == 6 * 1024 * 1024
    assert "partitions=5 objects=0 unused_tail_sectors=1" in captured.out
    assert "partition=4 name='hd5' start_sector=9831 sector_count=2456" in captured.out


@pytest.mark.parametrize("partition_count", [1, 3, 8])
def test_create_and_extract_hds_round_trips_logical_pcm_across_partition_counts(
    tmp_path: Path,
    partition_count: int,
) -> None:
    expected_pcm: list[bytes] = []
    partitions: list[dict[str, object]] = []
    for index in range(partition_count):
        source_path = tmp_path / f"tone-{index}.wav"
        samples = tuple((index + 1) * value for value in (0, 101, -203, 307, -409))
        expected_pcm.append(_write_mono_wav(source_path, samples))
        root_key = 48 + index
        partitions.append(
            {
                "name": f"hd{index + 1}",
                "volumes": [
                    {
                        "name": f"RoundTrip {index + 1}",
                        "waveforms": [
                            {
                                "id": "tone",
                                "name": f"Wave{index + 1}",
                                "path": source_path.name,
                                "root_key": root_key,
                            }
                        ],
                        "sample_banks": [
                            {
                                "name": f"Bank{index + 1}",
                                "waveform_id": "tone",
                                "root_key": root_key,
                                "key_low": root_key,
                                "key_high": root_key,
                                "level": 100,
                            }
                        ],
                    }
                ],
            }
        )
    manifest_path = tmp_path / "manifest.json"
    manifest_path.write_text(
        json.dumps(
            {
                "schema_version": "1.0",
                "size_bytes": (2 + partition_count * 2046) * 512,
                "partitions": partitions,
            }
        ),
        encoding="utf-8",
    )
    image_path = tmp_path / "HD00_512_roundtrip.hds"
    export_dir = tmp_path / "exports"

    create_code = axklibtool.main(["create", "hds", str(manifest_path), "-o", str(image_path)])
    extract_code = axklibtool.main(
        ["extract", "wav", "file", "-o", str(export_dir), str(image_path)]
    )

    assert create_code == 0
    assert extract_code == 0
    exported_wavs = sorted(export_dir.rglob("*.wav"))
    assert len(exported_wavs) == partition_count
    exported_pcm = [_read_mono_wav_pcm(path) for path in exported_wavs]
    assert sorted(exported_pcm) == sorted(_generated_exact_export_pcm(pcm) for pcm in expected_pcm)
    assert sorted(pcm[: len(pcm) - 8] for pcm in exported_pcm) == sorted(expected_pcm)


def test_create_and_extract_hds_manifest_renders_two_member_stereo_bank(
    tmp_path: Path,
) -> None:
    left_pcm = _write_mono_wav(tmp_path / "tone-left.wav", (0, 1000, -1000, 2000, -2000))
    right_pcm = _write_mono_wav(tmp_path / "tone-right.wav", (300, -300, 900, -900, 0))
    manifest_path = tmp_path / "manifest.json"
    manifest_path.write_text(
        json.dumps(
            {
                "schema_version": "1.0",
                "size_bytes": 4 * 1024 * 1024,
                "partitions": [
                    {
                        "name": "hd1",
                        "volumes": [
                            {
                                "name": "Stereo Vol",
                                "waveforms": [
                                    {
                                        "id": "left",
                                        "name": "Stereo-L",
                                        "path": "tone-left.wav",
                                        "root_key": 60,
                                    },
                                    {
                                        "id": "right",
                                        "name": "Stereo-R",
                                        "path": "tone-right.wav",
                                        "root_key": 60,
                                    },
                                ],
                                "sample_banks": [
                                    {
                                        "name": "Stereo Bank",
                                        "waveform_id": "left",
                                        "right_waveform_id": "right",
                                        "root_key": 60,
                                        "key_low": 48,
                                        "key_high": 72,
                                        "level": 96,
                                    }
                                ],
                            }
                        ],
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    image_path = tmp_path / "HD00_512_stereo.hds"
    export_dir = tmp_path / "exports"

    create_code = axklibtool.main(["create", "hds", str(manifest_path), "-o", str(image_path)])
    extract_code = axklibtool.main(
        ["extract", "wav", "file", "-o", str(export_dir), str(image_path)]
    )

    assert create_code == 0
    assert extract_code == 0
    physical_paths = sorted((export_dir / "_samples" / "physical").glob("*.wav"))
    rendered_paths = sorted((export_dir / "_samples" / "rendered").glob("*.wav"))
    assert len(physical_paths) == 2
    assert len(rendered_paths) == 1
    physical_pcm = sorted(_read_mono_wav_pcm(path) for path in physical_paths)
    stored_left = _generated_exact_export_pcm(left_pcm)
    stored_right = _generated_exact_export_pcm(right_pcm)
    assert physical_pcm == sorted([stored_left, stored_right])
    with wave.open(str(rendered_paths[0]), "rb") as rendered:
        assert rendered.getnchannels() == 2
        assert rendered.getsampwidth() == 2
        assert rendered.getframerate() == 44_100
        rendered_pcm = rendered.readframes(rendered.getnframes())
    expected_interleaved = b"".join(
        stored_left[offset : offset + 2] + stored_right[offset : offset + 2]
        for offset in range(0, len(stored_left), 2)
    )
    assert rendered_pcm == expected_interleaved


def test_create_hds_imports_and_resamples_interleaved_audio_manifest(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    source_path = tmp_path / "stereo-96k.wav"
    time = np.arange(960, dtype=np.float64) / 96_000
    sf.write(
        source_path,
        np.column_stack(
            (
                0.5 * np.sin(2 * np.pi * 1_000 * time),
                0.25 * np.sin(2 * np.pi * 2_000 * time),
            )
        ),
        96_000,
        format="WAV",
        subtype="FLOAT",
    )
    converted = import_sampler_audio(source_path, expected_channels=2)
    manifest_path = tmp_path / "manifest.json"
    manifest_path.write_text(
        json.dumps(
            {
                "schema_version": "1.0",
                "size_bytes": 4 * 1024 * 1024,
                "partitions": [
                    {
                        "name": "hd1",
                        "volumes": [
                            {
                                "name": "Imported Stereo",
                                "waveforms": [],
                                "sample_banks": [
                                    {
                                        "name": "Resampled Bank",
                                        "interleaved_audio_path": source_path.name,
                                        "left_waveform_name": "Resampled-L",
                                        "right_waveform_name": "Resampled-R",
                                        "root_key": 60,
                                        "key_low": 48,
                                        "key_high": 72,
                                        "level": 96,
                                    }
                                ],
                            }
                        ],
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    image_path = tmp_path / "HD00_512_imported_stereo.hds"
    export_dir = tmp_path / "exports"

    create_code = axklibtool.main(["create", "hds", str(manifest_path), "-o", str(image_path)])
    create_output = capsys.readouterr()
    extract_code = axklibtool.main(
        ["extract", "wav", "file", "-o", str(export_dir), str(image_path)]
    )

    assert create_code == 0
    assert extract_code == 0
    assert "actions=split-stereo,resampled,quantized-16bit" in create_output.out
    assert "rate=96000->44100" in create_output.out
    assert "waveforms='Resampled-L,Resampled-R'" in create_output.out
    physical_paths = sorted((export_dir / "_samples" / "physical").glob("*.wav"))
    assert len(physical_paths) == 2
    expected_channels = sorted(
        channel + channel[: min(len(channel), 8)] for channel in converted.pcm_channels
    )
    assert sorted(_read_mono_wav_pcm(path) for path in physical_paths) == expected_channels


def test_create_hds_manifest_writes_hardware_profile_sbac_and_prog(tmp_path: Path) -> None:
    _write_mono_wav(tmp_path / "tone.wav", (0, 1000, -1000, 2000, -2000))
    manifest_path = tmp_path / "manifest.json"
    manifest_path.write_text(
        json.dumps(
            {
                "schema_version": "1.0",
                "size_bytes": 1024 * 1024,
                "partitions": [
                    {
                        "name": "New Partition",
                        "volumes": [
                            {
                                "name": "New Volume",
                                "waveforms": [
                                    {
                                        "id": "pulse",
                                        "name": "pulse 1",
                                        "path": "tone.wav",
                                        "root_key": 66,
                                    }
                                ],
                                "sample_banks": [
                                    {
                                        "name": "JS01",
                                        "waveform_id": "pulse",
                                        "root_key": 66,
                                        "key_low": 0,
                                        "key_high": 127,
                                        "level": 100,
                                    },
                                    {
                                        "name": "JS02 *",
                                        "waveform_id": "pulse",
                                        "root_key": 66,
                                        "key_low": 0,
                                        "key_high": 127,
                                        "level": 100,
                                    },
                                ],
                                "sample_bank_groups": [
                                    {"name": "AUDSB", "member_sample_bank": "JS01"}
                                ],
                                "programs": [
                                    {
                                        "number": 1,
                                        "assignments": [
                                            {
                                                "sample_bank_group": "AUDSB",
                                                "receive_channel": 1,
                                            },
                                            {
                                                "sample_bank": "JS02 *",
                                                "receive_channel": 2,
                                            },
                                        ],
                                    }
                                ],
                            }
                        ],
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    image_path = tmp_path / "HD00_512_sbac_prog.hds"

    code = axklibtool.main(["create", "hds", str(manifest_path), "-o", str(image_path)])

    assert code == 0
    objects = load_sfs_objects(image_path)
    assert Counter(str(item.object_type) for item in objects) == Counter(
        {"SMPL": 1, "SBNK": 2, "SBAC": 1, "PROG": 1}
    )
    graph = build_relationship_graph(objects)
    assert Counter(row.relationship_type for row in graph.relationships) == Counter(
        {
            "SBNK_LEFT_MEMBER_TO_SMPL": 2,
            "SBAC_SLOT_TO_SBNK": 1,
            "PROG_ASSIGNMENT_TO_SBAC": 1,
            "PROG_ASSIGNMENT_TO_SBNK": 1,
            "SBNK_PROGRAM_BITMAP_TO_PROG": 1,
        }
    )
    assert all(row.quality == "Known" for row in graph.relationships)


def test_create_and_extract_hds_preserves_uneven_multi_volume_object_topology(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    object_counts_by_partition = ((0,), (1, 3), (2,), (0, 1, 2), (4,))
    expected_pcm: list[bytes] = []
    expected_placements: Counter[tuple[int, str, str]] = Counter()
    partitions: list[dict[str, object]] = []
    for partition_index, volume_counts in enumerate(object_counts_by_partition):
        volumes: list[dict[str, object]] = []
        for volume_index, object_count in enumerate(volume_counts):
            volume_name = f"P{partition_index + 1}V{volume_index + 1}"
            waveforms: list[dict[str, object]] = []
            sample_banks: list[dict[str, object]] = []
            for object_index in range(object_count):
                stem = f"p{partition_index}v{volume_index}o{object_index}"
                source_path = tmp_path / f"{stem}.wav"
                seed = 1 + partition_index * 20 + volume_index * 5 + object_index
                expected_pcm.append(
                    _write_mono_wav(
                        source_path,
                        (seed, seed * 101, seed * -103, seed * 107, seed * -109),
                    )
                )
                root_key = 36 + seed
                waveforms.append(
                    {
                        "id": stem,
                        "name": f"W{partition_index}{volume_index}{object_index}",
                        "path": source_path.name,
                        "root_key": root_key,
                    }
                )
                sample_banks.append(
                    {
                        "name": f"B{partition_index}{volume_index}{object_index}",
                        "waveform_id": stem,
                        "root_key": root_key,
                        "key_low": root_key,
                        "key_high": root_key,
                        "level": 80 + object_index,
                    }
                )
            expected_placements[(partition_index, volume_name, "waveform")] = object_count
            expected_placements[(partition_index, volume_name, "sample_bank")] = object_count
            volumes.append(
                {
                    "name": volume_name,
                    "waveforms": waveforms,
                    "sample_banks": sample_banks,
                }
            )
        partitions.append({"name": f"hd{partition_index + 1}", "volumes": volumes})
    manifest_path = tmp_path / "manifest.json"
    manifest_path.write_text(
        json.dumps(
            {
                "schema_version": "1.0",
                "size_bytes": 12 * 1024 * 1024,
                "partitions": partitions,
            }
        ),
        encoding="utf-8",
    )
    image_path = tmp_path / "HD00_512_topology.hds"
    export_dir = tmp_path / "exports"

    create_code = axklibtool.main(["create", "hds", str(manifest_path), "-o", str(image_path)])
    extract_code = axklibtool.main(
        ["extract", "wav", "file", "-o", str(export_dir), str(image_path)]
    )

    assert create_code == 0
    assert extract_code == 0
    exported_pcm = [_read_mono_wav_pcm(path) for path in export_dir.rglob("*.wav")]
    assert sorted(exported_pcm) == sorted(_generated_exact_export_pcm(pcm) for pcm in expected_pcm)
    assert sorted(pcm[: len(pcm) - 8] for pcm in exported_pcm) == sorted(expected_pcm)
    capsys.readouterr()
    info_code = axklibtool.main(["info", "--format", "json", str(image_path)])
    info_output = json.loads(capsys.readouterr().out)
    assert info_code == 0
    actual_placements: Counter[tuple[int, str, str]] = Counter()
    for partition_node in info_output["trees"][0]["roots"]:
        partition_index = int(partition_node["node_id"].split(":")[1])
        for volume_node in partition_node["children"]:
            stack = list(volume_node["children"])
            while stack:
                node = stack.pop()
                if node["node_type"] in {"waveform", "sample_bank"}:
                    actual_placements[
                        (partition_index, volume_node["display_name"], node["node_type"])
                    ] += 1
                stack.extend(node["children"])
    assert actual_placements == expected_placements
    objects = load_sfs_objects(image_path)
    objects_by_key = {item.object_key: item for item in objects}
    relationships = [
        row
        for row in build_relationship_graph(objects).relationships
        if row.relationship_type == "SBNK_LEFT_MEMBER_TO_SMPL"
    ]
    assert len(relationships) == len(expected_pcm)
    assert all(row.quality == "Known" for row in relationships)
    assert all(
        objects_by_key[row.source_key].partition_index
        == objects_by_key[row.target_key].partition_index
        and objects_by_key[row.source_key].name[1:] == objects_by_key[row.target_key].name[1:]
        for row in relationships
    )


def test_create_hds_uses_standard_overwrite_policy(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    manifest_path = tmp_path / "manifest.json"
    manifest_path.write_text(
        json.dumps(
            {
                "schema_version": "1.0",
                "size_bytes": 256 * 1024 * 1024,
                "partitions": [
                    {
                        "name": "hd1",
                        "volumes": [
                            {
                                "name": "New Volume",
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
    output = tmp_path / "existing.hds"
    output.write_bytes(b"existing")

    refused = axklibtool.main(["create", "hds", str(manifest_path), "-o", str(output)])
    replaced = axklibtool.main(
        ["create", "hds", str(manifest_path), "-o", str(output), "--overwrite"]
    )

    captured = capsys.readouterr()
    assert refused == 1
    assert replaced == 0
    assert "output already exists" in captured.err
    assert output.stat().st_size == 256 * 1024 * 1024


def test_create_hds_reports_manifest_errors_without_internal_error(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    manifest_path = tmp_path / "invalid.json"
    manifest_path.write_text('{"schema_version": "2.0"}', encoding="utf-8")

    code = axklibtool.main(["create", "hds", str(manifest_path), "-o", str(tmp_path / "bad.hds")])

    captured = capsys.readouterr()
    assert code == 2
    assert "missing required fields" in captured.err
    assert "internal error" not in captured.err


def test_create_hds_reports_missing_manifest_without_internal_error(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    missing = tmp_path / "missing.json"

    code = axklibtool.main(["create", "hds", str(missing), "-o", str(tmp_path / "bad.hds")])

    captured = capsys.readouterr()
    assert code == 2
    assert "missing.json" in captured.err
    assert "internal error" not in captured.err


def test_alter_hds_cli_dry_run_and_atomic_output(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    build_manifest = tmp_path / "build.json"
    build_manifest.write_text(
        json.dumps(
            {
                "schema_version": "1.0",
                "size_bytes": 8 * 1024 * 1024,
                "partitions": [
                    {
                        "name": "hd1",
                        "volumes": [
                            {"name": "Delete Me", "waveforms": [], "sample_banks": []}
                        ],
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    source = tmp_path / "source.hds"
    assert axklibtool.main(["create", "hds", str(build_manifest), "-o", str(source)]) == 0
    transaction = tmp_path / "transaction.json"
    transaction.write_text(
        json.dumps(
            {
                "schema_version": "1.0",
                "operations": [
                    {
                        "id": "delete",
                        "type": "delete_volume",
                        "partition_index": 0,
                        "volume_name": "Delete Me",
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    source_before = source.read_bytes()

    dry_run = axklibtool.main(["alter", "hds", str(source), str(transaction)])
    output = tmp_path / "altered.hds"
    applied = axklibtool.main(
        ["alter", "hds", str(source), str(transaction), "-o", str(output)]
    )

    captured = capsys.readouterr()
    assert dry_run == 0
    assert applied == 0
    assert source.read_bytes() == source_before
    assert output.exists()
    assert '"applied": false' in captured.out
    assert '"applied": true' in captured.out


def test_broken_pipe_is_not_reported_as_internal_error(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    def raise_broken_pipe(_args: object) -> int:
        raise BrokenPipeError()

    monkeypatch.setattr(axklibtool, "run_info", raise_broken_pipe)

    code = axklibtool.main(["info", "dummy.hds"])

    captured = capsys.readouterr()
    assert code == 0
    assert "internal error" not in captured.err


def test_inventory_refuses_nonempty_output_directory(tmp_path: Path) -> None:
    output = tmp_path / "out"
    output.mkdir()
    (output / "existing.txt").write_text("keep", encoding="utf-8")

    code = axklibtool.main(["inventory", "-o", str(output), "missing.hds"])

    assert code == 1


def test_objects_reports_partial_load_failure_as_exit_code_3(tmp_path: Path) -> None:
    output = tmp_path / "objects"

    code = axklibtool.main(["objects", "-o", str(output), str(tmp_path / "missing.hds")])

    assert code == 3
    assert (output / "objects.csv").exists()


def test_objects_writes_schema_manifest(tmp_path: Path) -> None:
    output = tmp_path / "objects"

    code = axklibtool.main(["objects", "-o", str(output), str(tmp_path / "missing.hds")])

    assert code == 3
    assert (output / "_schemas" / "objects.schema.json").exists()
    assert (output / "_schemas" / "schema_index.json").exists()


def test_relationships_reports_load_failure_without_traceback(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    output = tmp_path / "relationships"

    code = axklibtool.main(["relationships", "-o", str(output), str(tmp_path / "missing.hds")])

    captured = capsys.readouterr()
    assert code == 3
    assert "load_errors=1" in captured.out
    assert "internal error" not in captured.err
    assert (output / "relationships.csv").exists()
    assert (output / "load_errors.json").exists()
    assert (output / "_schemas" / "load_errors.schema.json").exists()


def test_coverage_reports_load_failure_without_traceback(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    output = tmp_path / "coverage"

    code = axklibtool.main(["coverage", "-o", str(output), str(tmp_path / "missing.hds")])

    captured = capsys.readouterr()
    assert code == 3
    assert "load_errors=1" in captured.out
    assert "internal error" not in captured.err
    assert (output / "coverage_summary.json").exists()
    assert (output / "load_errors.csv").exists()


def test_extract_wav_file_fails_on_existing_targets_without_overwrite(tmp_path: Path) -> None:
    source = tmp_path / "sample.smpl"
    output = tmp_path / "wav"
    _write_standalone_smpl(source)

    first = axklibtool.main(["extract", "wav", "file", "-o", str(output), str(source)])
    second = axklibtool.main(["extract", "wav", "file", "-o", str(output), str(source)])

    assert first == 0
    assert second == 1
    assert any(path.suffix == ".wav" for path in output.rglob("*.wav"))


def test_extract_wav_file_allows_existing_targets_with_overwrite(tmp_path: Path) -> None:
    source = tmp_path / "sample.smpl"
    output = tmp_path / "wav"
    _write_standalone_smpl(source)

    first = axklibtool.main(["extract", "wav", "file", "-o", str(output), str(source)])
    second = axklibtool.main(
        ["extract", "wav", "file", "--overwrite", "-o", str(output), str(source)]
    )

    assert first == 0
    assert second == 0


def test_extract_wav_file_progress_always_reports_same_line_status(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    source = tmp_path / "sample.smpl"
    output = tmp_path / "wav"
    _write_standalone_smpl(source)

    code = axklibtool.main(
        ["extract", "wav", "file", "--progress", "always", "-o", str(output), str(source)]
    )

    captured = capsys.readouterr()
    assert code == 0
    assert "\rloading:" in captured.err
    assert "\rresolving:" in captured.err
    assert "\rreading:" in captured.err
    assert "\rexporting:" in captured.err
    assert "_samples/physical/S01__" in captured.err
    assert captured.err.endswith("\n")


def test_extract_wav_file_writes_shared_sample_pool(tmp_path: Path) -> None:
    source = tmp_path / "sample.smpl"
    output = tmp_path / "targeted-wav"
    _write_standalone_smpl(source)

    code = axklibtool.main(["extract", "wav", "file", "-o", str(output), str(source)])

    assert code == 0
    wav_files = sorted(path.relative_to(output).as_posix() for path in output.rglob("*.wav"))
    assert len(wav_files) == 1
    assert wav_files[0].startswith("_samples/physical/S01__")
    assert not (output / "file" / "selection.axklib.json").exists()
    assert not any(path.name.startswith(".axklib_stage_") for path in output.iterdir())


def test_extract_sfz_file_runs_wave_export_without_manifest_lists(tmp_path: Path) -> None:
    source = tmp_path / "sample.smpl"
    output = tmp_path / "sfz"
    _write_standalone_smpl(source)

    code = axklibtool.main(["extract", "sfz", "file", "-o", str(output), str(source)])

    assert code == 0
    wav_files = sorted(path.relative_to(output).as_posix() for path in output.rglob("*.wav"))
    sfz_files = sorted(path.relative_to(output).as_posix() for path in output.rglob("*.sfz"))
    assert len(wav_files) == 1
    assert wav_files[0].startswith("_samples/physical/S01__")
    assert sfz_files == []
    assert not any(
        path.name
        in {
            "selection.axklib.json",
            "selection_exports.csv",
            "selection_exports.json",
            "sfz_exports.csv",
            "sfz_exports.json",
        }
        for path in output.rglob("*")
    )


def test_subcommand_help_output_is_available(capsys: pytest.CaptureFixture[str]) -> None:
    for argv in (
        ["inventory", "--help"],
        ["extract", "wav", "--help"],
        ["extract", "sfz", "--help"],
    ):
        with pytest.raises(SystemExit) as exc_info:
            axklibtool.main(list(argv))
        assert exc_info.value.code == 0

    captured = capsys.readouterr()
    help_text = captured.out.lower()
    assert "decode object inventory" in help_text
    assert "export targeted wavs" in help_text
    assert "generate sfz files" in help_text


def test_extract_waves_command_is_not_public(capsys: pytest.CaptureFixture[str]) -> None:
    with pytest.raises(SystemExit) as exc_info:
        axklibtool.main(["extract", "waves", "--help"])

    captured = capsys.readouterr()
    assert exc_info.value.code == 2
    assert "invalid choice" in captured.err


def test_extract_sfz_file_fails_on_existing_targets_without_overwrite(tmp_path: Path) -> None:
    source = tmp_path / "sample.smpl"
    output = tmp_path / "sfz"
    _write_standalone_smpl(source)

    first = axklibtool.main(["extract", "sfz", "file", "-o", str(output), str(source)])
    second = axklibtool.main(["extract", "sfz", "file", "-o", str(output), str(source)])

    assert first == 0
    assert second == 1


def test_debug_flag_controls_internal_traceback(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    output = tmp_path / "validate"

    code = axklibtool.main(["validate", "-o", str(output)])
    captured = capsys.readouterr()
    assert code == 4
    assert "internal error:" in captured.err
    assert "Traceback" not in captured.err

    debug_output = tmp_path / "validate-debug"
    debug_code = axklibtool.main(["--debug", "validate", "-o", str(debug_output)])
    debug_captured = capsys.readouterr()
    assert debug_code == 4
    assert "Traceback" in debug_captured.err


def test_corpus_audit_writes_input_manifest(tmp_path: Path) -> None:
    source = tmp_path / "sample.smpl"
    output = tmp_path / "audit"
    _write_standalone_smpl(source)

    code = axklibtool.main(["corpus", "audit", "--skip-wave-smoke", "-o", str(output), str(source)])

    assert code in {0, 1}
    assert (output / "input_manifest.csv").exists()
    assert (output / "input_manifest.json").exists()
    assert (output / "_schemas" / "input_manifest.schema.json").exists()


def test_validate_fails_if_detail_report_generation_fails(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    image = tmp_path / "source.hds"
    image.write_bytes(b"fixture")
    output = tmp_path / "validate"

    def fail_detail_reports(*_args: object, **_kwargs: object) -> list[object]:
        raise RuntimeError("detail failed")

    monkeypatch.setattr(axklibtool, "_write_validation_detail_reports", fail_detail_reports)

    code = axklibtool.main(["validate", "-o", str(output), str(image)])

    assert code == 4


def test_info_defaults_to_tree_and_summary_remains_available(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    source = tmp_path / "sample.smpl"
    _write_standalone_smpl(source)

    tree_code = axklibtool.main(["info", str(source)])
    tree_output = capsys.readouterr().out
    summary_code = axklibtool.main(["info", "--format", "summary", str(source)])
    summary_output = capsys.readouterr().out

    assert tree_code == 0
    assert "Standalone object" in tree_output
    assert "Waveforms" in tree_output
    assert summary_code == 0
    assert "objects=1" in summary_output
