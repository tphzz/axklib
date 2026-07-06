"""Generated Yamaha SFS/HDS image writer.

The writer starts with a narrow generated-image contract: fresh
hard-disk image, SFS partitions, volumes, current SMPL waveforms, and direct
single-member SBNK sample banks. It does not patch existing images.
"""

from __future__ import annotations

import wave
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import BinaryIO

from axklib.containers import sfs_allocation
from axklib.parameters import sbnk_contract

SECTOR_SIZE = 512
SECTORS_PER_CLUSTER = 2
CLUSTER_SIZE = SECTOR_SIZE * SECTORS_PER_CLUSTER
MAX_IMAGE_SIZE_BYTES = 2_147_483_648
MAX_PARTITION_SIZE_BYTES = 1_073_741_824
MIN_IMAGE_SIZE_BYTES = 1_048_576
PARTITION_START_SECTOR = 3
PARTITION_HEADER_SIZE = 1024
PARTITION_ENTRY_COUNT = 8
DIRECTORY_INDEX_SPAN_CLUSTERS = 358
DIRECT_EXTENT_CLUSTER_LIMIT = 0xFFFF
MIN_ALLOCATED_RECORD_CLUSTERS = 2
DISK_MODE_METADATA = bytes.fromhex(
    "a1 e0 01 52 a2 2c 00 00 00 00 00 17 09 10 00 00 00 00 00 17 09 10 00 00 01 00 01 52"
)
DEFAULT_DISK_IDENTIFIER = b"ab432100"
SECTOR2_PRIMARY_IDENTIFIER = b"c2b4e600"
SECTOR2_METADATA_MARKER = bytes.fromhex("23 44 01 54 23 94")
SECTOR2_FLAGS_OFFSET = 0x2B
SECTOR2_FLAGS_VALUE = 0x90
SUPPORTED_HARDWARE_METADATA_PARTITION_SECTORS = 524_285
PARTITION_HEADER_METADATA_256M = bytes.fromhex(
    "000000000000000300001388000002000000000200000000000000030007fffd00000400000320000017ab4a01529f34"
    "001aa5400152a3fc00000002000000c800001388016f5bf0000000000008000001529fa0000000000152a3fc00000000"
    "000000000152a3fc0000000800000400000aa30e01529f78000000030007fffd001aa5400152a3fc0000138800000400"
    "000000000000000100000000000000000000000200000000000000000000000000000000000a9f760152a22400000000"
    "00000001000000000000000100186cc4016eb30c000002021f000018426c75650043000001529fb400000002000000c8"
    "00000400000000000000000000000000"
)
OBJECT_MAGIC = b"FSFSDEV3SPLX"
SMPL_COMPACT_HEADER_SIZE = 0xAC
SMPL_OBJECT_HEADER_SIZE = 0x200
SBNK_OBJECT_SIZE = 0x188
SBNK_PAYLOAD_SIZE = sbnk_contract.CURRENT_SBNK_CONTRACT_PAYLOAD_SIZE
ROOT_DIRECTORY_ID = 1
SFS_ERROR_LOG_ID = 2
STANDARD_VOLUME_CATEGORY_NAMES = ("SMPL", "SBNK", "SBAC", "SEQU", "PROG")
SMPL_LINK_ID_BASE = 0x016B1DBC
SMPL_AUX_ID_DELTA = 0xBA
SMPL_OBJECT_HANDLE = 0x01443840
SBNK_OBJECT_HANDLE = 0x01443C30
SMPL_STORED_TAIL_FRAMES = 4
DEFAULT_FINE_TUNE_CENTS = 0
SBNK_SINGLE_MEMBER_DEFAULT_PAYLOAD = bytes.fromhex(
    "465346534445563353504c5853424e4b0000000000000004000001340000015800000000000000000000000000000000"
    "100c73696e6520776176652020202020202000b8000af67a015400000000000000000000000000000000000000000000"
    "000000000000000001443c3073696e580000000001443c3073696e652077617665202020202020200000000000000000"
    "000000000000000001443c3000000000016b1dbc000000004a04012047050120490b01e0480c01e00000000000000000"
    "000000000000000000000000000000000200000002004242bb80bb80ecec154215427f00300123280000000000000000"
    "00000080000000000000000000000000000000800000000000007f04007f0000000000003f00640000007f00007f7f7f"
    "00001a400a007f7f7f00000000000000007f7f7f000000000000000c7f7f7e087f7f0000000001270001000000017f00"
    "7f00c1e01e3a20003e20e1c600000080000000804a04012047050120490b01e0480c01e000000000000000000000017f"
    "007f005a5a000000"
)


