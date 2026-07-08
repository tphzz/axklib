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
MAX_PARTITION_SLOT_BYTES = 1_073_741_824
MIN_IMAGE_SIZE_BYTES = 1_048_576
PARTITION_START_SECTOR = 3
PARTITION_HEADER_SIZE = 1024
PARTITION_ENTRY_COUNT = 8
DIRECTORY_INDEX_SPAN_CLUSTERS = 358
PARTITION_HEADER_CAPACITY_VALUE = 200
PARTITION_HEADER_INITIAL_RESERVED_CLUSTER = 2
PARTITION_HEADER_INDEX_CAPACITY_VALUE = 5012
DIRECT_EXTENT_CLUSTER_LIMIT = 0xFFFF
MIN_ALLOCATED_RECORD_CLUSTERS = 2
# Profile constant for current writer profiles. Placement is known at superblock
# +0x80..+0x9b; field semantics are still unresolved.
DISK_DESCRIPTOR_UNRESOLVED_WORDS = (
    (0x80, 0xA1E00152),
    (0x84, 0xA22C0000),
    (0x88, 0x00000017),
    (0x8C, 0x09100000),
    (0x90, 0x00000017),
    (0x94, 0x09100000),
    (0x98, 0x01000152),
)
DEFAULT_DISK_IDENTIFIER = b"ab432100"
SECTOR2_PRIMARY_IDENTIFIER = b"c2b4e600"
SUPPORTED_HARDWARE_METADATA_PARTITION_SECTORS = 524_285
SUPPORTED_TWO_PARTITION_IMAGE_BYTES = 512 * 1024 * 1024
SUPPORTED_TWO_PARTITION_SECTORS = 524_286
SUPPORTED_TWO_PARTITION_STRIDE_SECTORS = 524_287
SUPPORTED_SPARSE_768M_IMAGE_BYTES = 768 * 1024 * 1024
SUPPORTED_SPARSE_768M_PARTITIONS = 3
PARTITION_HEADER_TAIL_OPAQUE_TEMPLATE = bytes.fromhex(
    "000000000000000000001388000002000000000200000000000000000000000000000400000320000017ab4a01529f34001aa5400000000000000002000000c8"
    "0000138800000000000000000000000001529fa000000000000000000000000000000000000000000000000800000400000aa30e01529f780000000000000000"
    "001aa540000000000000138800000400000000000000000000000000000000000000000200000000000000000000000000000000000a9f760152a22400000000"
    "00000000000000000000000000000000000000000000000000000000000000000000000001529fb400000002000000c800000400000000000000000000000000"
)
PARTITION_HEADER_TAIL_VARIABLE_OFFSETS = (
    0x104,
    0x114,
    0x118,
    0x11C,
    0x134,
    0x144,
    0x14C,
    0x154,
    0x158,
    0x15C,
    0x160,
    0x164,
    0x178,
    0x17C,
    0x184,
    0x194,
    0x1A8,
    0x1BC,
    0x1C0,
    0x1C4,
    0x1C8,
    0x1CC,
    0x1D0,
    0x1D4,
    0x1D8,
    0x1DC,
    0x1E0,
)
PARTITION_HEADER_DYNAMIC_BASE_WORD = 0x0152A3FC
PARTITION_HEADER_DYNAMIC_WORD_STEP = 0x11
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
    dot_partition_marker: int = 0,
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
        entry[31] = dot_partition_marker & 0xFF
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
    dot_partition_marker: int = 0,
) -> bytes:
    payload = bytearray()
    payload.extend(
        _directory_entry(
            ".",
            directory_id,
            directory_id=directory_id,
            dot_partition_marker=dot_partition_marker,
        )
    )
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

    def __init__(
        self,
        *,
        size_bytes: int,
        _sbnk_single_member_inactive_right_policy: str = sbnk_contract.CURRENT_SBNK_SINGLE_MEMBER_INACTIVE_RIGHT_POLICY_ZERO,
    ) -> None:
        if (
            _sbnk_single_member_inactive_right_policy
            not in sbnk_contract.CURRENT_SBNK_SINGLE_MEMBER_INACTIVE_RIGHT_POLICIES
        ):
            raise ValueError(
                "unknown SBNK inactive right-slot policy: "
                f"{_sbnk_single_member_inactive_right_policy!r}"
            )
        if size_bytes < MIN_IMAGE_SIZE_BYTES:
            raise ValueError(f"image size must be at least {MIN_IMAGE_SIZE_BYTES} bytes")
        if size_bytes > MAX_IMAGE_SIZE_BYTES:
            raise ValueError(f"image size exceeds A-series cap: {MAX_IMAGE_SIZE_BYTES} bytes")
        if size_bytes % SECTOR_SIZE:
            raise ValueError("image size must be a multiple of 512 bytes")
        self.size_bytes = size_bytes
        self._sbnk_single_member_inactive_right_policy = _sbnk_single_member_inactive_right_policy
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
    max_partition_sectors = MAX_PARTITION_SLOT_BYTES // SECTOR_SIZE
    if partition_count > 1:
        if _is_supported_two_partition_layout(
            size_bytes, partition_count
        ) or _is_supported_sparse_768m_layout(size_bytes, partition_count):
            start_sector, sector_count = _supported_multi_partition_slot_geometry(
                size_bytes, partition_count
            )
            return [
                _make_partition_plan(
                    partition,
                    index=index,
                    start_sector=start_sector + index * (sector_count + 1),
                    sector_count=sector_count,
                )
                for index, partition in enumerate(partitions)
            ]
        raise ValueError(
            "multi-partition writing currently supports only the hardware-proven "
            "512 MiB / two-partition and sparse 768 MiB / three-partition profiles"
        )

    sectors_per_partition = available_sectors
    if sectors_per_partition > max_partition_sectors:
        raise ValueError(
            "partition size would exceed 1 GiB; add more partitions or reduce image size"
        )
    if sectors_per_partition < 2045:
        raise ValueError("partition is too small for the generated SFS layout")
    return [
        _make_partition_plan(
            partitions[0],
            index=0,
            start_sector=PARTITION_START_SECTOR,
            sector_count=sectors_per_partition,
        )
    ]


