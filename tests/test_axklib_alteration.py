from __future__ import annotations

import hashlib
import wave
from pathlib import Path

import pytest

from axklib.alteration import alter_hds, parse_alteration_manifest
from axklib.containers import load_sfs_objects, sfs_dump, sfs_inventory
from axklib.relationships import build_relationship_graph
from axklib.write import HdsImageBuilder


def _write_wav(path: Path, *, frames: int = 8) -> None:
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(44_100)
        wav.writeframes(b"\x00\x00\xe8\x03" * frames)


def _add_volume(partition: object, name: str, wav_path: Path) -> None:
    volume = partition.add_volume(name)  # type: ignore[attr-defined]
    waveform = volume.add_waveform_from_wav(
        name=f"{name[:8]}W",
        path=wav_path,
        root_key=60,
    )
    volume.add_sample_bank(
        name=f"{name[:8]}B",
        waveform=waveform,
        root_key=60,
        key_low=0,
        key_high=127,
        level=100,
    )


def _source_image(tmp_path: Path, names: tuple[str, ...] = ("Keep", "Remove")) -> Path:
    wav_path = tmp_path / "source.wav"
    _write_wav(wav_path)
    builder = HdsImageBuilder(size_bytes=8 * 1024 * 1024)
    partition = builder.add_partition("hd1")
    for name in names:
        _add_volume(partition, name, wav_path)
    output = tmp_path / "source.hds"
    builder.write(output)
    return output


def _referenced_bank_source(tmp_path: Path) -> Path:
    wav_path = tmp_path / "referenced.wav"
    _write_wav(wav_path)
    builder = HdsImageBuilder(size_bytes=8 * 1024 * 1024)
    partition = builder.add_partition("hd1")
    volume = partition.add_volume("Banks")
    waveform = volume.add_waveform_from_wav(name="Shared Wave", path=wav_path, root_key=60)
    member_a = volume.add_sample_bank(
        name="Member A", waveform=waveform, root_key=60, key_low=60, key_high=60
    )
    member_b = volume.add_sample_bank(
        name="Member B", waveform=waveform, root_key=62, key_low=62, key_high=62
    )
    direct = volume.add_sample_bank(
        name="Direct", waveform=waveform, root_key=64, key_low=64, key_high=64
    )
    volume.add_sample_bank(
        name="Unused", waveform=waveform, root_key=65, key_low=65, key_high=65
    )
    group = volume.add_sample_bank_group(name="Group", members=(member_a, member_b))
    program = volume.add_program(number=1)
    program.assign_sample_bank_group(group, receive_channel=1)
    program.assign_sample_bank(direct, receive_channel=2)
    output = tmp_path / "referenced.hds"
    builder.write(output)
    return output


def _insert_volume(wav_name: str = "insert.wav", *, name: str = "Inserted") -> dict[str, object]:
    return {
        "name": name,
        "waveforms": [
            {
                "id": "wave",
                "name": "InsertedW",
                "path": wav_name,
                "root_key": 64,
            }
        ],
        "sample_banks": [
            {
                "name": "InsertedB",
                "waveform_id": "wave",
                "root_key": 64,
                "key_low": 12,
                "key_high": 100,
                "level": 90,
            }
        ],
    }


def _manifest(*operations: dict[str, object]) -> dict[str, object]:
    return {"schema_version": "1.0", "operations": list(operations)}


def _volume_names(path: Path) -> list[str]:
    parsed = sfs_dump.parse_image(
        path, sfs_dump.ReadOptions(max_nodes=4, include_node_payloads=False)
    )
    partitions = parsed["partitions"]
    assert isinstance(partitions, list)
    partition = partitions[0]
    assert isinstance(partition, dict)
    rows = sfs_inventory.scan_ynode_records(path, partition, [], sector_size=512)
    records = [sfs_inventory.ynode_to_index_record(row) for row in rows]
    _directories, entries = sfs_inventory.walk_directories(path, partition, records, rows)
    return [
        entry.name
        for entry in entries
        if entry.directory_id == 1
        and entry.name not in {".", "..", "sfserrlog", "sfserram"}
    ]