def _be16(value: int) -> bytes:
    return value.to_bytes(2, "big")


def _be32(value: int) -> bytes:
    return value.to_bytes(4, "big")


def _ceil_div(value: int, divisor: int) -> int:
    return (value + divisor - 1) // divisor


def _ascii_field(value: str, size: int, *, pad: bytes = b" ") -> bytes:
    encoded = value.encode("ascii")
    if len(encoded) > size:
        raise ValueError(f"ASCII field is too long for {size} bytes: {value!r}")
    return encoded.ljust(size, pad)


def _directory_entry(
    name: str,
    link_id: int,
    *,
    fixed_name_width: int | None = None,
    directory_id: int | None = None,
) -> bytes:
    name_bytes = (
        _ascii_field(name, fixed_name_width)
        if fixed_name_width is not None
        else name.encode("ascii")
    )
    raw_name = name_bytes + b"\x00"
    if len(raw_name) > 24:
        raise ValueError(f"SFS directory entry name is too long: {name!r}")
    entry = bytearray(32)
    entry[0:2] = _be16(0x20)
    entry[2:4] = _be16(len(raw_name))
    entry[4:8] = _be32(link_id)
    entry[8 : 8 + len(raw_name)] = raw_name
    if name == ".":
        entry[10:12] = b"\x4f\x58"
        entry[16:20] = b"\x00\x17\x82\x1a"
        entry[24:28] = b"\xff" * 4
    elif name == "..":
        if directory_id is None:
            raise ValueError("directory_id is required for parent directory entries")
        if directory_id > 0xFF:
            raise ValueError("v1 writer supports directory IDs up to 255")
        entry[11] = directory_id
        entry[16:20] = b"\x00\x17\x83\xc8"
    return bytes(entry)


def _directory_payload(
    directory_id: int,
    parent_id: int,
    entries: Sequence[tuple[str, int, int | None]],
) -> bytes:
    payload = bytearray()
    payload.extend(_directory_entry(".", directory_id, directory_id=directory_id))
    payload.extend(_directory_entry("..", parent_id, directory_id=directory_id))
    for name, link_id, fixed_name_width in entries:
        payload.extend(_directory_entry(name, link_id, fixed_name_width=fixed_name_width))
    return bytes(payload)


def _swap_16bit_words(data: bytes) -> bytes:
    if len(data) % 2:
        raise ValueError("16-bit PCM byte count must be even")
    out = bytearray(len(data))
    for offset in range(0, len(data), 2):
        out[offset] = data[offset + 1]
        out[offset + 1] = data[offset]
    return bytes(out)


def _stored_smpl_pcm(logical_big_endian_pcm: bytes) -> bytes:
    tail_bytes = min(len(logical_big_endian_pcm), SMPL_STORED_TAIL_FRAMES * 2)
    return logical_big_endian_pcm + logical_big_endian_pcm[:tail_bytes]


def _pitch_base_word(
    root_key: int,
    sample_rate: int,
    fine_tune: int = DEFAULT_FINE_TUNE_CENTS,
) -> int:
    return sbnk_contract.estimated_pitch_base_word(root_key, sample_rate, fine_tune) or 0


def _put_be16(data: bytearray, offset: int, value: int) -> None:
    data[offset : offset + 2] = _be16(value)


def _put_be32(data: bytearray, offset: int, value: int) -> None:
    data[offset : offset + 4] = _be32(value)


@dataclass(frozen=True)
class WaveformRef:
    """Reference returned by :meth:`VolumeBuilder.add_waveform_from_wav`."""

    key: str


@dataclass(frozen=True)
class SampleBankRef:
    """Reference returned by :meth:`VolumeBuilder.add_sample_bank`."""

    key: str


