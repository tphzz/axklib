from __future__ import annotations

import wave
from pathlib import Path

import pytest

from axklib.audio import decode_waveform
from axklib.containers import load_sfs_objects
from axklib.model import AxklibObjectType
from axklib.relationships import build_relationship_graph
from axklib.write import MAX_IMAGE_SIZE_BYTES, HdsImageBuilder


def _write_mono_wav(
    path: Path, samples: tuple[int, ...] = (0, 1000, -1000, 32767, -32768)
) -> bytes:
    pcm = b"".join(sample.to_bytes(2, "little", signed=True) for sample in samples)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(44_100)
        wav.writeframes(pcm)
    return pcm


def _be32(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], "big")


def _index_record(data: bytes, sfs_id: int) -> bytes:
    partition_start = _be32(data, 0xA8)
    partition_offset = partition_start * 512
    index_cluster = _be32(data, partition_offset + 0xA4)
    index_offset = (partition_start + index_cluster * 2) * 512
    block = sfs_id // 14
    slot = sfs_id % 14
    record_offset = index_offset + block * 1024 + slot * 72
    return data[record_offset : record_offset + 72]


def _index_record_in_partition(data: bytes, partition_index: int, sfs_id: int) -> bytes:
    partition_start = _be32(data, 0xA8 + partition_index * 8)
    partition_offset = partition_start * 512
    index_cluster = _be32(data, partition_offset + 0xA4)
    index_offset = (partition_start + index_cluster * 2) * 512
    block = sfs_id // 14
    slot = sfs_id % 14
    record_offset = index_offset + block * 1024 + slot * 72
    return data[record_offset : record_offset + 72]


def _record_payload(data: bytes, record: bytes) -> bytes:
    partition_start = _be32(data, 0xA8)
    cluster_offset = _be32(record, 0x0A)
    size = _be32(record, 0x06)
    payload_offset = (partition_start + cluster_offset * 2) * 512
    return data[payload_offset : payload_offset + size]


def _record_payload_in_partition(data: bytes, partition_index: int, record: bytes) -> bytes:
    partition_start = _be32(data, 0xA8 + partition_index * 8)
    cluster_offset = _be32(record, 0x0A)
    size = _be32(record, 0x06)
    payload_offset = (partition_start + cluster_offset * 2) * 512
    return data[payload_offset : payload_offset + size]


def _entry_names(payload: bytes) -> list[str]:
    names: list[str] = []
    for offset in range(0, len(payload), 32):
        entry = payload[offset : offset + 32]
        if len(entry) < 32 or not any(entry):
            continue
        raw_name = entry[8:32].split(b"\x00", 1)[0]
        names.append(raw_name.decode("ascii"))
    return names


def _assert_partition_header_residue_zero(data: bytes, partition_index: int) -> None:
    partition_start = _be32(data, 0xA8 + partition_index * 8)
    partition_offset = partition_start * 512
    assert data[partition_offset + 0x1BC : partition_offset + 0x1E4] == b"\x00" * 40
    assert data[
        partition_offset + 1024 + 0x1BC : partition_offset + 1024 + 0x1E4
    ] == b"\x00" * 40


def test_hds_writer_rejects_images_above_a_series_cap() -> None:
    with pytest.raises(ValueError, match="exceeds A-series cap"):
        HdsImageBuilder(size_bytes=MAX_IMAGE_SIZE_BYTES + 512)


def test_hds_writer_rejects_single_partition_above_one_gib() -> None:
    builder = HdsImageBuilder(size_bytes=MAX_IMAGE_SIZE_BYTES)
    builder.add_partition("hd1")

    with pytest.raises(ValueError, match="exceed 1 GiB"):
        builder.plan()


def test_hds_writer_rejects_unproven_multi_partition_profile() -> None:
    builder = HdsImageBuilder(size_bytes=256 * 1024 * 1024)
    builder.add_partition("hd1")
    builder.add_partition("hd2")

    with pytest.raises(
        ValueError, match="512 MiB / two-partition and sparse 768 MiB / three-partition profiles"
    ):
        builder.plan()