def test_delete_volume_preserves_unrelated_objects_and_can_leave_partition_empty(
    tmp_path: Path,
) -> None:
    source = _source_image(tmp_path, ("Keep", "Remove"))
    before = {
        item.name: item.payload
        for item in load_sfs_objects(source)
        if item.name.startswith("Keep")
    }
    transaction = parse_alteration_manifest(
        _manifest(
            {
                "id": "delete-remove",
                "type": "delete_volume",
                "partition_index": 0,
                "volume_name": "Remove",
            }
        )
    )
    output = tmp_path / "deleted.hds"

    result = alter_hds(source, transaction, output_path=output)

    assert result.applied
    assert _volume_names(output) == ["Keep"]
    assert {
        item.name: item.payload
        for item in load_sfs_objects(output)
        if item.name.startswith("Keep")
    } == before

    empty_output = tmp_path / "empty.hds"
    empty_transaction = parse_alteration_manifest(
        _manifest(
            {
                "id": "delete-last",
                "type": "delete_volume",
                "partition_index": 0,
                "volume_name": "Keep",
            }
        )
    )
    alter_hds(output, empty_transaction, output_path=empty_output)
    assert _volume_names(empty_output) == []


def test_ordered_delete_then_insert_reuses_logical_state(tmp_path: Path) -> None:
    source = _source_image(tmp_path, ("Replace",))
    _write_wav(tmp_path / "insert.wav", frames=16)
    transaction = parse_alteration_manifest(
        _manifest(
            {
                "id": "delete-old",
                "type": "delete_volume",
                "partition_index": 0,
                "volume_name": "Replace",
            },
            {
                "id": "insert-new",
                "type": "insert_volume",
                "partition_index": 0,
                "volume": _insert_volume(name="Replace"),
            },
        ),
        base_dir=tmp_path,
    )
    output = tmp_path / "replaced.hds"

    result = alter_hds(source, transaction, output_path=output)

    assert [report.id for report in result.operations] == ["delete-old", "insert-new"]
    assert _volume_names(output) == ["Replace"]
    assert [(item.object_type, item.name) for item in load_sfs_objects(output)] == [
        ("SMPL", "InsertedW"),
        ("SBNK", "InsertedB"),
    ]


def test_dry_run_does_not_modify_source_and_existing_output_is_refused(tmp_path: Path) -> None:
    source = _source_image(tmp_path, ("Remove",))
    before = hashlib.sha256(source.read_bytes()).digest()
    transaction = parse_alteration_manifest(
        _manifest(
            {
                "id": "delete",
                "type": "delete_volume",
                "partition_index": 0,
                "volume_name": "Remove",
            }
        )
    )

    result = alter_hds(source, transaction)

    assert not result.applied
    assert hashlib.sha256(source.read_bytes()).digest() == before
    existing = tmp_path / "existing.hds"
    existing.write_bytes(b"keep")
    with pytest.raises(FileExistsError, match="output already exists"):
        alter_hds(source, transaction, output_path=existing)
    assert existing.read_bytes() == b"keep"