def _is_supported_two_partition_layout(size_bytes: int, partition_count: int) -> bool:
    return size_bytes == SUPPORTED_TWO_PARTITION_IMAGE_BYTES and partition_count == 2


def _is_supported_sparse_768m_layout(size_bytes: int, partition_count: int) -> bool:
    return (
        size_bytes == SUPPORTED_SPARSE_768M_IMAGE_BYTES
        and partition_count == SUPPORTED_SPARSE_768M_PARTITIONS
    )


def _supported_multi_partition_slot_geometry(
    size_bytes: int, partition_count: int
) -> tuple[int, int]:
    """Return the proven A-series partition slot geometry for supported layouts."""
    slot_span = (size_bytes // SECTOR_SIZE - (PARTITION_START_SECTOR - 1)) // partition_count
    max_slot_span = MAX_PARTITION_SLOT_BYTES // SECTOR_SIZE
    if slot_span > max_slot_span:
        slot_span = max_slot_span
    return PARTITION_START_SECTOR, slot_span - 1


def _make_partition_plan(
    partition: PartitionBuilder,
    *,
    index: int,
    start_sector: int,
    sector_count: int,
) -> _PartitionPlan:
    max_partition_sectors = MAX_PARTITION_SLOT_BYTES // SECTOR_SIZE
    if sector_count > max_partition_sectors:
        raise ValueError("partition size would exceed 1 GiB")
    cluster_count = sector_count // SECTORS_PER_CLUSTER
    bitmap_cluster_count = _ceil_div(_ceil_div(cluster_count, 8), CLUSTER_SIZE)
    bitmap_cluster = 2 + bitmap_cluster_count
    directory_index_cluster = bitmap_cluster + bitmap_cluster_count
    first_payload_cluster = directory_index_cluster + DIRECTORY_INDEX_SPAN_CLUSTERS
    return _PartitionPlan(
        builder=partition,
        index=index,
        start_sector=start_sector,
        sector_count=sector_count,
        cluster_count=cluster_count,
        bitmap_cluster=bitmap_cluster,
        bitmap_cluster_count=bitmap_cluster_count,
        directory_index_cluster=directory_index_cluster,
        first_payload_cluster=first_payload_cluster,
        records=[],
    )


def _build_partition_records(plan: _PartitionPlan) -> None:
    records: list[_RecordPlan] = [
        _RecordPlan(0, b"\x00" * (32 * CLUSTER_SIZE), "hidden"),
        _RecordPlan(SFS_ERROR_LOG_ID, b"", "system"),
    ]
    root_entries: list[tuple[str, int, int | None]] = [
        ("sfserrlog", SFS_ERROR_LOG_ID, None),
        ("sfserram", 0, None),
    ]
    directory_dot_marker = plan.index * 0x78 if _is_sparse_multi_partition_plan(plan) else 0
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
                    _serialize_sbnk(
                        bank,
                        waveform,
                        inactive_right_policy=plan.builder._image._sbnk_single_member_inactive_right_policy,
                    ),
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
                _directory_payload(
                    volume_id,
                    ROOT_DIRECTORY_ID,
                    volume_entries,
                    dot_partition_marker=directory_dot_marker,
                ),
                "directory",
                directory_tail_value=2 + len(volume_entries),
            )
        )
        records.append(
            _RecordPlan(
                category_ids["SMPL"],
                _directory_payload(
                    category_ids["SMPL"],
                    volume_id,
                    smpl_entries,
                    dot_partition_marker=directory_dot_marker,
                ),
                "directory",
                directory_tail_value=2,
            )
        )
        records.append(
            _RecordPlan(
                category_ids["SBNK"],
                _directory_payload(
                    category_ids["SBNK"],
                    volume_id,
                    sbnk_entries,
                    dot_partition_marker=directory_dot_marker,
                ),
                "directory",
                directory_tail_value=2,
            )
        )
        for category_name in ("SBAC", "SEQU", "PROG"):
            records.append(
                _RecordPlan(
                    category_ids[category_name],
                    _directory_payload(
                        category_ids[category_name],
                        volume_id,
                        [],
                        dot_partition_marker=directory_dot_marker,
                    ),
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


def _serialize_sbnk(
    bank: _SampleBankSpec,
    waveform: _LoadedWaveform,
    *,
    inactive_right_policy: str,
) -> bytes:
    left = sbnk_contract.CurrentSbnkMemberSpec(
        sample_name=waveform.spec.name,
        smpl_link_id_0x078=waveform.spec.link_id,
        root_key_0x0d6=bank.root_key,
        sample_rate_0x0d8=waveform.sample_rate,
        fine_tune_cents_0x0dc=DEFAULT_FINE_TUNE_CENTS,
        wave_length_frames_0x0f0=waveform.frames,
        loop_start_frame_0x0f8=0,
        loop_length_frames_0x100=waveform.frames,
    )
    payload = sbnk_contract.serialize_current_single_member_sbnk_payload(
        bank_name=bank.name,
        left=left,
        inactive_right_policy=inactive_right_policy,
        key_range_high_0x0e2=bank.key_high,
        key_range_low_0x0e3=bank.key_low,
        sample_level_0x116=bank.level,
    )
    return payload[:SBNK_OBJECT_SIZE]


def _superblock_writes(size_bytes: int, plans: list[_PartitionPlan]) -> list[tuple[int, bytes]]:
    block = _build_superblock(size_bytes, plans)
    writes = [
        (0, block),
        (SECTOR_SIZE, block),
        (SECTOR_SIZE * 2, _sector2_metadata(plans)),
    ]
    if _is_supported_two_partition_profile(plans):
        writes.append(((plans[1].start_sector - 1) * SECTOR_SIZE, _sector2_metadata(plans, 1)))
    elif _is_supported_sparse_multi_profile(plans):
        for plan in plans[1:]:
            writes.append(
                ((plan.start_sector - 1) * SECTOR_SIZE, _sector2_metadata(plans, plan.index))
            )
    return writes


def _build_superblock(size_bytes: int, plans: list[_PartitionPlan]) -> bytes:
    block = bytearray(SECTOR_SIZE)
    block[0 : len(b"YAMAHA_dev3")] = b"YAMAHA_dev3"
    _write_disk_descriptor(block)
    _put_be32(block, 0x9C, SECTOR_SIZE)
    _put_be32(block, 0xA0, size_bytes // SECTOR_SIZE)
    _write_partition_table(block, plans)
    return bytes(block)


def _write_disk_descriptor(block: bytearray) -> None:
    for offset, value in DISK_DESCRIPTOR_UNRESOLVED_WORDS:
        _put_be32(block, offset, value)


def _write_partition_table(block: bytearray, plans: list[_PartitionPlan]) -> None:
    for plan in plans:
        rel = 0x0A8 + plan.index * 8
        _put_be32(block, rel, plan.start_sector)
        _put_be32(block, rel + 4, plan.sector_count)


def _is_supported_two_partition_profile(plans: list[_PartitionPlan]) -> bool:
    return (
        len(plans) == 2
        and plans[0].start_sector == PARTITION_START_SECTOR
        and plans[1].start_sector == PARTITION_START_SECTOR + SUPPORTED_TWO_PARTITION_STRIDE_SECTORS
        and all(plan.sector_count == SUPPORTED_TWO_PARTITION_SECTORS for plan in plans)
    )


def _is_supported_sparse_multi_profile(plans: list[_PartitionPlan]) -> bool:
    return (
        len(plans) == SUPPORTED_SPARSE_768M_PARTITIONS
        and plans[0].start_sector == PARTITION_START_SECTOR
        and all(
            plan.start_sector
            == PARTITION_START_SECTOR + plan.index * SUPPORTED_TWO_PARTITION_STRIDE_SECTORS
            for plan in plans
        )
        and all(plan.sector_count == SUPPORTED_TWO_PARTITION_SECTORS for plan in plans)
    )


def _sector2_metadata(plans: list[_PartitionPlan], sequence: int = 0) -> bytes:
    if _is_supported_two_partition_profile(plans):
        return _two_partition_sector_metadata(plans, sequence)
    if _is_supported_sparse_multi_profile(plans):
        return _sparse_partition_sector_metadata(sequence)
    return _single_partition_sector_metadata(plans)


def _single_partition_sector_metadata(plans: list[_PartitionPlan]) -> bytes:
    metadata = bytearray(SECTOR_SIZE)
    metadata[0:8] = SECTOR2_PRIMARY_IDENTIFIER
    metadata[9:17] = DEFAULT_DISK_IDENTIFIER
    return bytes(metadata)


def _sparse_partition_sector_metadata(sequence: int) -> bytes:
    if sequence < 0 or sequence >= PARTITION_ENTRY_COUNT:
        raise ValueError("sparse partition metadata sequence is out of range")
    metadata = bytearray(SECTOR_SIZE)
    metadata[0:8] = _sparse_sector_identifier(sequence)
    return bytes(metadata)


def _sparse_sector_identifier(sequence: int) -> bytes:
    return f"ab43210{sequence}".encode("ascii")


def _two_partition_sector_metadata(plans: list[_PartitionPlan], sequence: int) -> bytes:
    if sequence not in {0, 1}:
        raise ValueError("two-partition metadata sequence must be 0 or 1")
    metadata = bytearray(SECTOR_SIZE)
    metadata[0:8] = _two_partition_sector_identifier(sequence)
    metadata[9:17] = SECTOR2_PRIMARY_IDENTIFIER
    return bytes(metadata)


def _two_partition_sector_identifier(sequence: int) -> bytes:
    return f"bb73620{sequence}".encode("ascii")


def _partition_writes(plan: _PartitionPlan) -> list[tuple[int, bytes]]:
    start_offset = plan.start_sector * SECTOR_SIZE
    header = _build_partition_header(plan)
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


def _build_partition_header(plan: _PartitionPlan) -> bytes:
    header = bytearray(PARTITION_HEADER_SIZE)
    header[0 : len(b"YAMAHA_dev3")] = b"YAMAHA_dev3"
    header[0x40:0x50] = _partition_header_name(plan)
    _put_be32(header, 0x80, SECTORS_PER_CLUSTER)
    _put_be32(header, 0x84, PARTITION_HEADER_CAPACITY_VALUE)
    header[0x88:0x90] = b"\xff" * 8
    _put_be32(header, 0x90, plan.cluster_count)
    _put_be32(header, 0x94, SECTORS_PER_CLUSTER)
    _put_be32(header, 0x98, PARTITION_HEADER_INITIAL_RESERVED_CLUSTER)
    _put_be32(header, 0x9C, plan.bitmap_cluster)
    _put_be32(header, 0xA0, PARTITION_HEADER_INDEX_CAPACITY_VALUE)
    _put_be32(header, 0xA4, plan.directory_index_cluster)
    _put_be32(header, 0xA8, DIRECTORY_INDEX_SPAN_CLUSTERS)
    _apply_partition_metadata_profile(header, plan)
    return bytes(header)


def _partition_header_name(plan: _PartitionPlan) -> bytes:
    if (
        plan.sector_count == SUPPORTED_TWO_PARTITION_SECTORS
        and plan.start_sector
        == PARTITION_START_SECTOR + plan.index * SUPPORTED_TWO_PARTITION_STRIDE_SECTORS
        and plan.index > 0
    ):
        return _ascii_field(plan.builder.name, 15) + str(plan.index).encode("ascii")
    return _ascii_field(plan.builder.name, 16)


def _apply_partition_metadata_profile(header: bytearray, plan: _PartitionPlan) -> None:
    if _is_two_partition_plan(plan):
        header[0xAF] = plan.index
    if _has_partition_header_tail_profile(plan):
        header[0x100:0x200] = _partition_header_tail_metadata(plan)


def _has_partition_header_tail_profile(plan: _PartitionPlan) -> bool:
    return (
        _is_single_partition_256m_plan(plan)
        or _is_two_partition_plan(plan)
        or _is_sparse_multi_partition_plan(plan)
    )


def _is_single_partition_256m_plan(plan: _PartitionPlan) -> bool:
    return (
        plan.index == 0
        and plan.sector_count == SUPPORTED_HARDWARE_METADATA_PARTITION_SECTORS
        and len(plan.builder._image._partitions) == 1
    )


def _is_two_partition_plan(plan: _PartitionPlan) -> bool:
    return (
        plan.sector_count == SUPPORTED_TWO_PARTITION_SECTORS
        and plan.start_sector
        == PARTITION_START_SECTOR + plan.index * SUPPORTED_TWO_PARTITION_STRIDE_SECTORS
        and plan.builder._image.size_bytes == SUPPORTED_TWO_PARTITION_IMAGE_BYTES
        and len(plan.builder._image._partitions) == 2
    )


def _is_sparse_multi_partition_plan(plan: _PartitionPlan) -> bool:
    return (
        plan.sector_count == SUPPORTED_TWO_PARTITION_SECTORS
        and plan.start_sector
        == PARTITION_START_SECTOR + plan.index * SUPPORTED_TWO_PARTITION_STRIDE_SECTORS
        and _is_supported_sparse_768m_layout(
            plan.builder._image.size_bytes, len(plan.builder._image._partitions)
        )
    )


def _partition_header_tail_metadata(plan: _PartitionPlan) -> bytes:
    metadata = _partition_header_tail_template(
        PARTITION_HEADER_TAIL_OPAQUE_TEMPLATE,
        dynamic_offsets=PARTITION_HEADER_TAIL_VARIABLE_OFFSETS,
    )
    for offset, value in _partition_header_tail_values(plan).items():
        relative_offset = offset - 0x100
        metadata[relative_offset : relative_offset + 4] = _be32(value)
    return bytes(metadata)


def _partition_header_tail_template(
    template: bytes, *, dynamic_offsets: Sequence[int] = ()
) -> bytearray:
    metadata = bytearray(template)
    for offset in dynamic_offsets:
        relative_offset = offset - 0x100
        metadata[relative_offset : relative_offset + 4] = b"\x00" * 4
    return metadata


def _partition_header_tail_values(plan: _PartitionPlan) -> dict[int, int]:
    dynamic_word = _partition_header_dynamic_word(plan.index)
    values = {
        0x104: plan.start_sector,
        0x114: plan.index * 0x20,
        0x118: plan.start_sector,
        0x11C: plan.sector_count,
        0x134: dynamic_word,
        0x144: _partition_header_first_object_hint(plan.index),
        0x14C: _partition_header_image_sector_marker(plan),
        0x154: plan.index,
        0x158: dynamic_word,
        0x15C: plan.index * 0x20,
        0x160: plan.index * plan.sector_count,
        0x164: dynamic_word,
        0x178: plan.start_sector,
        0x17C: plan.sector_count,
        0x184: dynamic_word,
        0x194: _partition_header_profile_count_marker(plan),
        0x1A8: plan.index,
    }
    return values


def _partition_header_dynamic_word(partition_index: int) -> int:
    return PARTITION_HEADER_DYNAMIC_BASE_WORD + partition_index * PARTITION_HEADER_DYNAMIC_WORD_STEP


def _partition_header_first_object_hint(partition_index: int) -> int:
    # Profile byte branch observed in sampler-formatted images; not a general allocation rule.
    return 0x016F5BF0 if partition_index == 0 else 0x015D6CC0


def _partition_header_image_sector_marker(plan: _PartitionPlan) -> int:
    total_sectors = plan.builder._image.size_bytes // SECTOR_SIZE
    # Sparse profiles use this formula; the public semantic role is not exposed.
    if _is_sparse_multi_partition_plan(plan):
        return total_sectors - 1
    return total_sectors


def _partition_header_profile_count_marker(plan: _PartitionPlan) -> int:
    # Sparse profiles use zero here; the public semantic role is not exposed.
    if _is_sparse_multi_partition_plan(plan):
        return 0
    return len(plan.builder._image._partitions)


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
    "MAX_PARTITION_SLOT_BYTES",
    "SampleBankRef",
    "WaveformRef",
    "WrittenObjectRef",
]