def test_hds_writer_creates_parseable_current_smpl_and_sbnk(tmp_path: Path) -> None:
    source_wav = tmp_path / "tone.wav"
    expected_pcm = _write_mono_wav(source_wav)
    expected_stored_pcm = expected_pcm + expected_pcm[:8]
    image_path = tmp_path / "HD00_512_writer_minimal.hds"

    builder = HdsImageBuilder(size_bytes=4 * 1024 * 1024)
    partition = builder.add_partition("hd1")
    volume = partition.add_volume("WriterVol")
    waveform = volume.add_waveform_from_wav(name="Tone0001", path=source_wav, root_key=60)
    volume.add_sample_bank(
        name="Bank0001",
        waveform=waveform,
        root_key=60,
        key_low=0,
        key_high=127,
        level=100,
    )

    result = builder.write(image_path)

    assert result.path == image_path
    assert result.partitions == 1
    assert [(item.object_type, item.name) for item in result.objects] == [
        ("SMPL", "Tone0001"),
        ("SBNK", "Bank0001"),
    ]

    image_bytes = image_path.read_bytes()
    root_record = _index_record(image_bytes, 1)
    volume_record = _index_record(image_bytes, 3)
    smpl_category_record = _index_record(image_bytes, 4)
    smpl_object_record = _index_record(image_bytes, 9)
    sbnk_object_record = _index_record(image_bytes, 10)
    assert int.from_bytes(root_record[0x04:0x06], "big") == 2
    assert int.from_bytes(smpl_object_record[0x04:0x06], "big") == 2
    assert int.from_bytes(sbnk_object_record[0x04:0x06], "big") == 2
    root_payload = _record_payload(image_bytes, root_record)
    volume_payload = _record_payload(image_bytes, volume_record)
    smpl_payload = _record_payload(image_bytes, smpl_object_record)
    sbnk_payload = _record_payload(image_bytes, sbnk_object_record)
    assert len(smpl_payload) == 0x200 + len(expected_stored_pcm)
    assert smpl_payload[0x10:0x20] == b"\x00\x00\x02\x00\x00\x00\x00\x03\x00\x00\x00\x7c" + len(
        expected_stored_pcm
    ).to_bytes(4, "big")
    assert smpl_payload[0x20:0x24] == len(expected_stored_pcm).to_bytes(4, "big")
    assert (
        sbnk_payload[0x10:0x20]
        == b"\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x01\x34\x00\x00\x01\x58"
    )
    assert smpl_payload[0x54:0x64] == b"\x00" * 16
    assert smpl_payload[0x68:0x70] == bytes.fromhex("01443840016b1d02")
    assert smpl_payload[0x74:0x7C] == bytes.fromhex("01443840016b1dbc")
    assert sbnk_payload[0x68:0x6C] == bytes.fromhex("01443c30")
    assert sbnk_payload[0x98:0x9C] == bytes.fromhex("01443c30")
    assert sbnk_payload[0xA0:0xA4] == bytes.fromhex("016b1dbc")
    assert sbnk_payload[0xA8:0xB8] == bytes.fromhex("4a04012047050120490b01e0480c01e0")
    assert len(sbnk_payload) == 0x188
    assert root_payload[10:12] == b"\x4f\x58"
    assert root_payload[16:20] == b"\x00\x17\x82\x1a"
    assert root_payload[24:28] == b"\xff" * 4
    assert root_payload[43] == 1
    assert root_payload[48:52] == b"\x00\x17\x83\xc8"
    assert volume_payload[10:12] == b"\x4f\x58"
    assert volume_payload[43] == 3
    assert root_record[0x42:0x46] == b"\x94dir"
    assert volume_record[0x42:0x46] == b"\x94dir"
    assert smpl_category_record[0x42:0x46] == b"\x94dir"
    assert smpl_object_record[0x42:0x48] == b"\x9e\x00\x00\x00\x00\x01"
    assert sbnk_object_record[0x42:0x48] == b"\x9e\x00\x00\x00\x00\x01"
    assert _entry_names(root_payload) == [
        ".",
        "..",
        "sfserrlog",
        "sfserram",
        "WriterVol       ",
    ]
    assert _entry_names(volume_payload) == [
        ".",
        "..",
        "SMPL",
        "SBNK",
        "SBAC",
        "SEQU",
        "PROG",
    ]

    objects = load_sfs_objects(image_path)
    assert sorted((str(item.object_type), item.name) for item in objects) == [
        ("SBNK", "Bank0001"),
        ("SMPL", "Tone0001"),
    ]

    smpl = next(item for item in objects if item.object_type == AxklibObjectType.SMPL)
    waveform_data = decode_waveform(smpl)
    assert waveform_data.pcm == expected_stored_pcm
    assert waveform_data.sample_rate == 44_100
    assert waveform_data.root_key == 60
    assert waveform_data.loop_mode == 1

    graph = build_relationship_graph(objects)
    relationships = [
        row for row in graph.relationships if row.relationship_type == "SBNK_LEFT_MEMBER_TO_SMPL"
    ]
    assert len(relationships) == 1
    assert relationships[0].quality == "Known"
    assert relationships[0].basis == "sbnk-member-link+name"