def test_insert_uses_continuation_extents_in_fragmented_free_space(tmp_path: Path) -> None:
    source = _source_image(tmp_path, ("Keep",))
    _write_wav(tmp_path / "insert.wav", frames=12_000)
    parsed = sfs_dump.parse_image(
        source, sfs_dump.ReadOptions(max_nodes=1, include_node_payloads=False)
    )
    partitions = parsed["partitions"]
    assert isinstance(partitions, list)
    partition = partitions[0]
    assert isinstance(partition, dict)
    derived = partition["derived"]
    fields = partition["fields"]
    assert isinstance(derived, dict) and isinstance(fields, dict)
    bitmap_offset = derived["bitmap_absolute_offset"]
    assert isinstance(bitmap_offset, int)
    cluster_count_field = fields["number_of_clusters"]
    index_field = fields["cluster_offset_to_directory_index"]
    index_span_field = fields["unknown_static_0x0a8"]
    assert isinstance(cluster_count_field, dict)
    assert isinstance(index_field, dict)
    assert isinstance(index_span_field, dict)
    cluster_count = cluster_count_field["value"]
    first_payload = index_field["value"] + index_span_field["value"]
    assert isinstance(cluster_count, int) and isinstance(first_payload, int)
    data = bytearray(source.read_bytes())
    bitmap_size = (cluster_count + 7) // 8
    bitmap = bytearray(data[bitmap_offset : bitmap_offset + bitmap_size])
    for cluster in range(first_payload, cluster_count):
        if cluster % 2:
            bitmap[cluster // 8] |= 1 << (7 - cluster % 8)
    data[bitmap_offset : bitmap_offset + bitmap_size] = bitmap
    start_sector = partition["start_sector"]
    assert isinstance(start_sector, int)
    mirror_offset = start_sector * 512 + 2048
    data[mirror_offset : mirror_offset + 512] = bitmap[:512]
    source.write_bytes(data)
    transaction = parse_alteration_manifest(
        _manifest(
            {
                "id": "insert-fragmented",
                "type": "insert_volume",
                "partition_index": 0,
                "volume": _insert_volume(),
            }
        ),
        base_dir=tmp_path,
    )
    output = tmp_path / "fragmented.hds"

    alter_hds(source, transaction, output_path=output)

    objects = load_sfs_objects(output)
    inserted = next(item for item in objects if item.name == "InsertedW")
    assert inserted.payload
    assert _volume_names(output) == ["Keep", "Inserted"]


def test_manifest_rejects_duplicate_ids_and_unimplemented_operation() -> None:
    duplicate = {
        "id": "same",
        "type": "delete_volume",
        "partition_index": 0,
        "volume_name": "A",
    }
    with pytest.raises(ValueError, match="duplicate operation id"):
        parse_alteration_manifest(_manifest(duplicate, duplicate))
    with pytest.raises(ValueError, match="unsupported alteration operation"):
        parse_alteration_manifest(
            _manifest(
                {
                    "id": "future",
                    "type": "insert_prog",
                    "partition_index": 0,
                    "volume_name": "A",
                }
            )
        )
    with pytest.raises(ValueError, match="must name an earlier operation"):
        parse_alteration_manifest(
            _manifest(
                {
                    "id": "forward",
                    "type": "delete_volume",
                    "partition_index": {"operation_ref": "later"},
                    "volume_name": "A",
                },
                {
                    "id": "later",
                    "type": "delete_volume",
                    "partition_index": 0,
                    "volume_name": "B",
                },
            )
        )


def test_operation_reference_uses_earlier_result_scope(tmp_path: Path) -> None:
    source = _source_image(tmp_path, ("First", "Second"))
    transaction = parse_alteration_manifest(
        _manifest(
            {
                "id": "first",
                "type": "delete_volume",
                "partition_index": 0,
                "volume_name": "First",
            },
            {
                "id": "second",
                "type": "delete_volume",
                "partition_index": {"operation_ref": "first"},
                "volume_name": "Second",
            },
        )
    )
    output = tmp_path / "referenced.hds"

    alter_hds(source, transaction, output_path=output)

    assert _volume_names(output) == []


@pytest.mark.parametrize(
    "bank_name, message",
    [
        ("Member A", "referenced by SBAC"),
        ("Direct", "Program-link bitmap"),
    ],
)
def test_delete_sbnk_rejects_sbac_and_program_references(
    tmp_path: Path, bank_name: str, message: str
) -> None:
    source = _referenced_bank_source(tmp_path)
    transaction = parse_alteration_manifest(
        _manifest(
            {
                "id": "delete-bank",
                "type": "delete_sbnk",
                "partition_index": 0,
                "volume_name": "Banks",
                "sample_bank_name": bank_name,
            }
        )
    )

    with pytest.raises(ValueError, match=message):
        alter_hds(source, transaction)


def test_delete_sbnk_removes_only_unreferenced_bank_and_preserves_waveform(
    tmp_path: Path,
) -> None:
    source = _referenced_bank_source(tmp_path)
    source_wave = next(item for item in load_sfs_objects(source) if item.name == "Shared Wave")
    transaction = parse_alteration_manifest(
        _manifest(
            {
                "id": "delete-unused",
                "type": "delete_sbnk",
                "partition_index": 0,
                "volume_name": "Banks",
                "sample_bank_name": "Unused",
            }
        )
    )
    output = tmp_path / "bank-deleted.hds"

    result = alter_hds(source, transaction, output_path=output)

    names = {item.name for item in load_sfs_objects(output)}
    assert "Unused" not in names
    assert {"Member A", "Member B", "Direct", "Shared Wave"}.issubset(names)
    output_wave = next(item for item in load_sfs_objects(output) if item.name == "Shared Wave")
    assert output_wave.payload == source_wave.payload
    assert result.operations[0].freed_clusters == 2


def test_insert_sbnk_references_existing_waveform_and_can_replace_deleted_name(
    tmp_path: Path,
) -> None:
    source = _referenced_bank_source(tmp_path)
    transaction = parse_alteration_manifest(
        _manifest(
            {
                "id": "delete-unused",
                "type": "delete_sbnk",
                "partition_index": 0,
                "volume_name": "Banks",
                "sample_bank_name": "Unused",
            },
            {
                "id": "insert-unused",
                "type": "insert_sbnk",
                "partition_index": {"operation_ref": "delete-unused"},
                "volume_name": "Banks",
                "sample_bank": {
                    "name": "Unused",
                    "waveform_name": "Shared Wave",
                    "root_key": 67,
                    "key_low": 66,
                    "key_high": 68,
                    "level": 91,
                },
            },
        )
    )
    output = tmp_path / "bank-replaced.hds"

    result = alter_hds(source, transaction, output_path=output)

    objects = load_sfs_objects(output)
    bank = next(item for item in objects if item.name == "Unused")
    graph = build_relationship_graph(objects)
    member_edges = [
        row
        for row in graph.relationships
        if row.source_key == bank.object_key and row.relationship_type.endswith("_TO_SMPL")
    ]
    assert len(member_edges) == 1
    assert member_edges[0].quality == "Known"
    assert [report.object_name for report in result.operations] == ["Unused", "Unused"]


def test_insert_sbnk_builds_two_member_bank_from_existing_waveforms(tmp_path: Path) -> None:
    left_path = tmp_path / "left.wav"
    right_path = tmp_path / "right.wav"
    _write_wav(left_path, frames=16)
    _write_wav(right_path, frames=16)
    builder = HdsImageBuilder(size_bytes=8 * 1024 * 1024)
    partition = builder.add_partition("hd1")
    volume = partition.add_volume("Stereo")
    volume.add_waveform_from_wav(name="Stereo L", path=left_path, root_key=60)
    volume.add_waveform_from_wav(name="Stereo R", path=right_path, root_key=60)
    source = tmp_path / "stereo-source.hds"
    builder.write(source)
    transaction = parse_alteration_manifest(
        _manifest(
            {
                "id": "insert-stereo",
                "type": "insert_sbnk",
                "partition_index": 0,
                "volume_name": "Stereo",
                "sample_bank": {
                    "name": "Stereo Bank",
                    "waveform_name": "Stereo L",
                    "right_waveform_name": "Stereo R",
                    "root_key": 60,
                    "key_low": 0,
                    "key_high": 127,
                    "level": 100,
                },
            }
        )
    )
    output = tmp_path / "stereo-bank.hds"

    alter_hds(source, transaction, output_path=output)

    objects = load_sfs_objects(output)
    bank = next(item for item in objects if item.name == "Stereo Bank")
    edges = [row for row in build_relationship_graph(objects).relationships if row.source_key == bank.object_key]
    member_edges = [row for row in edges if row.relationship_type.endswith("_TO_SMPL")]
    assert len(member_edges) == 2
    assert all(row.quality == "Known" for row in member_edges)


def test_insert_sbnk_rejects_duplicate_bank_and_missing_waveform(tmp_path: Path) -> None:
    source = _referenced_bank_source(tmp_path)
    base_operation = {
        "id": "insert",
        "type": "insert_sbnk",
        "partition_index": 0,
        "volume_name": "Banks",
        "sample_bank": {
            "name": "Unused",
            "waveform_name": "Shared Wave",
            "root_key": 60,
            "key_low": 0,
            "key_high": 127,
        },
    }
    duplicate = parse_alteration_manifest(_manifest(base_operation))
    with pytest.raises(ValueError, match="already contains sample bank"):
        alter_hds(source, duplicate)
    missing_operation = dict(base_operation)
    missing_bank = dict(base_operation["sample_bank"])
    missing_bank["name"] = "New Bank"
    missing_bank["waveform_name"] = "Missing Wave"
    missing_operation["sample_bank"] = missing_bank
    missing = parse_alteration_manifest(_manifest(missing_operation))
    with pytest.raises(ValueError, match="requires exactly one SMPL"):
        alter_hds(source, missing)