@dataclass(frozen=True)
class WrittenObjectRef:
    key: str
    object_type: str
    name: str
    partition_index: int
    sfs_id: int
    cluster_offset: int
    byte_count: int


@dataclass(frozen=True)
class HdsWriteResult:
    path: Path
    size_bytes: int
    partitions: int
    objects: tuple[WrittenObjectRef, ...]
    warnings: tuple[str, ...] = ()


@dataclass
class _WaveformSpec:
    key: str
    name: str
    path: Path
    root_key: int
    link_id: int


@dataclass
class _SampleBankSpec:
    key: str
    name: str
    waveform_key: str
    root_key: int
    key_low: int
    key_high: int
    level: int


@dataclass
class _LoadedWaveform:
    spec: _WaveformSpec
    sample_rate: int
    frames: int
    pcm_little_endian: bytes
    stored_big_endian: bytes


@dataclass
class _RecordPlan:
    sfs_id: int
    payload: bytes
    payload_kind: str
    object_type: str = ""
    object_name: str = ""
    cluster_offset: int = 0
    cluster_count: int = 0
    directory_tail_value: int = 0

    @property
    def byte_count(self) -> int:
        return len(self.payload)


@dataclass
class _PartitionPlan:
    builder: PartitionBuilder
    index: int
    start_sector: int
    sector_count: int
    cluster_count: int
    bitmap_cluster: int
    bitmap_cluster_count: int
    directory_index_cluster: int
    first_payload_cluster: int
    records: list[_RecordPlan]


@dataclass
class VolumeBuilder:
    name: str
    _partition: PartitionBuilder
    _waveforms: list[_WaveformSpec] = field(default_factory=list)
    _sample_banks: list[_SampleBankSpec] = field(default_factory=list)

    def add_waveform_from_wav(self, *, name: str, path: str | Path, root_key: int) -> WaveformRef:
        _validate_midi_key(root_key, "root_key")
        key = _unique_key(name, {item.key for item in self._waveforms}, "waveform")
        link_id = SMPL_LINK_ID_BASE + len(self._waveforms) * 0x100
        self._waveforms.append(
            _WaveformSpec(
                key=key,
                name=name,
                path=Path(path),
                root_key=root_key,
                link_id=link_id,
            )
        )
        return WaveformRef(key)

    def add_sample_bank(
        self,
        *,
        name: str,
        waveform: WaveformRef,
        root_key: int,
        key_low: int,
        key_high: int,
        level: int = 127,
    ) -> SampleBankRef:
        _validate_midi_key(root_key, "root_key")
        _validate_midi_key(key_low, "key_low")
        _validate_midi_key(key_high, "key_high")
        if key_high < key_low:
            raise ValueError("key_high must be greater than or equal to key_low")
        if level < 0 or level > 127:
            raise ValueError("level must be in MIDI range 0..127")
        if waveform.key not in {item.key for item in self._waveforms}:
            raise ValueError(f"unknown waveform reference: {waveform.key}")
        key = _unique_key(name, {item.key for item in self._sample_banks}, "sample bank")
        self._sample_banks.append(
            _SampleBankSpec(
                key=key,
                name=name,
                waveform_key=waveform.key,
                root_key=root_key,
                key_low=key_low,
                key_high=key_high,
                level=level,
            )
        )
        return SampleBankRef(key)


@dataclass
class PartitionBuilder:
    name: str
    _image: HdsImageBuilder
    _volumes: list[VolumeBuilder] = field(default_factory=list)

    def add_volume(self, name: str) -> VolumeBuilder:
        if not name:
            raise ValueError("volume name must not be empty")
        _ascii_field(name, 23)
        volume = VolumeBuilder(name=name, _partition=self)
        self._volumes.append(volume)
        return volume