def test_hds_writer_creates_two_waveforms_and_banks_with_distinct_links(tmp_path: Path) -> None:
    source_a = tmp_path / "tone-a.wav"
    source_b = tmp_path / "tone-b.wav"
    _write_mono_wav(source_a, samples=(0, 1200, -1200, 2400, -2400))
    _write_mono_wav(source_b, samples=(300, -300, 900, -900, 0))
    image_path = tmp_path / "HD00_512_writer_two_banks.hds"

    builder = HdsImageBuilder(size_bytes=4 * 1024 * 1024)
    partition = builder.add_partition("hd1")
    volume = partition.add_volume("WriterVol")
    wave_a = volume.add_waveform_from_wav(name="ToneA", path=source_a, root_key=60)
    wave_b = volume.add_waveform_from_wav(name="ToneB", path=source_b, root_key=64)
    volume.add_sample_bank(
        name="BankA",
        waveform=wave_a,
        root_key=60,
        key_low=0,
        key_high=63,
        level=100,
    )
    volume.add_sample_bank(
        name="BankB",
        waveform=wave_b,
        root_key=64,
        key_low=64,
        key_high=127,
        level=90,
    )

    result = builder.write(image_path)

    assert [(item.object_type, item.name, item.sfs_id) for item in result.objects] == [
        ("SMPL", "ToneA", 9),
        ("SMPL", "ToneB", 10),
        ("SBNK", "BankA", 11),
        ("SBNK", "BankB", 12),
    ]
    image_bytes = image_path.read_bytes()
    smpl_a = _record_payload(image_bytes, _index_record(image_bytes, 9))
    smpl_b = _record_payload(image_bytes, _index_record(image_bytes, 10))
    sbnk_a = _record_payload(image_bytes, _index_record(image_bytes, 11))
    sbnk_b = _record_payload(image_bytes, _index_record(image_bytes, 12))

    assert smpl_a[0x74:0x7C] == bytes.fromhex("01443840016b1dbc")
    assert smpl_b[0x74:0x7C] == bytes.fromhex("01443840016b1ebc")
    assert sbnk_a[0xA0:0xA4] == bytes.fromhex("016b1dbc")
    assert sbnk_b[0xA0:0xA4] == bytes.fromhex("016b1ebc")
    assert sbnk_a[0x0D6] == 60
    assert sbnk_b[0x0D6] == 64
    assert sbnk_a[0x0E2:0x0E4] == bytes([63, 0])
    assert sbnk_b[0x0E2:0x0E4] == bytes([127, 64])
    assert sbnk_a[0x116] == 100
    assert sbnk_b[0x116] == 90

    objects = load_sfs_objects(image_path)
    graph = build_relationship_graph(objects)
    relationships = [
        row for row in graph.relationships if row.relationship_type == "SBNK_LEFT_MEMBER_TO_SMPL"
    ]
    assert sorted(
        (row.source_key, row.target_key, row.quality, row.basis) for row in relationships
    ) == [
        ("p0:sfs11", "p0:sfs9", "Known", "sbnk-member-link+name"),
        ("p0:sfs12", "p0:sfs10", "Known", "sbnk-member-link+name"),
    ]


