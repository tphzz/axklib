from __future__ import annotations

import wave
from pathlib import Path

import pytest

from axklib.audio import decode_waveform
from axklib.containers import load_sfs_objects, sfs_allocation
from axklib.model import AxklibObjectType
from axklib.parameters import sbnk_contract
from axklib.relationships import build_relationship_graph
from axklib.write import MAX_IMAGE_SIZE_BYTES, HdsImageBuilder


def _write_mono_wav(
    path: Path,
    samples: tuple[int, ...] = (0, 1000, -1000, 32767, -32768),
    sample_rate: int = 44_100,
) -> bytes:
    pcm = b"".join(sample.to_bytes(2, "little", signed=True) for sample in samples)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(pcm)
    return pcm


def _write_stereo_wav(
    path: Path,
    left_samples: tuple[int, ...],
    right_samples: tuple[int, ...],
    sample_rate: int = 44_100,
) -> tuple[bytes, bytes]:
    left_pcm = b"".join(sample.to_bytes(2, "little", signed=True) for sample in left_samples)
    right_pcm = b"".join(sample.to_bytes(2, "little", signed=True) for sample in right_samples)
    interleaved = b"".join(
        left_pcm[offset : offset + 2] + right_pcm[offset : offset + 2]
        for offset in range(0, len(left_pcm), 2)
    )
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(interleaved)
    return left_pcm, right_pcm


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
    assert data[partition_offset + 1024 + 0x1BC : partition_offset + 1024 + 0x1E4] == b"\x00" * 40


RETIRED_FIXED_TAIL_BYTE_OFFSETS = (
    0x10A,
    0x10B,
    0x10E,
    0x113,
    0x122,
    0x125,
    0x126,
    0x129,
    0x12A,
    0x12B,
    0x12C,
    0x12D,
    0x12E,
    0x12F,
    0x131,
    0x132,
    0x133,
    0x13B,
    0x13F,
    0x142,
    0x143,
    0x150,
    0x151,
    0x152,
    0x153,
    0x16B,
    0x16E,
    0x171,
    0x172,
    0x173,
    0x174,
    0x175,
    0x176,
    0x177,
    0x181,
    0x182,
    0x183,
    0x18A,
    0x18B,
    0x18E,
    0x1A3,
    0x1B5,
    0x1B6,
    0x1B7,
    0x1B8,
    0x1B9,
    0x1BA,
    0x1BB,
    0x1E4,
    0x1E5,
    0x1E6,
    0x1E7,
    0x1EB,
    0x1EF,
    0x1F2,
)


def _assert_partition_header_fixed_tail_zero(data: bytes, partition_index: int) -> None:
    partition_start = _be32(data, 0xA8 + partition_index * 8)
    partition_offset = partition_start * 512
    for header_base in (partition_offset, partition_offset + 1024):
        assert all(data[header_base + offset] == 0 for offset in RETIRED_FIXED_TAIL_BYTE_OFFSETS)


def test_hds_writer_rejects_images_above_a_series_cap() -> None:
    with pytest.raises(ValueError, match="exceeds A-series cap"):
        HdsImageBuilder(size_bytes=MAX_IMAGE_SIZE_BYTES + 512)


def test_hds_writer_caps_single_partition_in_two_gib_image() -> None:
    builder = HdsImageBuilder(size_bytes=MAX_IMAGE_SIZE_BYTES)
    builder.add_partition("hd1")

    [plan] = builder.plan()

    assert plan.start_sector == 3
    assert plan.sector_count == 0x1FFFFE
    assert MAX_IMAGE_SIZE_BYTES // 512 - (plan.start_sector + plan.sector_count) == 0x1FFFFF