class HdsImageBuilder:
    """Build a fresh Yamaha SFS hard-disk image from a typed model."""

    def __init__(self, *, size_bytes: int) -> None:
        if size_bytes < MIN_IMAGE_SIZE_BYTES:
            raise ValueError(f"image size must be at least {MIN_IMAGE_SIZE_BYTES} bytes")
        if size_bytes > MAX_IMAGE_SIZE_BYTES:
            raise ValueError(f"image size exceeds A-series cap: {MAX_IMAGE_SIZE_BYTES} bytes")
        if size_bytes % SECTOR_SIZE:
            raise ValueError("image size must be a multiple of 512 bytes")
        self.size_bytes = size_bytes
        self._partitions: list[PartitionBuilder] = []

    def add_partition(self, name: str) -> PartitionBuilder:
        if len(self._partitions) >= PARTITION_ENTRY_COUNT:
            raise ValueError("A-series SFS images support at most 8 partitions")
        if not name:
            raise ValueError("partition name must not be empty")
        _ascii_field(name, 16)
        partition = PartitionBuilder(name=name, _image=self)
        self._partitions.append(partition)
        return partition

    def plan(self) -> list[_PartitionPlan]:
        if not self._partitions:
            raise ValueError("image must contain at least one partition")
        plans = _layout_partitions(self.size_bytes, self._partitions)
        for plan in plans:
            _build_partition_records(plan)
        return plans

    def write(self, path: str | Path) -> HdsWriteResult:
        output = Path(path)
        output.parent.mkdir(parents=True, exist_ok=True)
        plans = self.plan()
        writes: list[tuple[int, bytes]] = []
        writes.extend(_superblock_writes(self.size_bytes, plans))
        written_objects: list[WrittenObjectRef] = []
        for plan in plans:
            writes.extend(_partition_writes(plan))
            written_objects.extend(_object_refs(plan))
        with output.open("wb") as handle:
            handle.truncate(self.size_bytes)
            for offset, data in sorted(writes, key=lambda item: item[0]):
                _write_at(handle, offset, data)
        return HdsWriteResult(
            path=output,
            size_bytes=self.size_bytes,
            partitions=len(plans),
            objects=tuple(written_objects),
        )


def _validate_midi_key(value: int, name: str) -> None:
    if value < 0 or value > 127:
        raise ValueError(f"{name} must be in MIDI range 0..127")


def _unique_key(name: str, existing: set[str], label: str) -> str:
    if not name:
        raise ValueError(f"{label} name must not be empty")
    _ascii_field(name, 16)
    key = name
    if key in existing:
        raise ValueError(f"duplicate {label} name: {name!r}")
    return key


def _layout_partitions(size_bytes: int, partitions: list[PartitionBuilder]) -> list[_PartitionPlan]:
    total_sectors = size_bytes // SECTOR_SIZE
    available_sectors = total_sectors - PARTITION_START_SECTOR
    if available_sectors <= 0:
        raise ValueError("image is too small for an SFS partition")
    partition_count = len(partitions)
    sectors_per_partition = available_sectors // partition_count
    max_partition_sectors = MAX_PARTITION_SIZE_BYTES // SECTOR_SIZE
    if sectors_per_partition > max_partition_sectors:
        raise ValueError(
            "partition size would exceed 1 GiB; add more partitions or reduce image size"
        )
    if sectors_per_partition < 2045:
        raise ValueError("partition is too small for the generated SFS layout")
    plans: list[_PartitionPlan] = []
    start = PARTITION_START_SECTOR
    for index, partition in enumerate(partitions):
        sector_count = sectors_per_partition
        if index == partition_count - 1:
            sector_count = available_sectors - sectors_per_partition * (partition_count - 1)
        if sector_count > max_partition_sectors:
            raise ValueError("final partition size would exceed 1 GiB")
        cluster_count = sector_count // SECTORS_PER_CLUSTER
        bitmap_cluster_count = _ceil_div(_ceil_div(cluster_count, 8), CLUSTER_SIZE)
        bitmap_cluster = 2 + bitmap_cluster_count
        directory_index_cluster = bitmap_cluster + bitmap_cluster_count
        first_payload_cluster = directory_index_cluster + DIRECTORY_INDEX_SPAN_CLUSTERS
        plans.append(
            _PartitionPlan(
                builder=partition,
                index=index,
                start_sector=start,
                sector_count=sector_count,
                cluster_count=cluster_count,
                bitmap_cluster=bitmap_cluster,
                bitmap_cluster_count=bitmap_cluster_count,
                directory_index_cluster=directory_index_cluster,
                first_payload_cluster=first_payload_cluster,
                records=[],
            )
        )
        start += sector_count
    return plans