def test_hds_writer_creates_two_volumes_without_cross_linking(tmp_path: Path) -> None:
    source_a = tmp_path / "volume-a.wav"
    source_b = tmp_path / "volume-b.wav"
    _write_mono_wav(source_a, samples=(0, 800, -800, 1600, -1600))
    _write_mono_wav(source_b, samples=(200, -200, 600, -600, 0))
    image_path = tmp_path / "HD00_512_writer_two_volumes.hds"

    builder = HdsImageBuilder(size_bytes=4 * 1024 * 1024)
    partition = builder.add_partition("hd1")
    volume_a = partition.add_volume("VolA")
    wave_a = volume_a.add_waveform_from_wav(name="WaveA", path=source_a, root_key=60)
    volume_a.add_sample_bank(
        name="BankA",
        waveform=wave_a,
        root_key=60,
        key_low=0,
        key_high=63,
        level=100,
    )
    volume_b = partition.add_volume("VolB")
    wave_b = volume_b.add_waveform_from_wav(name="WaveB", path=source_b, root_key=64)
    volume_b.add_sample_bank(
        name="BankB",
        waveform=wave_b,
        root_key=64,
        key_low=64,
        key_high=127,
        level=90,
    )

    result = builder.write(image_path)

    assert [(item.object_type, item.name, item.sfs_id) for item in result.objects] == [
        ("SMPL", "WaveA", 9),
        ("SBNK", "BankA", 10),
        ("SMPL", "WaveB", 17),
        ("SBNK", "BankB", 18),
    ]
    image_bytes = image_path.read_bytes()
    root_payload = _record_payload(image_bytes, _index_record(image_bytes, 1))
    volume_a_payload = _record_payload(image_bytes, _index_record(image_bytes, 3))
    volume_b_payload = _record_payload(image_bytes, _index_record(image_bytes, 11))
    assert _entry_names(root_payload) == [
        ".",
        "..",
        "sfserrlog",
        "sfserram",
        "VolA            ",
        "VolB            ",
    ]
    assert _entry_names(volume_a_payload) == [".", "..", "SMPL", "SBNK", "SBAC", "SEQU", "PROG"]
    assert _entry_names(volume_b_payload) == [".", "..", "SMPL", "SBNK", "SBAC", "SEQU", "PROG"]

    smpl_a = _record_payload(image_bytes, _index_record(image_bytes, 9))
    sbnk_a = _record_payload(image_bytes, _index_record(image_bytes, 10))
    smpl_b = _record_payload(image_bytes, _index_record(image_bytes, 17))
    sbnk_b = _record_payload(image_bytes, _index_record(image_bytes, 18))
    assert smpl_a[0x74:0x7C] == bytes.fromhex("01443840016b1dbc")
    assert sbnk_a[0xA0:0xA4] == bytes.fromhex("016b1dbc")
    assert smpl_b[0x74:0x7C] == bytes.fromhex("01443840016b1dbc")
    assert sbnk_b[0xA0:0xA4] == bytes.fromhex("016b1dbc")

    objects = load_sfs_objects(image_path)
    graph = build_relationship_graph(objects)
    relationships = [
        row for row in graph.relationships if row.relationship_type == "SBNK_LEFT_MEMBER_TO_SMPL"
    ]
    assert sorted(
        (row.source_key, row.target_key, row.quality, row.basis) for row in relationships
    ) == [
        ("p0:sfs10", "p0:sfs9", "Known", "sbnk-member-link+name"),
        ("p0:sfs18", "p0:sfs17", "Known", "sbnk-member-link+name"),
    ]


