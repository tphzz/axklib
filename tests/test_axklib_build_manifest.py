from __future__ import annotations

import json
import wave
from pathlib import Path

import pytest

from axklib.build_manifest import (
    build_hds_from_manifest,
    load_hds_build_manifest,
    parse_hds_build_manifest,
)
from axklib.containers import load_sfs_objects


def _write_wav(path: Path) -> None:
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(44_100)
        wav.writeframes(b"\x00\x00\x01\x00\xff\xff")


def _manifest(source_wav: str = "tone.wav") -> dict[str, object]:
    return {
        "schema_version": "1.0",
        "size_bytes": 256 * 1024 * 1024,
        "partitions": [
            {
                "name": "hd1",
                "volumes": [
                    {
                        "name": "Manifest Vol",
                        "waveforms": [
                            {
                                "id": "tone",
                                "name": "Tone0001",
                                "path": source_wav,
                                "root_key": 60,
                            }
                        ],
                        "sample_banks": [
                            {
                                "name": "Bank0001",
                                "waveform_id": "tone",
                                "root_key": 60,
                                "key_low": 0,
                                "key_high": 127,
                                "level": 100,
                            }
                        ],
                    }
                ],
            }
        ],
    }


def test_hds_build_manifest_resolves_wav_paths_and_builds_objects(tmp_path: Path) -> None:
    wav_path = tmp_path / "audio" / "tone.wav"
    wav_path.parent.mkdir()
    _write_wav(wav_path)
    manifest_path = tmp_path / "build.json"
    manifest_path.write_text(json.dumps(_manifest("audio/tone.wav")), encoding="utf-8")

    manifest = load_hds_build_manifest(manifest_path)
    output = tmp_path / "HD00_512_manifest.hds"
    result = build_hds_from_manifest(manifest, output)

    assert manifest.partitions[0].volumes[0].waveforms[0].path == wav_path
    assert result.partitions == 1
    assert [(item.object_type, item.name) for item in result.objects] == [
        ("SMPL", "Tone0001"),
        ("SBNK", "Bank0001"),
    ]
    assert [(item.object_type, item.name) for item in load_sfs_objects(output)] == [
        ("SMPL", "Tone0001"),
        ("SBNK", "Bank0001"),
    ]


def test_hds_build_manifest_rejects_unknown_fields() -> None:
    value = _manifest()
    value["unexpected"] = True

    with pytest.raises(ValueError, match="unknown fields: unexpected"):
        parse_hds_build_manifest(value)


def test_hds_build_manifest_rejects_unknown_waveform_reference() -> None:
    value = _manifest()
    partitions = value["partitions"]
    assert isinstance(partitions, list)
    partition = partitions[0]
    assert isinstance(partition, dict)
    volumes = partition["volumes"]
    assert isinstance(volumes, list)
    volume = volumes[0]
    assert isinstance(volume, dict)
    sample_banks = volume["sample_banks"]
    assert isinstance(sample_banks, list)
    bank = sample_banks[0]
    assert isinstance(bank, dict)
    bank["waveform_id"] = "missing"

    with pytest.raises(ValueError, match="references unknown waveform 'missing'"):
        parse_hds_build_manifest(value)


def test_hds_build_manifest_builds_plural_sample_bank_group_members(tmp_path: Path) -> None:
    _write_wav(tmp_path / "tone.wav")
    value = _manifest()
    partitions = value["partitions"]
    assert isinstance(partitions, list)
    partition = partitions[0]
    assert isinstance(partition, dict)
    volumes = partition["volumes"]
    assert isinstance(volumes, list)
    volume = volumes[0]
    assert isinstance(volume, dict)
    sample_banks = volume["sample_banks"]
    assert isinstance(sample_banks, list)
    original = sample_banks[0]
    assert isinstance(original, dict)
    sample_banks.extend(
        [
            {**original, "name": "Bank0002"},
            {**original, "name": "Bank0003"},
            {**original, "name": "Direct"},
        ]
    )
    volume["sample_bank_groups"] = [
        {
            "name": "Key Group",
            "member_sample_banks": ["Bank0001", "Bank0002", "Bank0003"],
        }
    ]
    volume["programs"] = [
        {
            "number": 1,
            "assignments": [
                {"sample_bank_group": "Key Group", "receive_channel": 1},
                {"sample_bank": "Direct", "receive_channel": 2},
            ],
        }
    ]

    manifest = parse_hds_build_manifest(value, base_dir=tmp_path)
    output = tmp_path / "plural-group.hds"
    build_hds_from_manifest(manifest, output)

    group = manifest.partitions[0].volumes[0].sample_bank_groups[0]
    assert group.member_sample_banks == ("Bank0001", "Bank0002", "Bank0003")
    objects = load_sfs_objects(output)
    sbac = next(item for item in objects if str(item.object_type) == "SBAC")
    assert sbac.payload[0x144] == 3


@pytest.mark.parametrize(
    "members, message",
    [
        ([], "must contain 1..3"),
        (["Bank0001"] * 2, "duplicate member"),
        (["Bank0001"] * 4, "must contain 1..3"),
    ],
)
def test_hds_build_manifest_rejects_invalid_plural_group_members(
    members: list[str], message: str
) -> None:
    value = _manifest()
    partitions = value["partitions"]
    assert isinstance(partitions, list)
    partition = partitions[0]
    assert isinstance(partition, dict)
    volumes = partition["volumes"]
    assert isinstance(volumes, list)
    volume = volumes[0]
    assert isinstance(volume, dict)
    volume["sample_bank_groups"] = [{"name": "Key Group", "member_sample_banks": members}]

    with pytest.raises(ValueError, match=message):
        parse_hds_build_manifest(value)


def test_hds_build_manifest_builds_general_multi_partition_geometry(tmp_path: Path) -> None:
    value = _manifest()
    value["size_bytes"] = 256 * 1024 * 1024
    partitions = value["partitions"]
    assert isinstance(partitions, list)
    first_partition = partitions[0]
    assert isinstance(first_partition, dict)
    first_volumes = first_partition["volumes"]
    assert isinstance(first_volumes, list)
    first_volume = first_volumes[0]
    assert isinstance(first_volume, dict)
    first_volume["waveforms"] = []
    first_volume["sample_banks"] = []
    partitions.append(
        {
            "name": "hd2",
            "volumes": [
                {
                    "name": "Empty Vol",
                    "waveforms": [],
                    "sample_banks": [],
                }
            ],
        }
    )
    manifest = parse_hds_build_manifest(value, base_dir=tmp_path)
    output = tmp_path / "general.hds"

    result = build_hds_from_manifest(manifest, output)

    assert result.partitions == 2
    assert [layout.start_sector for layout in result.partition_layouts] == [3, 262_146]
    assert [layout.sector_count for layout in result.partition_layouts] == [262_142] * 2


def test_hds_build_manifest_refuses_overwrite(tmp_path: Path) -> None:
    manifest = parse_hds_build_manifest(_manifest(), base_dir=tmp_path)
    output = tmp_path / "existing.hds"
    output.write_bytes(b"keep")

    with pytest.raises(FileExistsError, match="output already exists"):
        build_hds_from_manifest(manifest, output)

    assert output.read_bytes() == b"keep"