def _build_partition_records(plan: _PartitionPlan) -> None:
    records: list[_RecordPlan] = [
        _RecordPlan(0, b"\x00" * (32 * CLUSTER_SIZE), "hidden"),
        _RecordPlan(SFS_ERROR_LOG_ID, b"", "system"),
    ]
    root_entries: list[tuple[str, int, int | None]] = [
        ("sfserrlog", SFS_ERROR_LOG_ID, None),
        ("sfserram", 0, None),
    ]
    next_id = SFS_ERROR_LOG_ID + 1
    for volume in plan.builder._volumes:
        volume_id = next_id
        next_id += 1
        category_ids = {
            name: next_id + index for index, name in enumerate(STANDARD_VOLUME_CATEGORY_NAMES)
        }
        next_id += len(STANDARD_VOLUME_CATEGORY_NAMES)
        root_entries.append((volume.name, volume_id, 16))

        loaded = [_load_waveform(spec) for spec in volume._waveforms]
        loaded_by_key = {item.spec.key: item for item in loaded}
        smpl_entries: list[tuple[str, int, int | None]] = []
        sbnk_entries: list[tuple[str, int, int | None]] = []
        object_payloads: list[tuple[int, bytes, str, str, str]] = []

        for waveform in loaded:
            smpl_id = next_id
            next_id += 1
            smpl_entries.append((waveform.spec.name, smpl_id, 16))
            object_payloads.append(
                (
                    smpl_id,
                    _serialize_smpl(waveform),
                    "object",
                    "SMPL",
                    waveform.spec.name,
                )
            )

        for bank in volume._sample_banks:
            waveform = loaded_by_key[bank.waveform_key]
            sbnk_id = next_id
            next_id += 1
            sbnk_entries.append((bank.name, sbnk_id, 16))
            object_payloads.append(
                (
                    sbnk_id,
                    _serialize_sbnk(bank, waveform),
                    "object",
                    "SBNK",
                    bank.name,
                )
            )

        volume_entries = [
            (name, category_ids[name], None) for name in STANDARD_VOLUME_CATEGORY_NAMES
        ]
        records.append(
            _RecordPlan(
                volume_id,
                _directory_payload(volume_id, ROOT_DIRECTORY_ID, volume_entries),
                "directory",
                directory_tail_value=2 + len(volume_entries),
            )
        )
        records.append(
            _RecordPlan(
                category_ids["SMPL"],
                _directory_payload(category_ids["SMPL"], volume_id, smpl_entries),
                "directory",
                directory_tail_value=2,
            )
        )
        records.append(
            _RecordPlan(
                category_ids["SBNK"],
                _directory_payload(category_ids["SBNK"], volume_id, sbnk_entries),
                "directory",
                directory_tail_value=2,
            )
        )
        for category_name in ("SBAC", "SEQU", "PROG"):
            records.append(
                _RecordPlan(
                    category_ids[category_name],
                    _directory_payload(category_ids[category_name], volume_id, []),
                    "directory",
                    directory_tail_value=2,
                )
            )
        for sfs_id, payload, kind, object_type, object_name in object_payloads:
            records.append(_RecordPlan(sfs_id, payload, kind, object_type, object_name))

    records.append(
        _RecordPlan(
            ROOT_DIRECTORY_ID,
            _directory_payload(ROOT_DIRECTORY_ID, ROOT_DIRECTORY_ID, root_entries),
            "directory",
            directory_tail_value=len(root_entries),
        )
    )
    records.sort(key=lambda item: item.sfs_id)
    if [record.sfs_id for record in records] != list(range(len(records))):
        raise ValueError("generated SFS records must be contiguous from SFS ID 0")
    cluster = plan.first_payload_cluster
    for record in records:
        if record.payload_kind == "system":
            record.cluster_offset = 0
            record.cluster_count = 0
            continue
        record.cluster_offset = cluster
        record.cluster_count = _ceil_div(len(record.payload), CLUSTER_SIZE)
        if record.payload_kind in {"directory", "object"}:
            record.cluster_count = max(record.cluster_count, MIN_ALLOCATED_RECORD_CLUSTERS)
        if record.cluster_count > DIRECT_EXTENT_CLUSTER_LIMIT:
            raise ValueError("v1 writer supports only direct extents up to 65535 clusters")
        cluster += record.cluster_count
    if cluster >= plan.cluster_count:
        raise ValueError("generated partition does not have enough clusters for payloads")
    plan.records = records