def test_hds_writer_creates_two_partitions_with_independent_volumes(tmp_path: Path) -> None:
    source_a = tmp_path / "partition-a.wav"
    source_b = tmp_path / "partition-b.wav"
    _write_mono_wav(source_a, samples=(0, 700, -700, 1400, -1400))
    _write_mono_wav(source_b, samples=(250, -250, 750, -750, 0))
    image_path = tmp_path / "HD00_512_writer_two_partitions.hds"

    builder = HdsImageBuilder(size_bytes=512 * 1024 * 1024)
    partition_a = builder.add_partition("hd1")
    volume_a = partition_a.add_volume("PartA")
    wave_a = volume_a.add_waveform_from_wav(name="WaveA", path=source_a, root_key=60)
    volume_a.add_sample_bank(
        name="BankA",
        waveform=wave_a,
        root_key=60,
        key_low=0,
        key_high=63,
        level=100,
    )
    partition_b = builder.add_partition("hd2")
    volume_b = partition_b.add_volume("PartB")
    wave_b = volume_b.add_waveform_from_wav(name="WaveB", path=source_b, root_key=64)
    volume_b.add_sample_bank(
        name="BankB",
        waveform=wave_b,
        root_key=64,
        key_low=64,
        key_high=127,
        level=90,
    )

    result = builder.write(image_path)

    assert result.partitions == 2
    assert [
        (item.partition_index, item.object_type, item.name, item.sfs_id) for item in result.objects
    ] == [
        (0, "SMPL", "WaveA", 9),
        (0, "SBNK", "BankA", 10),
        (1, "SMPL", "WaveB", 9),
        (1, "SBNK", "BankB", 10),
    ]
    image_bytes = image_path.read_bytes()
    assert _be32(image_bytes, 0xA8) == 3
    assert _be32(image_bytes, 0xAC) == 524_286
    assert _be32(image_bytes, 0xB0) == 524_290
    assert _be32(image_bytes, 0xB4) == 524_286
    assert image_bytes[1024:1032] == b"bb736200"
    assert image_bytes[1024 + 0x09 : 1024 + 0x11] == b"c2b4e600"
    assert image_bytes[1024 + 0x12 : 1024 + 0x2C] == b"\x00" * 0x1A
    assert image_bytes[1024 + 0x30 : 1024 + 0x3F] == b"\x00" * 15
    assert image_bytes[1024 + 0x40 : 1024 + 0x5A] == b"\x00" * 0x1A
    pre_partition_b = (524_290 - 1) * 512
    assert image_bytes[pre_partition_b : pre_partition_b + 8] == b"bb736201"
    assert image_bytes[pre_partition_b + 0x09 : pre_partition_b + 0x11] == b"c2b4e600"
    assert image_bytes[pre_partition_b + 0x12 : pre_partition_b + 0x2C] == b"\x00" * 0x1A
    assert image_bytes[pre_partition_b + 0x30 : pre_partition_b + 0x3F] == b"\x00" * 15
    assert image_bytes[pre_partition_b + 0x40 : pre_partition_b + 0x5A] == b"\x00" * 0x1A

    partition_a_offset = _be32(image_bytes, 0xA8) * 512
    partition_b_offset = _be32(image_bytes, 0xB0) * 512
    assert image_bytes[partition_a_offset + 0x100 : partition_a_offset + 0x108] == bytes.fromhex(
        "0000000000000003"
    )
    assert image_bytes[partition_b_offset + 0x40 : partition_b_offset + 0x50] == b"hd2            1"
    assert image_bytes[partition_b_offset + 0xAF] == 1
    assert image_bytes[partition_b_offset + 0x100 : partition_b_offset + 0x108] == bytes.fromhex(
        "0000000000080002"
    )
    _assert_partition_header_residue_zero(image_bytes, 0)
    _assert_partition_header_residue_zero(image_bytes, 1)

    root_a = _record_payload_in_partition(
        image_bytes, 0, _index_record_in_partition(image_bytes, 0, 1)
    )
    root_b = _record_payload_in_partition(
        image_bytes, 1, _index_record_in_partition(image_bytes, 1, 1)
    )
    volume_a_payload = _record_payload_in_partition(
        image_bytes, 0, _index_record_in_partition(image_bytes, 0, 3)
    )
    volume_b_payload = _record_payload_in_partition(
        image_bytes, 1, _index_record_in_partition(image_bytes, 1, 3)
    )
    assert _entry_names(root_a) == [".", "..", "sfserrlog", "sfserram", "PartA           "]
    assert _entry_names(root_b) == [".", "..", "sfserrlog", "sfserram", "PartB           "]
    assert _entry_names(volume_a_payload) == [".", "..", "SMPL", "SBNK", "SBAC", "SEQU", "PROG"]
    assert _entry_names(volume_b_payload) == [".", "..", "SMPL", "SBNK", "SBAC", "SEQU", "PROG"]

    objects = load_sfs_objects(image_path)
    graph = build_relationship_graph(objects)
    relationships = [
        row for row in graph.relationships if row.relationship_type == "SBNK_LEFT_MEMBER_TO_SMPL"
    ]
    assert sorted(
        (row.source_key, row.target_key, row.quality, row.basis) for row in relationships
    ) == [
        ("p0:sfs10", "p0:sfs9", "Known", "sbnk-member-link+name"),
        ("p1:sfs10", "p1:sfs9", "Known", "sbnk-member-link+name"),
    ]


