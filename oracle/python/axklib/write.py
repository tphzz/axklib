"""Generated Yamaha SFS/HDS image writer.

The writer starts with a narrow generated-image contract: fresh
hard-disk image, SFS partitions, volumes, current SMPL waveforms, direct
single-member or equal-format stereo SBNK sample banks, and a hardware-proven
one-to-three-member SBAC/PROG topology. It does not patch existing images.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import BinaryIO

from axklib.audio.importing import SamplerAudio, import_sampler_audio
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
MIN_PARTITION_SECTORS = 2045
MAX_PARTITION_SLOT_SECTORS = MAX_PARTITION_SLOT_BYTES // SECTOR_SIZE - 1
MAX_PARTITION_SECTORS = MAX_PARTITION_SLOT_SECTORS - 1
# Preservation-only compatibility residue shared by generated hard-disk images.
# These contiguous words are not disk geometry fields.
SUPERBLOCK_FORMATTER_RESIDUE_OFFSET = 0x80
SUPERBLOCK_FORMATTER_RESIDUE_WORDS = (
    0xA1E00152,
    0xA22C0000,
    0x00000017,
    0x09100000,
    0x00000017,
    0x09100000,
    0x01000152,
)
# The low three bits carry the bounded partition sequence; the remaining bits
# are compatibility values.
FORMATTER_TRANSFER_TOKEN_SEQUENCE_MASK = 0x7
GENERAL_TRANSFER_TOKEN_BASE = 0xAB432100
PARTITION_HEADER_DYNAMIC_BASE_WORD = 0x0152A3FC
PARTITION_HEADER_DYNAMIC_WORD_STEP = 0x11
OBJECT_MAGIC = b"FSFSDEV3SPLX"
SMPL_COMPACT_HEADER_SIZE = 0xAC
SMPL_OBJECT_HEADER_SIZE = 0x200
SBNK_OBJECT_SIZE = 0x188
SBNK_PAYLOAD_SIZE = sbnk_contract.CURRENT_SBNK_CONTRACT_PAYLOAD_SIZE
SBAC_PAYLOAD_SIZE = 0x210
PROG_PAYLOAD_SIZE = 0x390
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
class SampleBankGroupRef:
    """Reference returned by :meth:`VolumeBuilder.add_sample_bank_group`."""

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
class WrittenPartitionLayout:
    index: int
    name: str
    start_sector: int
    sector_count: int
    cluster_count: int
    first_payload_cluster: int
    allocated_cluster_count: int
    free_cluster_count: int
    free_bytes: int
    sampler_visible_free_kib: int


@dataclass(frozen=True)
class HdsWriteResult:
    path: Path
    size_bytes: int
    partitions: int
    objects: tuple[WrittenObjectRef, ...]
    partition_layouts: tuple[WrittenPartitionLayout, ...] = ()
    unused_tail_sectors: int = 0
    warnings: tuple[str, ...] = ()
    audio_imports: tuple[AudioImportReport, ...] = ()


@dataclass(frozen=True)
class AudioImportReport:
    source_path: Path
    partition_index: int
    volume_name: str
    waveform_names: tuple[str, ...]
    source_format: str
    source_subtype: str
    source_channels: int
    source_sample_rate: int
    output_sample_rate: int
    output_frames: int
    resampled: bool
    quantized: bool
    split_stereo: bool
    clipped_samples: int


@dataclass
class _WaveformSpec:
    key: str
    name: str
    path: Path
    root_key: int
    link_id: int
    expected_channels: int
    source_channel: int
    target_sample_rate: int | None


@dataclass
class _SampleBankSpec:
    key: str
    name: str
    waveform_key: str
    right_waveform_key: str | None
    root_key: int
    key_low: int
    key_high: int
    level: int


@dataclass
class _SampleBankGroupSpec:
    key: str
    name: str
    member_bank_keys: tuple[str, ...]


@dataclass(frozen=True)
class _ProgramAssignmentSpec:
    target_key: str
    target_kind: str
    receive_channel: int


@dataclass
class _ProgramSpec:
    number: int
    assignments: list[_ProgramAssignmentSpec] = field(default_factory=list)


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
    audio_imports: list[AudioImportReport] = field(default_factory=list)


@dataclass
class VolumeBuilder:
    name: str
    _partition: PartitionBuilder
    _waveforms: list[_WaveformSpec] = field(default_factory=list)
    _sample_banks: list[_SampleBankSpec] = field(default_factory=list)
    _sample_bank_groups: list[_SampleBankGroupSpec] = field(default_factory=list)
    _programs: list[_ProgramSpec] = field(default_factory=list)

    def _append_waveform(
        self,
        *,
        key: str,
        name: str,
        path: str | Path,
        root_key: int,
        expected_channels: int,
        source_channel: int,
        target_sample_rate: int | None,
    ) -> WaveformRef:
        link_id = SMPL_LINK_ID_BASE + len(self._waveforms) * 0x100
        self._waveforms.append(
            _WaveformSpec(
                key=key,
                name=name,
                path=Path(path),
                root_key=root_key,
                link_id=link_id,
                expected_channels=expected_channels,
                source_channel=source_channel,
                target_sample_rate=target_sample_rate,
            )
        )
        return WaveformRef(key)

    def add_waveform_from_audio(
        self,
        *,
        name: str,
        path: str | Path,
        root_key: int,
        target_sample_rate: int | None = None,
    ) -> WaveformRef:
        """Add one mono waveform from a libsndfile-compatible audio source."""
        _validate_midi_key(root_key, "root_key")
        key = _unique_key(name, {item.key for item in self._waveforms}, "waveform")
        return self._append_waveform(
            key=key,
            name=name,
            path=path,
            root_key=root_key,
            expected_channels=1,
            source_channel=0,
            target_sample_rate=target_sample_rate,
        )

    def add_waveform_from_wav(
        self,
        *,
        name: str,
        path: str | Path,
        root_key: int,
        target_sample_rate: int | None = None,
    ) -> WaveformRef:
        """Add one mono WAV waveform through the sampler audio importer."""
        return self.add_waveform_from_audio(
            name=name,
            path=path,
            root_key=root_key,
            target_sample_rate=target_sample_rate,
        )

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
                right_waveform_key=None,
                root_key=root_key,
                key_low=key_low,
                key_high=key_high,
                level=level,
            )
        )
        return SampleBankRef(key)

    def add_stereo_sample_bank(
        self,
        *,
        name: str,
        left_waveform: WaveformRef,
        right_waveform: WaveformRef,
        root_key: int,
        key_low: int,
        key_high: int,
        level: int = 127,
    ) -> SampleBankRef:
        """Add a two-member stereo bank from equal-format mono waveforms."""
        _validate_midi_key(root_key, "root_key")
        _validate_midi_key(key_low, "key_low")
        _validate_midi_key(key_high, "key_high")
        if key_high < key_low:
            raise ValueError("key_high must be greater than or equal to key_low")
        if level < 0 or level > 127:
            raise ValueError("level must be in MIDI range 0..127")
        known_waveforms = {item.key for item in self._waveforms}
        if left_waveform.key not in known_waveforms:
            raise ValueError(f"unknown left waveform reference: {left_waveform.key}")
        if right_waveform.key not in known_waveforms:
            raise ValueError(f"unknown right waveform reference: {right_waveform.key}")
        if left_waveform.key == right_waveform.key:
            raise ValueError("stereo sample bank requires distinct left and right waveforms")
        key = _unique_key(name, {item.key for item in self._sample_banks}, "sample bank")
        self._sample_banks.append(
            _SampleBankSpec(
                key=key,
                name=name,
                waveform_key=left_waveform.key,
                right_waveform_key=right_waveform.key,
                root_key=root_key,
                key_low=key_low,
                key_high=key_high,
                level=level,
            )
        )
        return SampleBankRef(key)

    def add_stereo_sample_bank_from_audio(
        self,
        *,
        name: str,
        path: str | Path,
        root_key: int,
        key_low: int,
        key_high: int,
        level: int = 127,
        left_waveform_name: str | None = None,
        right_waveform_name: str | None = None,
        target_sample_rate: int | None = None,
    ) -> SampleBankRef:
        """Create a stereo SBNK and two mono SMPL members from one audio source."""
        _validate_midi_key(root_key, "root_key")
        _validate_midi_key(key_low, "key_low")
        _validate_midi_key(key_high, "key_high")
        if key_high < key_low:
            raise ValueError("key_high must be greater than or equal to key_low")
        if level < 0 or level > 127:
            raise ValueError("level must be in MIDI range 0..127")
        bank_key = _unique_key(name, {item.key for item in self._sample_banks}, "sample bank")
        base_name = name.encode("ascii")[:14].decode("ascii")
        left_name = left_waveform_name or f"{base_name}-L"
        right_name = right_waveform_name or f"{base_name}-R"
        existing_waveforms = {item.key for item in self._waveforms}
        left_key = _unique_key(left_name, existing_waveforms, "waveform")
        right_key = _unique_key(right_name, existing_waveforms | {left_key}, "waveform")
        left = self._append_waveform(
            key=left_key,
            name=left_name,
            path=path,
            root_key=root_key,
            expected_channels=2,
            source_channel=0,
            target_sample_rate=target_sample_rate,
        )
        right = self._append_waveform(
            key=right_key,
            name=right_name,
            path=path,
            root_key=root_key,
            expected_channels=2,
            source_channel=1,
            target_sample_rate=target_sample_rate,
        )
        self._sample_banks.append(
            _SampleBankSpec(
                key=bank_key,
                name=name,
                waveform_key=left.key,
                right_waveform_key=right.key,
                root_key=root_key,
                key_low=key_low,
                key_high=key_high,
                level=level,
            )
        )
        return SampleBankRef(bank_key)

    def add_sample_bank_group(
        self,
        *,
        name: str,
        member: SampleBankRef | None = None,
        members: Sequence[SampleBankRef] | None = None,
    ) -> SampleBankGroupRef:
        """Add a sample-bank group with one through three mono members."""
        if (member is None) == (members is None):
            raise ValueError("provide exactly one of member or members")
        requested = (member,) if member is not None else tuple(members or ())
        if not 1 <= len(requested) <= 3:
            raise ValueError("current SBAC writer profile supports 1..3 members")
        member_keys = tuple(item.key for item in requested)
        if len(member_keys) != len(set(member_keys)):
            raise ValueError("sample bank group members must be distinct")
        known_banks = {item.key for item in self._sample_banks}
        for member_key in member_keys:
            if member_key not in known_banks:
                raise ValueError(f"unknown sample bank reference: {member_key}")
            if any(member_key in item.member_bank_keys for item in self._sample_bank_groups):
                raise ValueError(f"sample bank is already grouped: {member_key!r}")
        key = _unique_key(
            name,
            {item.key for item in self._sample_bank_groups},
            "sample bank group",
        )
        self._sample_bank_groups.append(
            _SampleBankGroupSpec(key=key, name=name, member_bank_keys=member_keys)
        )
        return SampleBankGroupRef(key)

    def add_program(self, *, number: int = 1) -> ProgramBuilder:
        """Add one Program in the supported 001 through 128 range."""
        if number < 1 or number > 128:
            raise ValueError("Program number must be in range 1..128")
        if any(item.number == number for item in self._programs):
            raise ValueError(f"duplicate Program number: {number:03d}")
        spec = _ProgramSpec(number=number)
        self._programs.append(spec)
        return ProgramBuilder(_volume=self, _spec=spec)


@dataclass
class ProgramBuilder:
    """Build the two assignments in the hardware-proven Program 001 profile."""

    _volume: VolumeBuilder
    _spec: _ProgramSpec

    def assign_sample_bank_group(
        self,
        target: SampleBankGroupRef,
        *,
        receive_channel: int,
    ) -> None:
        _validate_receive_channel(receive_channel)
        if target.key not in {item.key for item in self._volume._sample_bank_groups}:
            raise ValueError(f"unknown sample bank group reference: {target.key}")
        self._spec.assignments.append(
            _ProgramAssignmentSpec(
                target_key=target.key,
                target_kind="SBAC",
                receive_channel=receive_channel,
            )
        )

    def assign_sample_bank(
        self,
        target: SampleBankRef,
        *,
        receive_channel: int,
    ) -> None:
        _validate_receive_channel(receive_channel)
        if target.key not in {item.key for item in self._volume._sample_banks}:
            raise ValueError(f"unknown sample bank reference: {target.key}")
        self._spec.assignments.append(
            _ProgramAssignmentSpec(
                target_key=target.key,
                target_kind="SBNK",
                receive_channel=receive_channel,
            )
        )


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
        _validate_sbac_prog_profile(self)
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
        audio_imports: list[AudioImportReport] = []
        for plan in plans:
            writes.extend(_partition_writes(plan))
            written_objects.extend(_object_refs(plan))
            audio_imports.extend(plan.audio_imports)
        with output.open("wb") as handle:
            handle.truncate(self.size_bytes)
            for offset, data in sorted(writes, key=lambda item: item[0]):
                _write_at(handle, offset, data)
        return HdsWriteResult(
            path=output,
            size_bytes=self.size_bytes,
            partitions=len(plans),
            objects=tuple(written_objects),
            partition_layouts=tuple(_written_partition_layout(plan) for plan in plans),
            unused_tail_sectors=_unused_tail_sectors(self.size_bytes, plans),
            warnings=tuple(
                f"audio source {report.source_path} clipped {report.clipped_samples} samples "
                "during sampler conversion"
                for report in audio_imports
                if report.clipped_samples
            ),
            audio_imports=tuple(audio_imports),
        )


def _validate_midi_key(value: int, name: str) -> None:
    if value < 0 or value > 127:
        raise ValueError(f"{name} must be in MIDI range 0..127")


def _validate_receive_channel(value: int) -> None:
    if value < 1 or value > 16:
        raise ValueError("receive_channel must be in MIDI channel range 1..16")


def _validate_sbac_prog_profile(builder: HdsImageBuilder) -> None:
    for partition in builder._partitions:
        configured = [
            volume
            for volume in partition._volumes
            if volume._sample_bank_groups or volume._programs
        ]
        for volume in configured:
            _validate_sbac_prog_volume(volume)


def _validate_sbac_prog_volume(volume: VolumeBuilder) -> None:
    if not volume._sample_bank_groups or not volume._programs:
        raise ValueError(
            "current SBAC/PROG writer profile requires both sample bank groups and Programs"
        )
    if len(volume._sample_bank_groups) != len(volume._programs):
        raise ValueError("current SBAC/PROG writer profile requires one assigned group per Program")
    groups = {group.key: group for group in volume._sample_bank_groups}
    banks = {bank.key: bank for bank in volume._sample_banks}
    assigned_groups: set[str] = set()
    assigned_direct: set[str] = set()
    for program in volume._programs:
        if len(program.assignments) != 2:
            raise ValueError(f"Program {program.number:03d} must contain exactly two assignments")
        grouped, direct = program.assignments
        group = groups.get(grouped.target_key)
        if grouped.target_kind != "SBAC" or group is None or grouped.receive_channel != 1:
            raise ValueError(
                f"Program {program.number:03d} first assignment must target a sample bank "
                "group on receive channel 1"
            )
        if grouped.target_key in assigned_groups:
            raise ValueError(
                f"sample bank group is assigned more than once: {grouped.target_key!r}"
            )
        if direct.target_kind != "SBNK" or direct.receive_channel != 2:
            raise ValueError(
                f"Program {program.number:03d} second assignment must target a direct sample "
                "bank on receive channel 2"
            )
        if direct.target_key in assigned_direct:
            raise ValueError(
                f"direct sample bank is assigned more than once: {direct.target_key!r}"
            )
        members = [banks[member_key] for member_key in group.member_bank_keys]
        if direct.target_key in group.member_bank_keys:
            raise ValueError("direct Program control must differ from the grouped members")
        direct_bank = banks.get(direct.target_key)
        if direct_bank is None:
            raise ValueError(f"unknown direct sample bank reference: {direct.target_key}")
        if any(member.right_waveform_key is not None for member in members) or (
            direct_bank.right_waveform_key is not None
        ):
            raise ValueError("current SBAC/PROG writer profile supports mono sample banks only")
        if len(members) == 1:
            comparable_fields = (
                "waveform_key",
                "root_key",
                "key_low",
                "key_high",
                "level",
            )
            if any(
                getattr(members[0], name) != getattr(direct_bank, name)
                for name in comparable_fields
            ):
                raise ValueError(
                    "one-member group and direct control must use matching waveform and parameters"
                )
        assigned_groups.add(grouped.target_key)
        assigned_direct.add(direct.target_key)
    if assigned_groups != set(groups):
        raise ValueError("every sample bank group must be assigned to exactly one Program")


def _unique_key(name: str, existing: set[str], label: str) -> str:
    if not name:
        raise ValueError(f"{label} name must not be empty")
    _ascii_field(name, 16)
    key = name
    if key in existing:
        raise ValueError(f"duplicate {label} name: {name!r}")
    return key


def _layout_partitions(
    size_bytes: int,
    partitions: list[PartitionBuilder],
) -> list[_PartitionPlan]:
    partition_count = len(partitions)
    start_sector, slot_span, sector_count = _general_partition_slot_geometry(
        size_bytes, partition_count
    )
    return [
        _make_partition_plan(
            partition,
            index=index,
            start_sector=start_sector + index * slot_span,
            sector_count=sector_count,
        )
        for index, partition in enumerate(partitions)
    ]


def _general_partition_slot_geometry(size_bytes: int, partition_count: int) -> tuple[int, int, int]:
    """Return equal partition-slot geometry within the image and slot caps."""
    slot_span = (size_bytes // SECTOR_SIZE - (PARTITION_START_SECTOR - 1)) // partition_count
    slot_span = min(slot_span, MAX_PARTITION_SLOT_SECTORS)
    sector_count = slot_span - 1
    if sector_count < MIN_PARTITION_SECTORS:
        raise ValueError(
            "partition slots are too small for the generated SFS layout: "
            f"size_bytes={size_bytes} partitions={partition_count} "
            f"sector_count={sector_count} minimum={MIN_PARTITION_SECTORS}"
        )
    return PARTITION_START_SECTOR, slot_span, sector_count


def _make_partition_plan(
    partition: PartitionBuilder,
    *,
    index: int,
    start_sector: int,
    sector_count: int,
) -> _PartitionPlan:
    if sector_count > MAX_PARTITION_SECTORS:
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
    directory_dot_marker = plan.index * 0x78 if _uses_multi_partition_metadata(plan) else 0
    next_id = SFS_ERROR_LOG_ID + 1
    for volume in plan.builder._volumes:
        volume_id = next_id
        next_id += 1
        category_ids = {
            name: next_id + index for index, name in enumerate(STANDARD_VOLUME_CATEGORY_NAMES)
        }
        next_id += len(STANDARD_VOLUME_CATEGORY_NAMES)
        root_entries.append((volume.name, volume_id, 16))

        loaded, import_reports = _load_volume_waveforms(
            volume._waveforms,
            partition_index=plan.index,
            volume_name=volume.name,
        )
        plan.audio_imports.extend(import_reports)
        loaded_by_key = {item.spec.key: item for item in loaded}
        smpl_entries: list[tuple[str, int, int | None]] = []
        sbnk_entries: list[tuple[str, int, int | None]] = []
        sbac_entries: list[tuple[str, int, int | None]] = []
        prog_entries: list[tuple[str, int, int | None]] = []
        object_payloads: list[tuple[int, bytes, str, str, str]] = []
        grouped_bank_keys = {
            member_key
            for group in volume._sample_bank_groups
            for member_key in group.member_bank_keys
        }
        directly_linked_programs: dict[str, list[int]] = {}
        for program in volume._programs:
            for assignment in program.assignments:
                if assignment.target_kind == "SBNK":
                    directly_linked_programs.setdefault(assignment.target_key, []).append(
                        program.number
                    )

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
            left_waveform = loaded_by_key[bank.waveform_key]
            right_waveform = (
                loaded_by_key[bank.right_waveform_key]
                if bank.right_waveform_key is not None
                else None
            )
            if right_waveform is not None:
                if left_waveform.sample_rate != right_waveform.sample_rate:
                    raise ValueError(
                        f"stereo sample bank {bank.name!r} requires matching sample rates: "
                        f"left={left_waveform.sample_rate} right={right_waveform.sample_rate}"
                    )
                if left_waveform.frames != right_waveform.frames:
                    raise ValueError(
                        f"stereo sample bank {bank.name!r} requires matching logical frame counts: "
                        f"left={left_waveform.frames} right={right_waveform.frames}"
                    )
            sbnk_id = next_id
            next_id += 1
            sbnk_entries.append((bank.name, sbnk_id, 16))
            object_payloads.append(
                (
                    sbnk_id,
                    _serialize_sbnk(
                        bank,
                        left_waveform,
                        right_waveform=right_waveform,
                        inactive_right_policy=plan.builder._image._sbnk_single_member_inactive_right_policy,
                        sample_bank_member=bank.key in grouped_bank_keys,
                        linked_program_numbers=directly_linked_programs.get(bank.key, []),
                    ),
                    "object",
                    "SBNK",
                    bank.name,
                )
            )

        banks_by_key = {bank.key: bank for bank in volume._sample_banks}
        for group in volume._sample_bank_groups:
            sbac_id = next_id
            next_id += 1
            sbac_entries.append((group.name, sbac_id, 16))
            object_payloads.append(
                (
                    sbac_id,
                    _serialize_sbac(group, banks_by_key),
                    "object",
                    "SBAC",
                    group.name,
                )
            )

        bank_names = {bank.key: bank.name for bank in volume._sample_banks}
        group_names = {group.key: group.name for group in volume._sample_bank_groups}
        for program in volume._programs:
            prog_id = next_id
            next_id += 1
            object_name = f"{program.number:03d}"
            prog_entries.append((object_name, prog_id, 16))
            object_payloads.append(
                (
                    prog_id,
                    _serialize_prog(program, bank_names=bank_names, group_names=group_names),
                    "object",
                    "PROG",
                    object_name,
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
        records.append(
            _RecordPlan(
                category_ids["SBAC"],
                _directory_payload(
                    category_ids["SBAC"],
                    volume_id,
                    sbac_entries,
                    dot_partition_marker=directory_dot_marker,
                ),
                "directory",
                directory_tail_value=2,
            )
        )
        records.append(
            _RecordPlan(
                category_ids["SEQU"],
                _directory_payload(
                    category_ids["SEQU"],
                    volume_id,
                    [],
                    dot_partition_marker=directory_dot_marker,
                ),
                "directory",
                directory_tail_value=2,
            )
        )
        records.append(
            _RecordPlan(
                category_ids["PROG"],
                _directory_payload(
                    category_ids["PROG"],
                    volume_id,
                    prog_entries,
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


def _load_volume_waveforms(
    specs: list[_WaveformSpec],
    *,
    partition_index: int,
    volume_name: str,
) -> tuple[list[_LoadedWaveform], list[AudioImportReport]]:
    cache: dict[tuple[Path, int, int | None], SamplerAudio] = {}
    names_by_source: dict[tuple[Path, int, int | None], list[str]] = {}
    loaded: list[_LoadedWaveform] = []
    for spec in specs:
        source_key = (spec.path, spec.expected_channels, spec.target_sample_rate)
        audio = cache.get(source_key)
        if audio is None:
            audio = import_sampler_audio(
                spec.path,
                expected_channels=spec.expected_channels,
                target_sample_rate=spec.target_sample_rate,
            )
            cache[source_key] = audio
        names_by_source.setdefault(source_key, []).append(spec.name)
        pcm = audio.pcm_channels[spec.source_channel]
        loaded.append(
            _LoadedWaveform(
                spec=spec,
                sample_rate=audio.output_sample_rate,
                frames=audio.output_frames,
                pcm_little_endian=pcm,
                stored_big_endian=_stored_smpl_pcm(_swap_16bit_words(pcm)),
            )
        )
    reports = [
        AudioImportReport(
            source_path=audio.source_path,
            partition_index=partition_index,
            volume_name=volume_name,
            waveform_names=tuple(names_by_source[source_key]),
            source_format=audio.source_format,
            source_subtype=audio.source_subtype,
            source_channels=audio.source_channels,
            source_sample_rate=audio.source_sample_rate,
            output_sample_rate=audio.output_sample_rate,
            output_frames=audio.output_frames,
            resampled=audio.resampled,
            quantized=audio.quantized,
            split_stereo=audio.source_channels == 2,
            clipped_samples=audio.clipped_samples,
        )
        for source_key, audio in cache.items()
    ]
    return loaded, reports


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
    right_waveform: _LoadedWaveform | None,
    inactive_right_policy: str,
    sample_bank_member: bool,
    linked_program_numbers: Sequence[int],
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
    if right_waveform is None:
        payload = sbnk_contract.serialize_current_single_member_sbnk_payload(
            bank_name=bank.name,
            left=left,
            inactive_right_policy=inactive_right_policy,
            key_range_high_0x0e2=bank.key_high,
            key_range_low_0x0e3=bank.key_low,
            sample_level_0x116=bank.level,
        )
    else:
        right = sbnk_contract.CurrentSbnkMemberSpec(
            sample_name=right_waveform.spec.name,
            smpl_link_id_0x078=right_waveform.spec.link_id,
            root_key_0x0d6=bank.root_key,
            sample_rate_0x0d8=right_waveform.sample_rate,
            fine_tune_cents_0x0dc=DEFAULT_FINE_TUNE_CENTS,
            wave_length_frames_0x0f0=right_waveform.frames,
            loop_start_frame_0x0f8=0,
            loop_length_frames_0x100=right_waveform.frames,
        )
        payload = sbnk_contract.serialize_current_two_member_sbnk_payload(
            bank_name=bank.name,
            left=left,
            right=right,
            key_range_high_0x0e2=bank.key_high,
            key_range_low_0x0e3=bank.key_low,
            sample_level_0x116=bank.level,
        )
    data = bytearray(payload[:SBNK_OBJECT_SIZE])
    if sample_bank_member:
        data[0x0D0] |= 0x01
    for program_number in linked_program_numbers:
        if program_number < 1 or program_number > 128:
            raise ValueError(f"linked Program number is out of range: {program_number}")
        word_offset = 0x0C0 + ((program_number - 1) // 32) * 4
        word = int.from_bytes(data[word_offset : word_offset + 4], "big")
        word |= 1 << ((program_number - 1) % 32)
        _put_be32(data, word_offset, word)
    return bytes(data)


def _serialize_sbac(
    group: _SampleBankGroupSpec,
    banks_by_key: dict[str, _SampleBankSpec],
) -> bytes:
    payload = bytearray(SBAC_PAYLOAD_SIZE)
    payload[0 : len(OBJECT_MAGIC)] = OBJECT_MAGIC
    payload[0x0C:0x10] = b"SBAC"
    _put_be32(payload, 0x14, 4)
    _put_be32(payload, 0x18, 0x1BC)
    _put_be32(payload, 0x1C, 0x1E0)
    payload[0x30:0x32] = b"\x11\x0c"
    payload[0x32:0x42] = _ascii_field(group.name, 16)
    payload[0x144] = len(group.member_bank_keys)
    for index, member_key in enumerate(group.member_bank_keys):
        row_offset = 0x14C + index * 0x14
        payload[row_offset : row_offset + 0x10] = _ascii_field(banks_by_key[member_key].name, 16)
    return bytes(payload)


def _serialize_prog(
    program: _ProgramSpec,
    *,
    bank_names: dict[str, str],
    group_names: dict[str, str],
) -> bytes:
    payload = bytearray(PROG_PAYLOAD_SIZE)
    object_name = f"{program.number:03d}"
    payload[0 : len(OBJECT_MAGIC)] = OBJECT_MAGIC
    payload[0x0C:0x10] = b"PROG"
    _put_be32(payload, 0x14, 4)
    _put_be32(payload, 0x18, 0x2B0)
    _put_be32(payload, 0x1C, 0x360)
    payload[0x30:0x32] = b"\x14\x0c"
    payload[0x32:0x42] = _ascii_field(object_name, 16)
    payload[0x78:0x80] = _ascii_field(f"Pgm {object_name}", 8)
    payload[0x80:0x98] = bytes.fromhex("0005ffff000000014000407f000000fe005a5a2778ff0002")
    for index, assignment in enumerate(program.assignments):
        offset = 0x120 + index * 0x38
        target_name = (
            group_names[assignment.target_key]
            if assignment.target_kind == "SBAC"
            else bank_names[assignment.target_key]
        )
        payload[offset : offset + 0x38] = _serialize_prog_assignment(
            target_name=target_name,
            target_kind=assignment.target_kind,
            receive_channel=assignment.receive_channel,
        )
    return bytes(payload)


def _serialize_prog_assignment(
    *,
    target_name: str,
    target_kind: str,
    receive_channel: int,
) -> bytes:
    row = bytearray(0x38)
    row[0x00:0x10] = _ascii_field(target_name, 16)
    row[0x14] = 0x11 if target_kind == "SBAC" else 0x10
    row[0x15] = receive_channel - 1
    row[0x1D] = 0xFF
    row[0x1E] = 0x7F
    row[0x21] = 0x7F
    row[0x23] = 0xFF
    row[0x24] = 0xFF
    row[0x28] = 0xFF
    row[0x2D] = 0xFF
    row[0x30] = 0xFF
    row[0x33] = 0x01
    return bytes(row)


def _superblock_writes(size_bytes: int, plans: list[_PartitionPlan]) -> list[tuple[int, bytes]]:
    block = _build_superblock(size_bytes, plans)
    writes = [
        (0, block),
        (SECTOR_SIZE, block),
        (SECTOR_SIZE * 2, _sector2_metadata()),
    ]
    for plan in plans[1:]:
        writes.append(((plan.start_sector - 1) * SECTOR_SIZE, _sector2_metadata(plan.index)))
    return writes


def _build_superblock(size_bytes: int, plans: list[_PartitionPlan]) -> bytes:
    block = bytearray(SECTOR_SIZE)
    block[0 : len(b"YAMAHA_dev3")] = b"YAMAHA_dev3"
    _write_superblock_formatter_residue(block)
    _put_be32(block, 0x9C, SECTOR_SIZE)
    _put_be32(block, 0xA0, size_bytes // SECTOR_SIZE)
    _write_partition_table(block, plans)
    return bytes(block)


def _write_superblock_formatter_residue(block: bytearray) -> None:
    for index, value in enumerate(SUPERBLOCK_FORMATTER_RESIDUE_WORDS):
        _put_be32(block, SUPERBLOCK_FORMATTER_RESIDUE_OFFSET + index * 4, value)


def _write_partition_table(block: bytearray, plans: list[_PartitionPlan]) -> None:
    for plan in plans:
        rel = 0x0A8 + plan.index * 8
        _put_be32(block, rel, plan.start_sector)
        _put_be32(block, rel + 4, plan.sector_count)


def _sector2_metadata(sequence: int = 0) -> bytes:
    if sequence < 0 or sequence >= PARTITION_ENTRY_COUNT:
        raise ValueError("partition metadata sequence is out of range")
    metadata = bytearray(SECTOR_SIZE)
    metadata[0:8] = _formatter_transfer_token(GENERAL_TRANSFER_TOKEN_BASE, sequence)
    return bytes(metadata)


def _formatter_transfer_token(base: int, sequence: int) -> bytes:
    if sequence < 0 or sequence > FORMATTER_TRANSFER_TOKEN_SEQUENCE_MASK:
        raise ValueError("formatter transfer token sequence must be between 0 and 7")
    if base & FORMATTER_TRANSFER_TOKEN_SEQUENCE_MASK:
        raise ValueError("formatter transfer token base must reserve its low three bits")
    return f"{base | sequence:08x}".encode("ascii")


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
    _apply_partition_compatibility_metadata(header, plan)
    return bytes(header)


def _partition_header_name(plan: _PartitionPlan) -> bytes:
    if plan.index > 0 and len(plan.builder._image._partitions) > 1:
        return _ascii_field(plan.builder.name, 15) + str(plan.index).encode("ascii")
    return _ascii_field(plan.builder.name, 16)


def _apply_partition_compatibility_metadata(header: bytearray, plan: _PartitionPlan) -> None:
    if _uses_two_partition_index_byte(plan):
        header[0xAF] = plan.index
    for offset, value in _partition_header_tail_values(plan).items():
        _put_be32(header, offset, value)


def _uses_two_partition_index_byte(plan: _PartitionPlan) -> bool:
    return len(plan.builder._image._partitions) == 2


def _uses_multi_partition_metadata(plan: _PartitionPlan) -> bool:
    return len(plan.builder._image._partitions) >= 3


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
        0x194: _partition_header_count_marker(plan),
        0x1A8: plan.index,
    }
    return values


def _partition_header_dynamic_word(partition_index: int) -> int:
    return PARTITION_HEADER_DYNAMIC_BASE_WORD + partition_index * PARTITION_HEADER_DYNAMIC_WORD_STEP


def _partition_header_first_object_hint(partition_index: int) -> int:
    # Compatibility value varies after the first partition; it is not an allocation rule.
    return 0x016F5BF0 if partition_index == 0 else 0x015D6CC0


def _partition_header_image_sector_marker(plan: _PartitionPlan) -> int:
    total_sectors = plan.builder._image.size_bytes // SECTOR_SIZE
    if _uses_multi_partition_metadata(plan):
        return total_sectors - (len(plan.builder._image._partitions) - 2)
    return total_sectors


def _partition_header_count_marker(plan: _PartitionPlan) -> int:
    if _uses_multi_partition_metadata(plan):
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


def _written_partition_layout(plan: _PartitionPlan) -> WrittenPartitionLayout:
    allocated_cluster_count = sum(record.cluster_count for record in plan.records)
    free_space = sfs_allocation.calculate_sfs_free_space(
        cluster_count=plan.cluster_count,
        first_payload_cluster=plan.first_payload_cluster,
        allocated_cluster_count=allocated_cluster_count,
        cluster_size_bytes=CLUSTER_SIZE,
    )
    return WrittenPartitionLayout(
        index=plan.index,
        name=plan.builder.name,
        start_sector=plan.start_sector,
        sector_count=plan.sector_count,
        cluster_count=plan.cluster_count,
        first_payload_cluster=plan.first_payload_cluster,
        allocated_cluster_count=allocated_cluster_count,
        free_cluster_count=free_space.free_cluster_count,
        free_bytes=free_space.free_bytes,
        sampler_visible_free_kib=free_space.sampler_visible_free_kib,
    )


def _unused_tail_sectors(size_bytes: int, plans: list[_PartitionPlan]) -> int:
    used_end_sector = max(plan.start_sector + plan.sector_count for plan in plans)
    return size_bytes // SECTOR_SIZE - used_end_sector


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
    "AudioImportReport",
    "HdsImageBuilder",
    "HdsWriteResult",
    "MAX_IMAGE_SIZE_BYTES",
    "MAX_PARTITION_SLOT_BYTES",
    "SampleBankRef",
    "WaveformRef",
    "WrittenObjectRef",
    "WrittenPartitionLayout",
]