def _load_waveform(spec: _WaveformSpec) -> _LoadedWaveform:
    with wave.open(str(spec.path), "rb") as wav:
        if wav.getnchannels() != 1:
            raise ValueError("v1 writer supports only mono WAV input")
        if wav.getsampwidth() != 2:
            raise ValueError("v1 writer supports only 16-bit PCM WAV input")
        if wav.getcomptype() != "NONE":
            raise ValueError("v1 writer supports only uncompressed PCM WAV input")
        frames = wav.getnframes()
        sample_rate = wav.getframerate()
        pcm = wav.readframes(frames)
    if frames <= 0:
        raise ValueError("WAV input must contain at least one frame")
    if sample_rate <= 0 or sample_rate > 0xFFFF:
        raise ValueError("sample rate is outside the current SMPL field range")
    return _LoadedWaveform(
        spec=spec,
        sample_rate=sample_rate,
        frames=frames,
        pcm_little_endian=pcm,
        stored_big_endian=_stored_smpl_pcm(_swap_16bit_words(pcm)),
    )


def _serialize_smpl(waveform: _LoadedWaveform) -> bytes:
    stored_size = len(waveform.stored_big_endian)
    pitch_base = _pitch_base_word(waveform.spec.root_key, waveform.sample_rate)
    header = bytearray(SMPL_OBJECT_HEADER_SIZE)
    header[0 : len(OBJECT_MAGIC)] = OBJECT_MAGIC
    header[0x0C:0x10] = b"SMPL"
    _put_be32(header, 0x10, SMPL_OBJECT_HEADER_SIZE)
    _put_be32(header, 0x14, 3)
    _put_be32(header, 0x18, SMPL_COMPACT_HEADER_SIZE - 0x30)
    _put_be32(header, 0x1C, stored_size)
    _put_be32(header, 0x20, stored_size)
    _put_be16(header, 0x28, waveform.sample_rate)
    _put_be16(header, 0x2A, 2)
    header[0x30:0x32] = b"\x02\xc0"
    header[0x32:0x42] = _ascii_field(waveform.spec.name, 16)
    header[0x42:0x4A] = bytes.fromhex("0000000a877c0154")
    _put_be32(header, 0x68, SMPL_OBJECT_HANDLE)
    _put_be32(header, 0x6C, waveform.spec.link_id - SMPL_AUX_ID_DELTA)
    _put_be32(header, 0x74, SMPL_OBJECT_HANDLE)
    _put_be32(header, 0x78, waveform.spec.link_id)
    _put_be16(header, 0x7C, waveform.sample_rate)
    header[0x7E] = waveform.spec.root_key
    header[0x7F] = DEFAULT_FINE_TUNE_CENTS & 0xFF
    _put_be16(header, 0x80, pitch_base)
    _put_be32(header, 0x84, 0x30010000)
    _put_be32(header, 0x92, waveform.frames)
    _put_be32(header, 0x96, 0)
    _put_be32(header, 0x9A, waveform.frames)
    return bytes(header) + waveform.stored_big_endian