def test_hds_writer_scales_bitmap_and_index_geometry_for_256_mib(tmp_path: Path) -> None:
    source_wav = tmp_path / "tone.wav"
    _write_mono_wav(source_wav)
    image_path = tmp_path / "HD00_512_writer_256m.hds"

    builder = HdsImageBuilder(size_bytes=256 * 1024 * 1024)
    partition = builder.add_partition("hd1")
    volume = partition.add_volume("WriterVol")
    waveform = volume.add_waveform_from_wav(name="Tone0001", path=source_wav, root_key=60)
    volume.add_sample_bank(
        name="Bank0001",
        waveform=waveform,
        root_key=60,
        key_low=0,
        key_high=127,
    )

    builder.write(image_path)

    image_bytes = image_path.read_bytes()
    assert image_bytes[0x80:0x9C] == bytes.fromhex(
        "a1 e0 01 52 a2 2c 00 00 00 00 00 17 09 10 00 00 00 00 00 17 09 10 00 00 01 00 01 52"
    )
    assert image_bytes[512 + 0x80 : 512 + 0x9C] == image_bytes[0x80:0x9C]
    assert image_bytes[1024:1032] == b"c2b4e600"
    assert image_bytes[1033:1041] == b"ab432100"
    assert image_bytes[1042:1068] == b"\x00" * 0x1A
    partition_start = _be32(image_bytes, 0xA8)
    partition_offset = partition_start * 512
    assert _be32(image_bytes, partition_offset + 0x9C) == 34
    assert _be32(image_bytes, partition_offset + 0xA4) == 66
    assert _be32(image_bytes, partition_offset + 0xA8) == 358
    assert image_bytes[partition_offset + 0x100 : partition_offset + 0x108] == bytes.fromhex(
        "0000000000000003"
    )
    assert image_bytes[partition_offset + 0x11C : partition_offset + 0x120] == (524_285).to_bytes(
        4, "big"
    )
    _assert_partition_header_residue_zero(image_bytes, 0)
    assert (
        image_bytes[partition_offset : partition_offset + 1024]
        == image_bytes[partition_offset + 1024 : partition_offset + 2048]
    )
    assert image_bytes[7 * 512 : 8 * 512] == image_bytes[71 * 512 : 72 * 512]
    assert any(image_bytes[7 * 512 : 8 * 512])
    hidden_record = _index_record(image_bytes, 0)
    root_record = _index_record(image_bytes, 1)
    assert _be32(hidden_record, 0x0A) == 424
    assert _be32(root_record, 0x0A) == 456


def test_hds_writer_creates_sparse_768m_three_partition_empty_volume_image(tmp_path: Path) -> None:
    image_path = tmp_path / "HD00_512_sparse_768m_3p_empty_volumes.hds"

    builder = HdsImageBuilder(size_bytes=768 * 1024 * 1024)
    for _index in range(3):
        partition = builder.add_partition("New Partition")
        partition.add_volume("New Volume")

    result = builder.write(image_path)

    assert result.partitions == 3
    assert result.objects == ()
    image_bytes = image_path.read_bytes()
    assert len(image_bytes) == 768 * 1024 * 1024
    assert [_be32(image_bytes, 0xA8 + index * 8) for index in range(3)] == [
        3,
        524_290,
        1_048_577,
    ]
    assert [_be32(image_bytes, 0xAC + index * 8) for index in range(3)] == [
        524_286,
        524_286,
        524_286,
    ]
    for index, sector in enumerate([2, 524_289, 1_048_576]):
        assert image_bytes[sector * 512 : sector * 512 + 8] == f"ab43210{index}".encode("ascii")
        assert image_bytes[sector * 512 + 8 : sector * 512 + 512] == b"\x00" * 504

    for index in range(3):
        partition_offset = _be32(image_bytes, 0xA8 + index * 8) * 512
        expected_name = "New Partition" if index == 0 else f"New Partition  {index}"
        assert image_bytes[
            partition_offset + 0x40 : partition_offset + 0x50
        ] == expected_name.encode("ascii").ljust(16)
        assert image_bytes[partition_offset + 0xAF] == 0
        assert _be32(image_bytes, partition_offset + 0x104) == [3, 524_290, 1_048_577][index]
        assert _be32(image_bytes, partition_offset + 0x154) == index
        assert _be32(image_bytes, partition_offset + 0x160) == index * 524_286
        assert _be32(image_bytes, partition_offset + 0x1A8) == index
        _assert_partition_header_residue_zero(image_bytes, index)

        root_payload = _record_payload_in_partition(
            image_bytes, index, _index_record_in_partition(image_bytes, index, 1)
        )
        volume_payload = _record_payload_in_partition(
            image_bytes, index, _index_record_in_partition(image_bytes, index, 3)
        )
        assert _entry_names(root_payload) == [
            ".",
            "..",
            "sfserrlog",
            "sfserram",
            "New Volume      ",
        ]
        assert _entry_names(volume_payload) == [
            ".",
            "..",
            "SMPL",
            "SBNK",
            "SBAC",
            "SEQU",
            "PROG",
        ]