@pytest.mark.parametrize("partition_count", range(1, 9))
def test_hds_writer_supports_every_partition_count_at_two_gib_cap(
    partition_count: int,
) -> None:
    builder = HdsImageBuilder(size_bytes=MAX_IMAGE_SIZE_BYTES)
    for index in range(partition_count):
        builder.add_partition(f"hd{index + 1}")

    plans = builder.plan()
    raw_slot_span = (MAX_IMAGE_SIZE_BYTES // 512 - 2) // partition_count
    slot_span = min(raw_slot_span, 0x1FFFFF)

    assert [plan.start_sector for plan in plans] == [
        3 + index * slot_span for index in range(partition_count)
    ]
    assert [plan.sector_count for plan in plans] == [slot_span - 1] * partition_count


@pytest.mark.parametrize("partition_count", range(1, 9))
def test_hds_writer_uses_equal_partition_slots(
    partition_count: int,
) -> None:
    size_bytes = partition_count * 256 * 1024 * 1024
    builder = HdsImageBuilder(size_bytes=size_bytes)
    for index in range(partition_count):
        builder.add_partition(f"hd{index + 1}")

    plans = builder.plan()
    slot_span = (size_bytes // 512 - 2) // partition_count

    assert [plan.start_sector for plan in plans] == [
        3 + index * slot_span for index in range(partition_count)
    ]
    assert [plan.sector_count for plan in plans] == [slot_span - 1] * partition_count


def test_hds_writer_preserves_division_remainder() -> None:
    size_bytes = 1280 * 1024 * 1024 + 512
    builder = HdsImageBuilder(size_bytes=size_bytes)
    for index in range(5):
        builder.add_partition(f"hd{index + 1}")

    plans = builder.plan()
    total_sectors = size_bytes // 512
    slot_span = (total_sectors - 2) // 5

    assert [plan.start_sector for plan in plans] == [3 + index * slot_span for index in range(5)]
    assert [plan.sector_count for plan in plans] == [slot_span - 1] * 5
    assert total_sectors - (plans[-1].start_sector + plans[-1].sector_count) == 4


@pytest.mark.parametrize("partition_count", range(1, 9))
def test_hds_writer_accepts_exact_minimum_size_for_partition_count(
    partition_count: int,
) -> None:
    minimum_total_sectors = 2 + partition_count * 2046
    builder = HdsImageBuilder(size_bytes=minimum_total_sectors * 512)
    for index in range(partition_count):
        builder.add_partition(f"hd{index + 1}")

    plans = builder.plan()

    assert [plan.sector_count for plan in plans] == [2045] * partition_count


@pytest.mark.parametrize("partition_count", range(1, 9))
def test_hds_writer_rejects_below_minimum_size_for_partition_count(
    partition_count: int,
) -> None:
    minimum_total_sectors = 2 + partition_count * 2046
    with pytest.raises(ValueError, match="image size must|partition slots are too small"):
        builder = HdsImageBuilder(size_bytes=(minimum_total_sectors - 1) * 512)
        for index in range(partition_count):
            builder.add_partition(f"hd{index + 1}")
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
    layout = result.partition_layouts[0]
    assert layout.first_payload_cluster == 362
    assert layout.allocated_cluster_count == 50
    assert layout.free_cluster_count == layout.cluster_count - 362 - 50
    assert layout.free_bytes == layout.free_cluster_count * 1024
    assert layout.sampler_visible_free_kib == layout.free_cluster_count
    allocation_summary = sfs_allocation.analyze_image(image_path).summaries[0]
    assert allocation_summary.first_payload_cluster == layout.first_payload_cluster
    assert allocation_summary.stored_used_cluster_count == layout.allocated_cluster_count
    assert allocation_summary.sampler_free_cluster_count == layout.free_cluster_count
    assert allocation_summary.sampler_visible_free_kib == layout.sampler_visible_free_kib
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
    assert sbnk_payload[0x088:0x098] == b"\x00" * 16
    assert _be32(sbnk_payload, 0x0A4) == 0
    assert sbnk_payload[0x0D7] == 0
    assert sbnk_payload[0x0DA:0x0DC] == b"\x00\x00"
    assert sbnk_payload[0x0DD] == 0
    assert sbnk_payload[0x0E0:0x0E2] == b"\x00\x00"
    assert sbnk_payload[0x0EC:0x0F0] == b"\x00" * 4
    assert _be32(sbnk_payload, 0x0F4) == 0
    assert _be32(sbnk_payload, 0x0FC) == 0
    assert _be32(sbnk_payload, 0x104) == 0
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


def test_hds_writer_creates_hardware_profile_sbac_and_prog(tmp_path: Path) -> None:
    source_wav = tmp_path / "tone.wav"
    _write_mono_wav(source_wav, sample_rate=48_000)
    image_path = tmp_path / "HD00_512_writer_sbac_prog.hds"

    builder = HdsImageBuilder(size_bytes=1024 * 1024)
    volume = builder.add_partition("New Partition").add_volume("New Volume")
    waveform = volume.add_waveform_from_wav(name="pulse 1", path=source_wav, root_key=66)
    grouped_bank = volume.add_sample_bank(
        name="JS01",
        waveform=waveform,
        root_key=66,
        key_low=0,
        key_high=127,
        level=100,
    )
    direct_bank = volume.add_sample_bank(
        name="JS02 *",
        waveform=waveform,
        root_key=66,
        key_low=0,
        key_high=127,
        level=100,
    )
    group = volume.add_sample_bank_group(name="AUDSB", member=grouped_bank)
    program = volume.add_program(number=1)
    program.assign_sample_bank_group(group, receive_channel=1)
    program.assign_sample_bank(direct_bank, receive_channel=2)

    result = builder.write(image_path)

    assert [(item.object_type, item.name) for item in result.objects] == [
        ("SMPL", "pulse 1"),
        ("SBNK", "JS01"),
        ("SBNK", "JS02 *"),
        ("SBAC", "AUDSB"),
        ("PROG", "001"),
    ]
    data = image_path.read_bytes()
    sbnk_grouped = _record_payload(data, _index_record(data, 10))
    sbnk_direct = _record_payload(data, _index_record(data, 11))
    sbac = _record_payload(data, _index_record(data, 12))
    prog = _record_payload(data, _index_record(data, 13))

    assert sbnk_grouped[0x0D0] == 0x03
    assert _be32(sbnk_grouped, 0x0C0) == 0
    assert sbnk_direct[0x0D0] == 0x02
    assert _be32(sbnk_direct, 0x0C0) == 1

    assert len(sbac) == 0x210
    assert sbac[0x0C:0x10] == b"SBAC"
    assert sbac[0x32:0x42] == b"AUDSB           "
    assert sbac[0x42:0x144] == b"\x00" * (0x144 - 0x42)
    assert sbac[0x144] == 1
    assert sbac[0x14C:0x15C] == b"JS01            "
    assert sbac[0x15C:0x210] == b"\x00" * (0x210 - 0x15C)

    assert len(prog) == 0x390
    assert prog[0x0C:0x10] == b"PROG"
    assert prog[0x32:0x42] == b"001             "
    assert prog[0x42:0x78] == b"\x00" * (0x78 - 0x42)
    assert prog[0x78:0x80] == b"Pgm 001 "
    assert prog[0x8B] == 127
    assert prog[0x8E] == 0
    assert prog[0x98:0x120] == b"\x00" * (0x120 - 0x98)
    assert prog[0x120:0x130] == b"AUDSB           "
    assert _be32(prog, 0x130) == 0
    assert prog[0x134:0x136] == b"\x11\x00"
    assert prog[0x158:0x168] == b"JS02 *          "
    assert _be32(prog, 0x168) == 0
    assert prog[0x16C:0x16E] == b"\x10\x01"
    assert prog[0x190:] == b"\x00" * (0x390 - 0x190)

    objects = load_sfs_objects(image_path)
    graph = build_relationship_graph(objects)
    relation_types = [row.relationship_type for row in graph.relationships]
    assert relation_types.count("SBNK_LEFT_MEMBER_TO_SMPL") == 2
    assert relation_types.count("SBAC_SLOT_TO_SBNK") == 1
    assert relation_types.count("PROG_ASSIGNMENT_TO_SBAC") == 1
    assert relation_types.count("PROG_ASSIGNMENT_TO_SBNK") == 1
    assert relation_types.count("SBNK_PROGRAM_BITMAP_TO_PROG") == 1
    assert all(row.quality == "Known" for row in graph.relationships)
    assert not graph.ambiguous()


def test_hds_writer_creates_three_member_sbac_and_prog(tmp_path: Path) -> None:
    source_wav = tmp_path / "tone.wav"
    _write_mono_wav(source_wav)
    image_path = tmp_path / "HD00_512_writer_three_member_sbac.hds"

    builder = HdsImageBuilder(size_bytes=1024 * 1024)
    volume = builder.add_partition("New Partition").add_volume("Key Group Vol")
    waveform = volume.add_waveform_from_wav(name="Tone", path=source_wav, root_key=60)
    members = tuple(
        volume.add_sample_bank(
            name=f"Member {note}",
            waveform=waveform,
            root_key=root_key,
            key_low=root_key,
            key_high=root_key,
            level=100,
        )
        for note, root_key in (("C3", 60), ("D3", 62), ("E3", 64))
    )
    direct = volume.add_sample_bank(
        name="Direct G3",
        waveform=waveform,
        root_key=67,
        key_low=67,
        key_high=67,
        level=100,
    )
    group = volume.add_sample_bank_group(name="Key Group", members=members)
    program = volume.add_program(number=1)
    program.assign_sample_bank_group(group, receive_channel=1)
    program.assign_sample_bank(direct, receive_channel=2)

    builder.write(image_path)

    objects = load_sfs_objects(image_path)
    sbac = next(item.payload for item in objects if item.object_type == AxklibObjectType.SBAC)
    assert sbac[0x144] == 3
    for index, name in enumerate(("Member C3", "Member D3", "Member E3")):
        row_offset = 0x14C + index * 0x14
        assert sbac[row_offset : row_offset + 0x10].rstrip(b" ") == name.encode("ascii")
        assert _be32(sbac, row_offset + 0x10) == 0

    banks = {
        item.name: item.payload for item in objects if item.object_type == AxklibObjectType.SBNK
    }
    assert all(banks[name][0x0D0] == 0x03 for name in ("Member C3", "Member D3", "Member E3"))
    assert banks["Direct G3"][0x0D0] == 0x02
    assert _be32(banks["Direct G3"], 0x0C0) == 1

    graph = build_relationship_graph(objects)
    relation_types = [row.relationship_type for row in graph.relationships]
    assert relation_types.count("SBNK_LEFT_MEMBER_TO_SMPL") == 4
    assert relation_types.count("SBAC_SLOT_TO_SBNK") == 3
    assert relation_types.count("PROG_ASSIGNMENT_TO_SBAC") == 1
    assert relation_types.count("PROG_ASSIGNMENT_TO_SBNK") == 1
    assert relation_types.count("SBNK_PROGRAM_BITMAP_TO_PROG") == 1
    assert all(row.quality == "Known" for row in graph.relationships)
    assert not graph.ambiguous()


def test_hds_writer_allows_general_geometry_for_proven_sbac_prog_topology(
    tmp_path: Path,
) -> None:
    source_wav = tmp_path / "tone.wav"
    _write_mono_wav(source_wav)
    builder = HdsImageBuilder(size_bytes=4 * 1024 * 1024)
    volume = builder.add_partition("hd1").add_volume("WriterVol")
    waveform = volume.add_waveform_from_wav(name="Tone", path=source_wav, root_key=60)
    grouped = volume.add_sample_bank(
        name="Grouped",
        waveform=waveform,
        root_key=60,
        key_low=0,
        key_high=127,
    )
    direct = volume.add_sample_bank(
        name="Direct",
        waveform=waveform,
        root_key=60,
        key_low=0,
        key_high=127,
    )
    group = volume.add_sample_bank_group(name="Group", member=grouped)
    program = volume.add_program()
    program.assign_sample_bank_group(group, receive_channel=1)
    program.assign_sample_bank(direct, receive_channel=2)

    result = builder.write(tmp_path / "HD00_512_writer_sbac_prog_4m.hds")

    assert result.size_bytes == 4 * 1024 * 1024
    assert [(item.object_type, item.name) for item in result.objects][-2:] == [
        ("SBAC", "Group"),
        ("PROG", "001"),
    ]


def test_hds_writer_allows_multiple_sbac_prog_volumes_in_one_partition(
    tmp_path: Path,
) -> None:
    source_wav = tmp_path / "tone.wav"
    _write_mono_wav(source_wav)
    builder = HdsImageBuilder(size_bytes=4 * 1024 * 1024)
    partition = builder.add_partition("hd1")

    for suffix in ("A", "B"):
        volume = partition.add_volume(f"Writer {suffix}")
        waveform = volume.add_waveform_from_wav(name=f"Tone {suffix}", path=source_wav, root_key=60)
        grouped = volume.add_sample_bank(
            name=f"Grouped {suffix}",
            waveform=waveform,
            root_key=60,
            key_low=0,
            key_high=127,
        )
        direct = volume.add_sample_bank(
            name=f"Direct {suffix}",
            waveform=waveform,
            root_key=60,
            key_low=0,
            key_high=127,
        )
        group = volume.add_sample_bank_group(name=f"Group {suffix}", member=grouped)
        program = volume.add_program()
        program.assign_sample_bank_group(group, receive_channel=1)
        program.assign_sample_bank(direct, receive_channel=2)

    result = builder.write(tmp_path / "HD00_512_writer_sbac_prog_two_volumes.hds")

    assert [(item.object_type, item.name) for item in result.objects].count(("PROG", "001")) == 2
    objects = load_sfs_objects(result.path)
    graph = build_relationship_graph(objects)
    assert (
        sum(row.relationship_type == "PROG_ASSIGNMENT_TO_SBAC" for row in graph.relationships) == 2
    )
    assert (
        sum(row.relationship_type == "PROG_ASSIGNMENT_TO_SBNK" for row in graph.relationships) == 2
    )
    assert all(row.quality == "Known" for row in graph.relationships)


def test_hds_writer_allows_multiple_groups_and_program_bitmap_boundaries(
    tmp_path: Path,
) -> None:
    source_wav = tmp_path / "tone.wav"
    _write_mono_wav(source_wav)
    builder = HdsImageBuilder(size_bytes=4 * 1024 * 1024)
    volume = builder.add_partition("hd1").add_volume("Multi Program")
    waveform = volume.add_waveform_from_wav(name="Tone", path=source_wav, root_key=60)

    expected_words = {
        1: (1, 0, 0, 0),
        32: (0x80000000, 0, 0, 0),
        33: (0, 1, 0, 0),
        65: (0, 0, 1, 0),
        97: (0, 0, 0, 1),
    }
    for index, number in enumerate(expected_words):
        root_key = 60 + index
        grouped = volume.add_sample_bank(
            name=f"Member {number:03d}",
            waveform=waveform,
            root_key=root_key,
            key_low=root_key,
            key_high=root_key,
        )
        direct = volume.add_sample_bank(
            name=f"Direct {number:03d}",
            waveform=waveform,
            root_key=root_key,
            key_low=root_key,
            key_high=root_key,
        )
        group = volume.add_sample_bank_group(name=f"Group {number:03d}", member=grouped)
        program = volume.add_program(number=number)
        program.assign_sample_bank_group(group, receive_channel=1)
        program.assign_sample_bank(direct, receive_channel=2)

    result = builder.write(tmp_path / "HD00_512_writer_multi_program.hds")
    objects = load_sfs_objects(result.path)
    graph = build_relationship_graph(objects)

    assert sum(item.object_type == AxklibObjectType.PROG for item in objects) == 5
    assert sum(item.object_type == AxklibObjectType.SBAC for item in objects) == 5
    assert all(row.quality == "Known" for row in graph.relationships)
    for number, words in expected_words.items():
        direct = next(
            item
            for item in objects
            if item.object_type == AxklibObjectType.SBNK and item.name == f"Direct {number:03d}"
        )
        assert tuple(_be32(direct.payload, 0x0C0 + index * 4) for index in range(4)) == words


def test_hds_writer_can_zero_single_member_inactive_right_lane(tmp_path: Path) -> None:
    source_wav = tmp_path / "tone.wav"
    _write_mono_wav(source_wav)
    image_path = tmp_path / "HD00_512_writer_zero_inactive_right.hds"

    builder = HdsImageBuilder(
        size_bytes=4 * 1024 * 1024,
        _sbnk_single_member_inactive_right_policy=sbnk_contract.CURRENT_SBNK_SINGLE_MEMBER_INACTIVE_RIGHT_POLICY_ZERO,
    )
    partition = builder.add_partition("hd1")
    volume = partition.add_volume("WriterVol")
    waveform = volume.add_waveform_from_wav(name="Tone0001", path=source_wav, root_key=60)
    volume.add_sample_bank(
        name="Bank0001",
        waveform=waveform,
        root_key=60,
        key_low=60,
        key_high=60,
        level=100,
    )

    builder.write(image_path)

    image_bytes = image_path.read_bytes()
    sbnk_record = _index_record(image_bytes, 10)
    sbnk_payload = _record_payload(image_bytes, sbnk_record)
    assert sbnk_payload[0x078:0x088] == b"Tone0001        "
    assert sbnk_payload[0x088:0x098] == b"\x00" * 16
    assert _be32(sbnk_payload, 0x0A0) == 0x016B1DBC
    assert _be32(sbnk_payload, 0x0A4) == 0
    assert sbnk_payload[0x0D6] == 60
    assert sbnk_payload[0x0D7] == 0
    assert sbnk_payload[0x0D8:0x0DC] == b"\xac\x44\x00\x00"
    assert sbnk_payload[0x0DD] == 0
    assert sbnk_payload[0x0E0:0x0E2] == b"\x00\x00"
    assert sbnk_payload[0x0EC:0x0F0] == b"\x00" * 4
    assert _be32(sbnk_payload, 0x0F0) == 5
    assert _be32(sbnk_payload, 0x0F4) == 0
    assert _be32(sbnk_payload, 0x0F8) == 0
    assert _be32(sbnk_payload, 0x0FC) == 0
    assert _be32(sbnk_payload, 0x100) == 5
    assert _be32(sbnk_payload, 0x104) == 0

    objects = load_sfs_objects(image_path)
    graph = build_relationship_graph(objects)
    relationships = [
        row for row in graph.relationships if row.relationship_type == "SBNK_LEFT_MEMBER_TO_SMPL"
    ]
    assert len(relationships) == 1
    assert relationships[0].quality == "Known"


def test_hds_writer_serializes_single_member_sbnk_from_explicit_defaults(tmp_path: Path) -> None:
    source_wav = tmp_path / "tone.wav"
    _write_mono_wav(source_wav)
    image_path = tmp_path / "HD00_512_writer_explicit_sbnk.hds"

    builder = HdsImageBuilder(
        size_bytes=4 * 1024 * 1024,
        _sbnk_single_member_inactive_right_policy=sbnk_contract.CURRENT_SBNK_SINGLE_MEMBER_INACTIVE_RIGHT_POLICY_ZERO,
    )
    partition = builder.add_partition("hd1")
    volume = partition.add_volume("WriterVol")
    waveform = volume.add_waveform_from_wav(name="Tone0001", path=source_wav, root_key=60)
    volume.add_sample_bank(
        name="Bank0001",
        waveform=waveform,
        root_key=60,
        key_low=60,
        key_high=60,
        level=100,
    )

    builder.write(image_path)

    image_bytes = image_path.read_bytes()
    sbnk_record = _index_record(image_bytes, 10)
    sbnk_payload = _record_payload(image_bytes, sbnk_record)
    assert sbnk_payload[0x10:0x20] == bytes.fromhex("00000000000000040000013400000158")
    assert sbnk_payload[0x30:0x32] == bytes.fromhex("100c")
    assert sbnk_payload[0x43:0x4A] == bytes.fromhex("b8000af67a0154")
    assert _be32(sbnk_payload, 0x68) == 0x01443C30
    assert sbnk_payload[0x6C:0x70] == b"\x00" * 4
    assert sbnk_payload[0x74:0x78] == b"\x00" * 4
    assert sbnk_payload[0x78:0x88] == b"Tone0001        "
    assert sbnk_payload[0x88:0x98] == b"\x00" * 16
    assert _be32(sbnk_payload, 0x98) == 0x01443C30
    assert _be32(sbnk_payload, 0xA0) == 0x016B1DBC
    assert _be32(sbnk_payload, 0xA4) == 0
    assert sbnk_payload[0xA8:0xB8] == bytes.fromhex("4a04012047050120490b01e0480c01e0")
    assert sbnk_payload[0xD0] == 0x02
    assert sbnk_payload[0xD1] == 0
    assert sbnk_payload[0xD2:0xD6] == bytes.fromhex("00000200")
    assert sbnk_payload[0xD6] == 60
    assert sbnk_payload[0xD7] == 0
    assert sbnk_payload[0xD8:0xDC] == bytes.fromhex("ac440000")
    assert sbnk_payload[0xDD] == 0
    assert sbnk_payload[0xE0:0xE2] == b"\x00" * 2
    assert sbnk_payload[0xE2:0xE8] == bytes.fromhex("3c3c30012328")
    assert sbnk_payload[0x108:0x118] == bytes.fromhex("00007f04007f0000000000003f006400")
    assert sbnk_payload[0x152:0x157] == bytes.fromhex("c1e01e3a20")
    assert sbnk_payload[0x158:0x15C] == bytes.fromhex("3e20e1c6")
    assert sbnk_payload[0x164:0x17C] == bytes(
        [74, 4, 1, 32, 71, 5, 1, 32, 73, 11, 1, 224, 72, 12, 1, 224, 0, 0, 0, 0, 0, 0, 0, 0]
    )
    assert sbnk_payload[0x17C:0x185] == bytes.fromhex("0000017f007f005a5a")

    objects = load_sfs_objects(image_path)
    graph = build_relationship_graph(objects)
    relationships = [
        row for row in graph.relationships if row.relationship_type == "SBNK_LEFT_MEMBER_TO_SMPL"
    ]
    assert len(relationships) == 1
    assert relationships[0].quality == "Known"


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


def test_hds_writer_creates_two_member_stereo_bank_with_confirmed_links(
    tmp_path: Path,
) -> None:
    source_left = tmp_path / "stereo-left.wav"
    source_right = tmp_path / "stereo-right.wav"
    _write_mono_wav(source_left, samples=(0, 1000, -1000, 2000, -2000))
    _write_mono_wav(source_right, samples=(300, -300, 900, -900, 0))
    image_path = tmp_path / "HD00_512_writer_stereo.hds"
    builder = HdsImageBuilder(size_bytes=4 * 1024 * 1024)
    volume = builder.add_partition("hd1").add_volume("Stereo Vol")
    left = volume.add_waveform_from_wav(name="Stereo-L", path=source_left, root_key=60)
    right = volume.add_waveform_from_wav(name="Stereo-R", path=source_right, root_key=60)
    volume.add_stereo_sample_bank(
        name="Stereo Bank",
        left_waveform=left,
        right_waveform=right,
        root_key=60,
        key_low=48,
        key_high=72,
        level=96,
    )

    result = builder.write(image_path)

    assert [(item.object_type, item.name, item.sfs_id) for item in result.objects] == [
        ("SMPL", "Stereo-L", 9),
        ("SMPL", "Stereo-R", 10),
        ("SBNK", "Stereo Bank", 11),
    ]
    objects = load_sfs_objects(image_path)
    sbnk = next(item for item in objects if item.type == "SBNK")
    parsed = sbnk_contract.parse_current_sbnk_contract_payload(sbnk.payload)
    assert parsed.bank_topology == "two-member"
    assert parsed.left.sample_name == "Stereo-L"
    assert parsed.right is not None
    assert parsed.right.sample_name == "Stereo-R"
    assert parsed.left.sample_rate == parsed.right.sample_rate == 44_100
    assert parsed.left.wave_length_frames == parsed.right.wave_length_frames == 5
    assert parsed.key_range_low_0x0e3 == 48
    assert parsed.key_range_high_0x0e2 == 72
    assert parsed.sample_level_0x116 == 96
    assert sbnk.payload[0x0E5] == 1
    assert sbnk.payload[0x0EA:0x0EC] == b"\x00\x00"
    assert sbnk.payload[0x0EE:0x0F0] == b"\x00\x00"
    relationships = [
        row
        for row in build_relationship_graph(objects).relationships
        if row.relationship_type in {"SBNK_LEFT_MEMBER_TO_SMPL", "SBNK_RIGHT_MEMBER_TO_SMPL"}
    ]
    assert [(row.relationship_type, row.target_key, row.quality) for row in relationships] == [
        ("SBNK_LEFT_MEMBER_TO_SMPL", "p0:sfs9", "Known"),
        ("SBNK_RIGHT_MEMBER_TO_SMPL", "p0:sfs10", "Known"),
    ]


def test_hds_writer_imports_interleaved_stereo_into_two_mono_smpl_objects(
    tmp_path: Path,
) -> None:
    source = tmp_path / "interleaved.wav"
    left_pcm, right_pcm = _write_stereo_wav(
        source,
        (0, 1000, -1000, 2000, -2000),
        (300, -300, 900, -900, 0),
    )
    image_path = tmp_path / "HD00_512_interleaved_stereo.hds"
    builder = HdsImageBuilder(size_bytes=4 * 1024 * 1024)
    volume = builder.add_partition("hd1").add_volume("Stereo Vol")
    volume.add_stereo_sample_bank_from_audio(
        name="Stereo Bank",
        path=source,
        root_key=60,
        key_low=48,
        key_high=72,
        level=96,
    )

    result = builder.write(image_path)

    assert [(item.object_type, item.name) for item in result.objects] == [
        ("SMPL", "Stereo Bank-L"),
        ("SMPL", "Stereo Bank-R"),
        ("SBNK", "Stereo Bank"),
    ]
    assert len(result.audio_imports) == 1
    report = result.audio_imports[0]
    assert report.waveform_names == ("Stereo Bank-L", "Stereo Bank-R")
    assert report.split_stereo
    assert not report.resampled
    assert not report.quantized
    assert not result.warnings

    objects = load_sfs_objects(image_path)
    waveforms = {
        item.name: decode_waveform(item).pcm
        for item in objects
        if item.object_type == AxklibObjectType.SMPL
    }
    assert waveforms == {
        "Stereo Bank-L": left_pcm + left_pcm[:8],
        "Stereo Bank-R": right_pcm + right_pcm[:8],
    }
    graph = build_relationship_graph(objects)
    stereo_links = [
        row
        for row in graph.relationships
        if row.relationship_type in {"SBNK_LEFT_MEMBER_TO_SMPL", "SBNK_RIGHT_MEMBER_TO_SMPL"}
    ]
    assert [(row.relationship_type, row.target_key, row.quality) for row in stereo_links] == [
        ("SBNK_LEFT_MEMBER_TO_SMPL", "p0:sfs9", "Known"),
        ("SBNK_RIGHT_MEMBER_TO_SMPL", "p0:sfs10", "Known"),
    ]


@pytest.mark.parametrize(
    ("right_samples", "right_sample_rate", "message"),
    [
        ((0, 1, 2, 3), 44_100, "matching logical frame counts"),
        ((0, 1, 2, 3, 4), 48_000, "matching sample rates"),
    ],
)
def test_hds_writer_rejects_mismatched_stereo_members(
    tmp_path: Path,
    right_samples: tuple[int, ...],
    right_sample_rate: int,
    message: str,
) -> None:
    source_left = tmp_path / "left.wav"
    source_right = tmp_path / "right.wav"
    _write_mono_wav(source_left, samples=(0, 1, 2, 3, 4))
    _write_mono_wav(
        source_right,
        samples=right_samples,
        sample_rate=right_sample_rate,
    )
    builder = HdsImageBuilder(size_bytes=4 * 1024 * 1024)
    volume = builder.add_partition("hd1").add_volume("Stereo Vol")
    left = volume.add_waveform_from_wav(name="Left", path=source_left, root_key=60)
    right = volume.add_waveform_from_wav(name="Right", path=source_right, root_key=60)
    volume.add_stereo_sample_bank(
        name="Stereo Bank",
        left_waveform=left,
        right_waveform=right,
        root_key=60,
        key_low=0,
        key_high=127,
    )

    with pytest.raises(ValueError, match=message):
        builder.plan()


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
    assert image_bytes[1024:1032] == b"ab432100"
    assert image_bytes[1024 + 0x09 : 1024 + 0x11] == b"\x00" * 8
    assert image_bytes[1024 + 0x12 : 1024 + 0x2C] == b"\x00" * 0x1A
    assert image_bytes[1024 + 0x30 : 1024 + 0x3F] == b"\x00" * 15
    assert image_bytes[1024 + 0x40 : 1024 + 0x5A] == b"\x00" * 0x1A
    pre_partition_b = (524_290 - 1) * 512
    assert image_bytes[pre_partition_b : pre_partition_b + 8] == b"ab432101"
    assert image_bytes[pre_partition_b + 0x09 : pre_partition_b + 0x11] == b"\x00" * 8
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
    _assert_partition_header_fixed_tail_zero(image_bytes, 0)
    _assert_partition_header_residue_zero(image_bytes, 1)
    _assert_partition_header_fixed_tail_zero(image_bytes, 1)

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
    assert image_bytes[1024:1032] == b"ab432100"
    assert image_bytes[1033:1041] == b"\x00" * 8
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
    _assert_partition_header_fixed_tail_zero(image_bytes, 0)
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


def test_hds_writer_creates_768m_three_partition_empty_volume_image(tmp_path: Path) -> None:
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
        _assert_partition_header_fixed_tail_zero(image_bytes, index)

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


def test_hds_writer_writes_eight_small_partitions(
    tmp_path: Path,
) -> None:
    image_path = tmp_path / "HD00_512_8m_8p.hds"
    builder = HdsImageBuilder(size_bytes=8 * 1024 * 1024)
    for _index in range(8):
        partition = builder.add_partition("New Partition")
        partition.add_volume("New Volume")

    result = builder.write(image_path)

    assert result.partitions == 8
    assert result.unused_tail_sectors == 6
    assert [layout.start_sector for layout in result.partition_layouts] == [
        3 + index * 2047 for index in range(8)
    ]
    assert [layout.sector_count for layout in result.partition_layouts] == [2046] * 8
    image_bytes = image_path.read_bytes()
    for index, layout in enumerate(result.partition_layouts):
        transfer_sector = 2 if index == 0 else layout.start_sector - 1
        transfer_offset = transfer_sector * 512
        assert image_bytes[transfer_offset : transfer_offset + 8] == f"ab43210{index}".encode(
            "ascii"
        )
        assert image_bytes[transfer_offset + 8 : transfer_offset + 512] == b"\x00" * 504

        header_offset = layout.start_sector * 512
        assert (
            image_bytes[header_offset : header_offset + 1024]
            == image_bytes[header_offset + 1024 : header_offset + 2048]
        )
        assert _be32(image_bytes, header_offset + 0x104) == layout.start_sector
        assert _be32(image_bytes, header_offset + 0x11C) == layout.sector_count
        assert _be32(image_bytes, header_offset + 0x134) == 0x0152A3FC + index * 0x11
        assert _be32(image_bytes, header_offset + 0x14C) == 16_384 - 6
        assert _be32(image_bytes, header_offset + 0x194) == 0
        expected_name = "New Partition" if index == 0 else f"New Partition  {index}"
        assert image_bytes[header_offset + 0x40 : header_offset + 0x50] == expected_name.encode(
            "ascii"
        ).ljust(16)

        volume_payload = _record_payload_in_partition(
            image_bytes,
            index,
            _index_record_in_partition(image_bytes, index, 3),
        )
        assert volume_payload[31] == (index * 0x78) & 0xFF


def test_hds_writer_creates_768m_object_volume_on_first_partition(
    tmp_path: Path,
) -> None:
    source_wav = tmp_path / "tone.wav"
    _write_mono_wav(source_wav)
    image_path = tmp_path / "HD00_512_sparse_768m_3p_p0_object.hds"

    builder = HdsImageBuilder(size_bytes=768 * 1024 * 1024)
    partition_a = builder.add_partition("New Partition")
    volume_a = partition_a.add_volume("New Volume")
    waveform = volume_a.add_waveform_from_wav(name="Tone0001", path=source_wav, root_key=60)
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
        (obj.partition_index, obj.sfs_id, obj.object_type, obj.name) for obj in result.objects
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


def test_hds_writer_creates_768m_object_volume_on_second_partition(
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
    waveform = volume_b.add_waveform_from_wav(name="Tone0001", path=source_wav, root_key=60)
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
        (obj.partition_index, obj.sfs_id, obj.object_type, obj.name) for obj in result.objects
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


def test_hds_writer_creates_768m_object_volume_on_third_partition(
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
    waveform = volume_c.add_waveform_from_wav(name="Tone0001", path=source_wav, root_key=60)
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
        (obj.partition_index, obj.sfs_id, obj.object_type, obj.name) for obj in result.objects
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