def _serialize_sbnk(bank: _SampleBankSpec, waveform: _LoadedWaveform) -> bytes:
    pitch_base = _pitch_base_word(bank.root_key, waveform.sample_rate)
    payload = bytearray(SBNK_SINGLE_MEMBER_DEFAULT_PAYLOAD)
    payload[0 : len(OBJECT_MAGIC)] = OBJECT_MAGIC
    payload[0x0C:0x10] = b"SBNK"
    payload[0x32:0x42] = _ascii_field(bank.name, 16)
    payload[0x50:0x68] = b"\x00" * 24
    _put_be32(payload, 0x68, SBNK_OBJECT_HANDLE)
    payload[0x78:0x88] = _ascii_field(waveform.spec.name, 16)
    payload[0x88:0x98] = b"\x00" * 16
    _put_be32(payload, 0x98, SBNK_OBJECT_HANDLE)
    _put_be32(payload, 0xA0, waveform.spec.link_id)
    _put_be32(payload, 0xA4, 0)
    payload[0x0D6] = bank.root_key
    payload[0x0D7] = bank.root_key
    _put_be16(payload, 0x0D8, waveform.sample_rate)
    _put_be16(payload, 0x0DA, waveform.sample_rate)
    payload[0x0DC] = DEFAULT_FINE_TUNE_CENTS & 0xFF
    payload[0x0DD] = DEFAULT_FINE_TUNE_CENTS & 0xFF
    _put_be16(payload, 0x0DE, pitch_base)
    _put_be16(payload, 0x0E0, pitch_base)
    payload[0x0E2] = bank.key_high
    payload[0x0E3] = bank.key_low
    _put_be32(payload, 0x0F0, waveform.frames)
    _put_be32(payload, 0x0F4, 0)
    _put_be32(payload, 0x0F8, 0)
    _put_be32(payload, 0x0FC, 0)
    _put_be32(payload, 0x100, waveform.frames)
    _put_be32(payload, 0x104, 0)
    payload[0x116] = bank.level
    return bytes(payload[:SBNK_OBJECT_SIZE])