def test_hds_writer_creates_sparse_768m_object_volume_on_first_partition(
    tmp_path: Path,
) -> None:
    source_wav = tmp_path / "tone.wav"
    _write_mono_wav(source_wav)
    image_path = tmp_path / "HD00_512_sparse_768m_3p_p0_object.hds"

    builder = HdsImageBuilder(size_bytes=768 * 1024 * 1024)
    partition_a = builder.add_partition("New Partition")
    volume_a = partition_a.add_volume("New Volume")
    waveform = volume_a.add_waveform_from_wav(
        name="Tone0001", path=source_wav, root_key=60
    )
    volume_a.add_sample_bank(
        name="Bank0001",
        waveform=waveform,
        root_key=60,
        key_low=60,
        key_high=60,
        level=100,
    )
    for _index in range(2):
        partition = builder.add_partition("New Partition")
        partition.add_volume("New Volume")

    result = builder.write(image_path)

    assert result.partitions == 3
    assert [
        (obj.partition_index, obj.sfs_id, obj.object_type, obj.name)
        for obj in result.objects
    ] == [
        (0, 9, "SMPL", "Tone0001"),
        (0, 10, "SBNK", "Bank0001"),
    ]
    image_bytes = image_path.read_bytes()
    assert len(image_bytes) == 768 * 1024 * 1024
    assert [_be32(image_bytes, 0xA8 + index * 8) for index in range(3)] == [
        3,
        524_290,
        1_048_577,
    ]

    root_a = _record_payload_in_partition(
        image_bytes, 0, _index_record_in_partition(image_bytes, 0, 1)
    )
    volume_a_payload = _record_payload_in_partition(
        image_bytes, 0, _index_record_in_partition(image_bytes, 0, 3)
    )
    smpl_category = _record_payload_in_partition(
        image_bytes, 0, _index_record_in_partition(image_bytes, 0, 4)
    )
    sbnk_category = _record_payload_in_partition(
        image_bytes, 0, _index_record_in_partition(image_bytes, 0, 5)
    )
    smpl_payload = _record_payload_in_partition(
        image_bytes, 0, _index_record_in_partition(image_bytes, 0, 9)
    )
    sbnk_payload = _record_payload_in_partition(
        image_bytes, 0, _index_record_in_partition(image_bytes, 0, 10)
    )
    assert _entry_names(root_a) == [
        ".",
        "..",
        "sfserrlog",
        "sfserram",
        "New Volume      ",
    ]
    assert _entry_names(volume_a_payload) == [
        ".",
        "..",
        "SMPL",
        "SBNK",
        "SBAC",
        "SEQU",
        "PROG",
    ]
    assert _entry_names(smpl_category) == [".", "..", "Tone0001        "]
    assert _entry_names(sbnk_category) == [".", "..", "Bank0001        "]
    assert smpl_payload[0x0C:0x10] == b"SMPL"
    assert sbnk_payload[0x0C:0x10] == b"SBNK"
    assert sbnk_payload[0x0D6] == 60
    assert sbnk_payload[0x0E2:0x0E4] == bytes([60, 60])
    assert sbnk_payload[0x116] == 100

    for index in (1, 2):
        root_payload = _record_payload_in_partition(
            image_bytes, index, _index_record_in_partition(image_bytes, index, 1)
        )
        volume_payload = _record_payload_in_partition(
            image_bytes, index, _index_record_in_partition(image_bytes, index, 3)
        )
        smpl_payload = _record_payload_in_partition(
            image_bytes, index, _index_record_in_partition(image_bytes, index, 4)
        )
        sbnk_payload = _record_payload_in_partition(
            image_bytes, index, _index_record_in_partition(image_bytes, index, 5)
        )
        assert _entry_names(root_payload) == [
            ".",
            "..",
            "sfserrlog",
            "sfserram",
            "New Volume      ",
        ]
        assert _entry_names(volume_payload) == [
            ".",
            "..",
            "SMPL",
            "SBNK",
            "SBAC",
            "SEQU",
            "PROG",
        ]
        assert _entry_names(smpl_payload) == [".", ".."]
        assert _entry_names(sbnk_payload) == [".", ".."]