def _superblock_writes(size_bytes: int, plans: list[_PartitionPlan]) -> list[tuple[int, bytes]]:
    block = bytearray(SECTOR_SIZE)
    block[0 : len(b"YAMAHA_dev3")] = b"YAMAHA_dev3"
    block[0x80:0x9C] = DISK_MODE_METADATA
    _put_be32(block, 0x9C, SECTOR_SIZE)
    _put_be32(block, 0xA0, size_bytes // SECTOR_SIZE)
    for plan in plans:
        rel = 0x0A8 + plan.index * 8
        _put_be32(block, rel, plan.start_sector)
        _put_be32(block, rel + 4, plan.sector_count)
    return [
        (0, bytes(block)),
        (SECTOR_SIZE, bytes(block)),
        (SECTOR_SIZE * 2, _sector2_metadata(plans)),
    ]


def _sector2_metadata(plans: list[_PartitionPlan]) -> bytes:
    metadata = bytearray(SECTOR_SIZE)
    metadata[0:8] = SECTOR2_PRIMARY_IDENTIFIER
    metadata[9:17] = DEFAULT_DISK_IDENTIFIER
    metadata[0x12:0x18] = SECTOR2_METADATA_MARKER
    if plans:
        metadata[0x19] = 0x13
        metadata[0x1A:0x2A] = _ascii_field(plans[0].builder.name, 16)
    metadata[SECTOR2_FLAGS_OFFSET] = SECTOR2_FLAGS_VALUE
    return bytes(metadata)


def _partition_writes(plan: _PartitionPlan) -> list[tuple[int, bytes]]:
    start_offset = plan.start_sector * SECTOR_SIZE
    header = bytearray(1024)
    header[0 : len(b"YAMAHA_dev3")] = b"YAMAHA_dev3"
    header[0x40:0x50] = _ascii_field(plan.builder.name, 16)
    _put_be32(header, 0x80, 2)
    _put_be32(header, 0x84, 200)
    header[0x88:0x90] = b"\xff" * 8
    _put_be32(header, 0x90, plan.cluster_count)
    _put_be32(header, 0x94, SECTORS_PER_CLUSTER)
    _put_be32(header, 0x98, 2)
    _put_be32(header, 0x9C, plan.bitmap_cluster)
    _put_be32(header, 0xA0, 5012)
    _put_be32(header, 0xA4, plan.directory_index_cluster)
    _put_be32(header, 0xA8, DIRECTORY_INDEX_SPAN_CLUSTERS)
    _apply_partition_metadata_profile(header, plan)

    bitmap = bytearray(plan.bitmap_cluster_count * CLUSTER_SIZE)
    index = bytearray(_ceil_div(len(plan.records) * 72, 1024) * 1024)
    payload_writes: list[tuple[int, bytes]] = []
    for record in plan.records:
        if record.cluster_count:
            _mark_extent(bitmap, record.cluster_offset, record.cluster_count, plan.cluster_count)
        rel = _index_record_offset(record.sfs_id)
        index[rel : rel + 72] = _index_record(record)
        if record.payload:
            payload_offset = _cluster_offset(plan, record.cluster_offset)
            payload_writes.append((payload_offset, record.payload))
    bitmap_offset = _cluster_offset(plan, plan.bitmap_cluster)
    bitmap_mirror_offset = start_offset + PARTITION_HEADER_SIZE * 2
    index_offset = _cluster_offset(plan, plan.directory_index_cluster)
    return [
        (start_offset, bytes(header)),
        (start_offset + 1024, bytes(header)),
        (bitmap_mirror_offset, bytes(bitmap[:SECTOR_SIZE])),
        (bitmap_offset, bytes(bitmap)),
        (index_offset, bytes(index)),
        *payload_writes,
    ]


def _apply_partition_metadata_profile(header: bytearray, plan: _PartitionPlan) -> None:
    if plan.sector_count == SUPPORTED_HARDWARE_METADATA_PARTITION_SECTORS:
        header[0x100:0x200] = PARTITION_HEADER_METADATA_256M


def _object_refs(plan: _PartitionPlan) -> list[WrittenObjectRef]:
    refs: list[WrittenObjectRef] = []
    for record in plan.records:
        if record.payload_kind != "object":
            continue
        refs.append(
            WrittenObjectRef(
                key=f"p{plan.index}:sfs{record.sfs_id}",
                object_type=record.object_type,
                name=record.object_name,
                partition_index=plan.index,
                sfs_id=record.sfs_id,
                cluster_offset=record.cluster_offset,
                byte_count=record.byte_count,
            )
        )
    return refs


def _cluster_offset(plan: _PartitionPlan, cluster_offset: int) -> int:
    return (plan.start_sector + cluster_offset * SECTORS_PER_CLUSTER) * SECTOR_SIZE


def _index_record_offset(sfs_id: int) -> int:
    block = sfs_id // 14
    slot = sfs_id % 14
    return block * 1024 + slot * 72


def _index_record(record: _RecordPlan) -> bytes:
    data = bytearray(72)
    if record.payload_kind != "system":
        data[0:2] = _be16(1)
        data[2:4] = b"\x00\x00"
        data[4:6] = _be16(record.cluster_count)
        data[6:10] = _be32(record.byte_count)
        data[0x0A:0x0E] = _be32(record.cluster_offset)
        data[0x0E:0x12] = _be32(record.cluster_count)
        data[0x12:0x16] = _be32(record.byte_count)
    data[0x3A:0x42] = b"\xff" * 8
    if record.payload_kind == "directory":
        data[0x42:0x46] = b"\x94dir"
        data[0x46:0x48] = _be16(record.directory_tail_value)
    elif record.payload_kind == "object":
        data[0x42:0x48] = b"\x9e\x00\x00\x00\x00\x01"
    elif record.payload_kind in {"hidden", "system"}:
        data[0x42:0x48] = b"\x94\x00\x00\x00\x00\x01"
    return bytes(data)


def _mark_extent(
    bitmap: bytearray, cluster_offset: int, cluster_count: int, cluster_count_limit: int
) -> None:
    sfs_allocation.add_extent_to_bitmap(
        bitmap,
        cluster_offset=cluster_offset,
        cluster_count=cluster_count,
        cluster_count_limit=cluster_count_limit,
    )


def _write_at(handle: BinaryIO, offset: int, data: bytes) -> None:
    handle.seek(offset)
    handle.write(data)


__all__ = [
    "HdsImageBuilder",
    "HdsWriteResult",
    "MAX_IMAGE_SIZE_BYTES",
    "MAX_PARTITION_SIZE_BYTES",
    "SampleBankRef",
    "WaveformRef",
    "WrittenObjectRef",
]