def test_hds_writer_creates_sparse_768m_object_volume_on_second_partition(
    tmp_path: Path,
) -> None:
    source_wav = tmp_path / "tone.wav"
    _write_mono_wav(source_wav)
    image_path = tmp_path / "HD00_512_sparse_768m_3p_p1_object.hds"

    builder = HdsImageBuilder(size_bytes=768 * 1024 * 1024)
    partition_a = builder.add_partition("New Partition")
    partition_a.add_volume("New Volume")
    partition_b = builder.add_partition("New Partition")
    volume_b = partition_b.add_volume("New Volume")
    waveform = volume_b.add_waveform_from_wav(
        name="Tone0001", path=source_wav, root_key=60
    )
    volume_b.add_sample_bank(
        name="Bank0001",
        waveform=waveform,
        root_key=60,
        key_low=60,
        key_high=60,
        level=100,
    )
    partition_c = builder.add_partition("New Partition")
    partition_c.add_volume("New Volume")

    result = builder.write(image_path)

    assert [
        (obj.partition_index, obj.sfs_id, obj.object_type, obj.name)
        for obj in result.objects
    ] == [
        (1, 9, "SMPL", "Tone0001"),
        (1, 10, "SBNK", "Bank0001"),
    ]
    image_bytes = image_path.read_bytes()
    assert [_be32(image_bytes, 0xA8 + index * 8) for index in range(3)] == [
        3,
        524_290,
        1_048_577,
    ]

    for index, expected_smpl, expected_sbnk in (
        (0, [".", ".."], [".", ".."]),
        (1, [".", "..", "Tone0001        "], [".", "..", "Bank0001        "]),
        (2, [".", ".."], [".", ".."]),
    ):
        smpl_category = _record_payload_in_partition(
            image_bytes, index, _index_record_in_partition(image_bytes, index, 4)
        )
        sbnk_category = _record_payload_in_partition(
            image_bytes, index, _index_record_in_partition(image_bytes, index, 5)
        )
        assert _entry_names(smpl_category) == expected_smpl
        assert _entry_names(sbnk_category) == expected_sbnk

    smpl_payload = _record_payload_in_partition(
        image_bytes, 1, _index_record_in_partition(image_bytes, 1, 9)
    )
    sbnk_payload = _record_payload_in_partition(
        image_bytes, 1, _index_record_in_partition(image_bytes, 1, 10)
    )
    assert smpl_payload[0x0C:0x10] == b"SMPL"
    assert sbnk_payload[0x0C:0x10] == b"SBNK"
    assert sbnk_payload[0x0D6] == 60
    assert sbnk_payload[0x0E2:0x0E4] == bytes([60, 60])
    assert sbnk_payload[0x116] == 100

def test_hds_writer_creates_sparse_768m_object_volume_on_third_partition(
    tmp_path: Path,
) -> None:
    source_wav = tmp_path / "tone.wav"
    _write_mono_wav(source_wav)
    image_path = tmp_path / "HD00_512_sparse_768m_3p_p2_object.hds"

    builder = HdsImageBuilder(size_bytes=768 * 1024 * 1024)
    partition_a = builder.add_partition("New Partition")
    partition_a.add_volume("New Volume")
    partition_b = builder.add_partition("New Partition")
    partition_b.add_volume("New Volume")
    partition_c = builder.add_partition("New Partition")
    volume_c = partition_c.add_volume("New Volume")
    waveform = volume_c.add_waveform_from_wav(
        name="Tone0001", path=source_wav, root_key=60
    )
    volume_c.add_sample_bank(
        name="Bank0001",
        waveform=waveform,
        root_key=60,
        key_low=60,
        key_high=60,
        level=100,
    )

    result = builder.write(image_path)

    assert [
        (obj.partition_index, obj.sfs_id, obj.object_type, obj.name)
        for obj in result.objects
    ] == [
        (2, 9, "SMPL", "Tone0001"),
        (2, 10, "SBNK", "Bank0001"),
    ]
    image_bytes = image_path.read_bytes()
    assert [_be32(image_bytes, 0xA8 + index * 8) for index in range(3)] == [
        3,
        524_290,
        1_048_577,
    ]

    for index, expected_smpl, expected_sbnk in (
        (0, [".", ".."], [".", ".."]),
        (1, [".", ".."], [".", ".."]),
        (2, [".", "..", "Tone0001        "], [".", "..", "Bank0001        "]),
    ):
        smpl_category = _record_payload_in_partition(
            image_bytes, index, _index_record_in_partition(image_bytes, index, 4)
        )
        sbnk_category = _record_payload_in_partition(
            image_bytes, index, _index_record_in_partition(image_bytes, index, 5)
        )
        assert _entry_names(smpl_category) == expected_smpl
        assert _entry_names(sbnk_category) == expected_sbnk

    smpl_payload = _record_payload_in_partition(
        image_bytes, 2, _index_record_in_partition(image_bytes, 2, 9)
    )
    sbnk_payload = _record_payload_in_partition(
        image_bytes, 2, _index_record_in_partition(image_bytes, 2, 10)
    )
    assert smpl_payload[0x0C:0x10] == b"SMPL"
    assert sbnk_payload[0x0C:0x10] == b"SBNK"
    assert sbnk_payload[0x0D6] == 60
    assert sbnk_payload[0x0E2:0x0E4] == bytes([60, 60])
    assert sbnk_payload[0x116] == 100
