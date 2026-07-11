"""Versioned JSON manifest support for fresh HDS image creation."""

from __future__ import annotations

import json
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path

from axklib.write import HdsImageBuilder, HdsWriteResult, SampleBankRef, WaveformRef

HDS_BUILD_MANIFEST_SCHEMA_VERSION = "1.0"


@dataclass(frozen=True)
class HdsManifestWaveform:
    id: str
    name: str
    path: Path
    root_key: int
    target_sample_rate: int | None


@dataclass(frozen=True)
class HdsManifestSampleBank:
    name: str
    waveform_id: str | None
    right_waveform_id: str | None
    interleaved_audio_path: Path | None
    left_waveform_name: str | None
    right_waveform_name: str | None
    target_sample_rate: int | None
    root_key: int
    key_low: int
    key_high: int
    level: int = 127


@dataclass(frozen=True)
class HdsManifestSampleBankGroup:
    name: str
    member_sample_banks: tuple[str, ...]


@dataclass(frozen=True)
class HdsManifestProgramAssignment:
    target_kind: str
    target_name: str
    receive_channel: int


@dataclass(frozen=True)
class HdsManifestProgram:
    number: int
    assignments: tuple[HdsManifestProgramAssignment, ...]


@dataclass(frozen=True)
class HdsManifestVolume:
    name: str
    waveforms: tuple[HdsManifestWaveform, ...]
    sample_banks: tuple[HdsManifestSampleBank, ...]
    sample_bank_groups: tuple[HdsManifestSampleBankGroup, ...]
    programs: tuple[HdsManifestProgram, ...]


@dataclass(frozen=True)
class HdsManifestPartition:
    name: str
    volumes: tuple[HdsManifestVolume, ...]


@dataclass(frozen=True)
class HdsBuildManifest:
    schema_version: str
    size_bytes: int
    partitions: tuple[HdsManifestPartition, ...]


def _mapping(value: object, context: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise ValueError(f"{context} must be a JSON object")
    if not all(isinstance(key, str) for key in value):
        raise ValueError(f"{context} contains a non-string field name")
    return value


def _list(value: object, context: str) -> list[object]:
    if not isinstance(value, list):
        raise ValueError(f"{context} must be a JSON array")
    return value


def _string(value: object, context: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{context} must be a non-empty string")
    return value


def _integer(value: object, context: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{context} must be an integer")
    return value


def _fields(
    value: Mapping[str, object],
    context: str,
    *,
    required: set[str],
    optional: set[str] | None = None,
) -> None:
    optional = optional or set()
    missing = sorted(required - value.keys())
    unknown = sorted(value.keys() - required - optional)
    if missing:
        raise ValueError(f"{context} is missing required fields: {', '.join(missing)}")
    if unknown:
        raise ValueError(f"{context} has unknown fields: {', '.join(unknown)}")


def _parse_waveform(value: object, context: str, base_dir: Path) -> HdsManifestWaveform:
    row = _mapping(value, context)
    _fields(
        row,
        context,
        required={"id", "name", "path", "root_key"},
        optional={"target_sample_rate"},
    )
    source_path = Path(_string(row["path"], f"{context}.path"))
    if not source_path.is_absolute():
        source_path = base_dir / source_path
    return HdsManifestWaveform(
        id=_string(row["id"], f"{context}.id"),
        name=_string(row["name"], f"{context}.name"),
        path=source_path,
        root_key=_integer(row["root_key"], f"{context}.root_key"),
        target_sample_rate=(
            _integer(row["target_sample_rate"], f"{context}.target_sample_rate")
            if "target_sample_rate" in row
            else None
        ),
    )


def _parse_sample_bank(
    value: object,
    context: str,
    base_dir: Path,
) -> HdsManifestSampleBank:
    row = _mapping(value, context)
    _fields(
        row,
        context,
        required={"name", "root_key", "key_low", "key_high"},
        optional={
            "level",
            "waveform_id",
            "right_waveform_id",
            "interleaved_audio_path",
            "left_waveform_name",
            "right_waveform_name",
            "target_sample_rate",
        },
    )
    source_fields = [
        field for field in ("waveform_id", "interleaved_audio_path") if field in row
    ]
    if len(source_fields) != 1:
        raise ValueError(
            f"{context} must contain exactly one of waveform_id or interleaved_audio_path"
        )
    interleaved = source_fields[0] == "interleaved_audio_path"
    interleaved_only = {
        "left_waveform_name",
        "right_waveform_name",
        "target_sample_rate",
    }
    if not interleaved and any(field in row for field in interleaved_only):
        raise ValueError(
            f"{context} waveform_id form cannot contain interleaved-audio fields"
        )
    if interleaved and "right_waveform_id" in row:
        raise ValueError(
            f"{context} interleaved_audio_path form cannot contain right_waveform_id"
        )
    source_path: Path | None = None
    if interleaved:
        source_path = Path(
            _string(row["interleaved_audio_path"], f"{context}.interleaved_audio_path")
        )
        if not source_path.is_absolute():
            source_path = base_dir / source_path
    return HdsManifestSampleBank(
        name=_string(row["name"], f"{context}.name"),
        waveform_id=(
            _string(row["waveform_id"], f"{context}.waveform_id")
            if not interleaved
            else None
        ),
        right_waveform_id=(
            _string(row["right_waveform_id"], f"{context}.right_waveform_id")
            if "right_waveform_id" in row
            else None
        ),
        interleaved_audio_path=source_path,
        left_waveform_name=(
            _string(row["left_waveform_name"], f"{context}.left_waveform_name")
            if "left_waveform_name" in row
            else None
        ),
        right_waveform_name=(
            _string(row["right_waveform_name"], f"{context}.right_waveform_name")
            if "right_waveform_name" in row
            else None
        ),
        target_sample_rate=(
            _integer(row["target_sample_rate"], f"{context}.target_sample_rate")
            if "target_sample_rate" in row
            else None
        ),
        root_key=_integer(row["root_key"], f"{context}.root_key"),
        key_low=_integer(row["key_low"], f"{context}.key_low"),
        key_high=_integer(row["key_high"], f"{context}.key_high"),
        level=_integer(row.get("level", 127), f"{context}.level"),
    )


def _parse_sample_bank_group(value: object, context: str) -> HdsManifestSampleBankGroup:
    row = _mapping(value, context)
    _fields(
        row,
        context,
        required={"name"},
        optional={"member_sample_bank", "member_sample_banks"},
    )
    member_fields = [
        field for field in ("member_sample_bank", "member_sample_banks") if field in row
    ]
    if len(member_fields) != 1:
        raise ValueError(
            f"{context} must contain exactly one of member_sample_bank or member_sample_banks"
        )
    members: tuple[str, ...]
    if member_fields[0] == "member_sample_bank":
        members = (_string(row["member_sample_bank"], f"{context}.member_sample_bank"),)
    else:
        members = tuple(
            _string(item, f"{context}.member_sample_banks[{index}]")
            for index, item in enumerate(
                _list(row["member_sample_banks"], f"{context}.member_sample_banks")
            )
        )
    if not 1 <= len(members) <= 3:
        raise ValueError(f"{context} must contain 1..3 member sample banks")
    if len(members) != len(set(members)):
        raise ValueError(f"{context} contains duplicate member sample banks")
    return HdsManifestSampleBankGroup(
        name=_string(row["name"], f"{context}.name"),
        member_sample_banks=members,
    )


def _parse_program_assignment(value: object, context: str) -> HdsManifestProgramAssignment:
    row = _mapping(value, context)
    _fields(
        row,
        context,
        required={"receive_channel"},
        optional={"sample_bank", "sample_bank_group"},
    )
    targets = [name for name in ("sample_bank", "sample_bank_group") if name in row]
    if len(targets) != 1:
        raise ValueError(f"{context} must contain exactly one of sample_bank or sample_bank_group")
    target_field = targets[0]
    return HdsManifestProgramAssignment(
        target_kind="SBNK" if target_field == "sample_bank" else "SBAC",
        target_name=_string(row[target_field], f"{context}.{target_field}"),
        receive_channel=_integer(row["receive_channel"], f"{context}.receive_channel"),
    )


def _parse_program(value: object, context: str) -> HdsManifestProgram:
    row = _mapping(value, context)
    _fields(row, context, required={"number", "assignments"})
    assignments = tuple(
        _parse_program_assignment(item, f"{context}.assignments[{index}]")
        for index, item in enumerate(_list(row["assignments"], f"{context}.assignments"))
    )
    return HdsManifestProgram(
        number=_integer(row["number"], f"{context}.number"),
        assignments=assignments,
    )


def _parse_volume(value: object, context: str, base_dir: Path) -> HdsManifestVolume:
    row = _mapping(value, context)
    _fields(
        row,
        context,
        required={"name", "waveforms", "sample_banks"},
        optional={"sample_bank_groups", "programs"},
    )
    waveforms = tuple(
        _parse_waveform(item, f"{context}.waveforms[{index}]", base_dir)
        for index, item in enumerate(_list(row["waveforms"], f"{context}.waveforms"))
    )
    waveform_ids = [waveform.id for waveform in waveforms]
    if len(waveform_ids) != len(set(waveform_ids)):
        raise ValueError(f"{context}.waveforms contains duplicate ids")
    sample_banks = tuple(
        _parse_sample_bank(item, f"{context}.sample_banks[{index}]", base_dir)
        for index, item in enumerate(_list(row["sample_banks"], f"{context}.sample_banks"))
    )
    known_waveforms = set(waveform_ids)
    for index, bank in enumerate(sample_banks):
        if bank.waveform_id is not None and bank.waveform_id not in known_waveforms:
            raise ValueError(
                f"{context}.sample_banks[{index}].waveform_id references unknown waveform "
                f"{bank.waveform_id!r}"
            )
        if bank.right_waveform_id is not None:
            if bank.right_waveform_id not in known_waveforms:
                raise ValueError(
                    f"{context}.sample_banks[{index}].right_waveform_id references "
                    f"unknown waveform {bank.right_waveform_id!r}"
                )
            if bank.right_waveform_id == bank.waveform_id:
                raise ValueError(f"{context}.sample_banks[{index}] stereo members must be distinct")
    bank_names = [bank.name for bank in sample_banks]
    if len(bank_names) != len(set(bank_names)):
        raise ValueError(f"{context}.sample_banks contains duplicate names")
    sample_bank_groups = tuple(
        _parse_sample_bank_group(item, f"{context}.sample_bank_groups[{index}]")
        for index, item in enumerate(
            _list(row.get("sample_bank_groups", []), f"{context}.sample_bank_groups")
        )
    )
    group_names = [group.name for group in sample_bank_groups]
    if len(group_names) != len(set(group_names)):
        raise ValueError(f"{context}.sample_bank_groups contains duplicate names")
    known_banks = set(bank_names)
    for index, group in enumerate(sample_bank_groups):
        for member_index, member_name in enumerate(group.member_sample_banks):
            if member_name not in known_banks:
                raise ValueError(
                    f"{context}.sample_bank_groups[{index}].member_sample_banks"
                    f"[{member_index}] references unknown sample bank {member_name!r}"
                )
    programs = tuple(
        _parse_program(item, f"{context}.programs[{index}]")
        for index, item in enumerate(_list(row.get("programs", []), f"{context}.programs"))
    )
    if len({program.number for program in programs}) != len(programs):
        raise ValueError(f"{context}.programs contains duplicate numbers")
    known_groups = set(group_names)
    for program_index, program in enumerate(programs):
        for assignment_index, assignment in enumerate(program.assignments):
            known_targets = known_banks if assignment.target_kind == "SBNK" else known_groups
            if assignment.target_name not in known_targets:
                target_label = "sample bank" if assignment.target_kind == "SBNK" else "group"
                raise ValueError(
                    f"{context}.programs[{program_index}].assignments[{assignment_index}] "
                    f"references unknown {target_label} {assignment.target_name!r}"
                )
    return HdsManifestVolume(
        name=_string(row["name"], f"{context}.name"),
        waveforms=waveforms,
        sample_banks=sample_banks,
        sample_bank_groups=sample_bank_groups,
        programs=programs,
    )


def _parse_partition(value: object, context: str, base_dir: Path) -> HdsManifestPartition:
    row = _mapping(value, context)
    _fields(row, context, required={"name", "volumes"})
    volumes = tuple(
        _parse_volume(item, f"{context}.volumes[{index}]", base_dir)
        for index, item in enumerate(_list(row["volumes"], f"{context}.volumes"))
    )
    if not volumes:
        raise ValueError(f"{context}.volumes must contain at least one volume")
    return HdsManifestPartition(
        name=_string(row["name"], f"{context}.name"),
        volumes=volumes,
    )


def parse_hds_build_manifest(
    value: object,
    *,
    base_dir: str | Path = ".",
) -> HdsBuildManifest:
    """Parse and validate a JSON-compatible HDS build manifest value."""
    row = _mapping(value, "manifest")
    _fields(row, "manifest", required={"schema_version", "size_bytes", "partitions"})
    schema_version = _string(row["schema_version"], "manifest.schema_version")
    if schema_version != HDS_BUILD_MANIFEST_SCHEMA_VERSION:
        raise ValueError(
            "manifest.schema_version must be "
            f"{HDS_BUILD_MANIFEST_SCHEMA_VERSION!r}, got {schema_version!r}"
        )
    root = Path(base_dir)
    partitions = tuple(
        _parse_partition(item, f"manifest.partitions[{index}]", root)
        for index, item in enumerate(_list(row["partitions"], "manifest.partitions"))
    )
    if not partitions:
        raise ValueError("manifest.partitions must contain at least one partition")
    return HdsBuildManifest(
        schema_version=schema_version,
        size_bytes=_integer(row["size_bytes"], "manifest.size_bytes"),
        partitions=partitions,
    )


def load_hds_build_manifest(path: str | Path) -> HdsBuildManifest:
    """Load a versioned HDS build manifest from JSON."""
    source = Path(path)
    with source.open(encoding="utf-8") as handle:
        value = json.load(handle)
    return parse_hds_build_manifest(value, base_dir=source.parent)


def build_hds_from_manifest(
    manifest: HdsBuildManifest,
    output_path: str | Path,
    *,
    overwrite: bool = False,
) -> HdsWriteResult:
    """Build an HDS image from a validated manifest."""
    output = Path(output_path)
    if output.exists() and not overwrite:
        raise FileExistsError(f"output already exists: {output}")
    builder = HdsImageBuilder(size_bytes=manifest.size_bytes)
    for partition_spec in manifest.partitions:
        partition = builder.add_partition(partition_spec.name)
        for volume_spec in partition_spec.volumes:
            volume = partition.add_volume(volume_spec.name)
            waveform_refs: dict[str, WaveformRef] = {}
            sample_bank_refs: dict[str, SampleBankRef] = {}
            for waveform_spec in volume_spec.waveforms:
                waveform_refs[waveform_spec.id] = volume.add_waveform_from_wav(
                    name=waveform_spec.name,
                    path=waveform_spec.path,
                    root_key=waveform_spec.root_key,
                    target_sample_rate=waveform_spec.target_sample_rate,
                )
            for bank_spec in volume_spec.sample_banks:
                if bank_spec.interleaved_audio_path is not None:
                    sample_bank_refs[bank_spec.name] = (
                        volume.add_stereo_sample_bank_from_audio(
                            name=bank_spec.name,
                            path=bank_spec.interleaved_audio_path,
                            root_key=bank_spec.root_key,
                            key_low=bank_spec.key_low,
                            key_high=bank_spec.key_high,
                            level=bank_spec.level,
                            left_waveform_name=bank_spec.left_waveform_name,
                            right_waveform_name=bank_spec.right_waveform_name,
                            target_sample_rate=bank_spec.target_sample_rate,
                        )
                    )
                elif bank_spec.right_waveform_id is None:
                    if bank_spec.waveform_id is None:
                        raise ValueError(f"sample bank {bank_spec.name!r} has no waveform source")
                    sample_bank_refs[bank_spec.name] = volume.add_sample_bank(
                        name=bank_spec.name,
                        waveform=waveform_refs[bank_spec.waveform_id],
                        root_key=bank_spec.root_key,
                        key_low=bank_spec.key_low,
                        key_high=bank_spec.key_high,
                        level=bank_spec.level,
                    )
                else:
                    if bank_spec.waveform_id is None:
                        raise ValueError(f"sample bank {bank_spec.name!r} has no left waveform")
                    sample_bank_refs[bank_spec.name] = volume.add_stereo_sample_bank(
                        name=bank_spec.name,
                        left_waveform=waveform_refs[bank_spec.waveform_id],
                        right_waveform=waveform_refs[bank_spec.right_waveform_id],
                        root_key=bank_spec.root_key,
                        key_low=bank_spec.key_low,
                        key_high=bank_spec.key_high,
                        level=bank_spec.level,
                    )
            group_refs = {
                group_spec.name: volume.add_sample_bank_group(
                    name=group_spec.name,
                    members=tuple(
                        sample_bank_refs[name] for name in group_spec.member_sample_banks
                    ),
                )
                for group_spec in volume_spec.sample_bank_groups
            }
            for program_spec in volume_spec.programs:
                program = volume.add_program(number=program_spec.number)
                for assignment in program_spec.assignments:
                    if assignment.target_kind == "SBAC":
                        program.assign_sample_bank_group(
                            group_refs[assignment.target_name],
                            receive_channel=assignment.receive_channel,
                        )
                    else:
                        program.assign_sample_bank(
                            sample_bank_refs[assignment.target_name],
                            receive_channel=assignment.receive_channel,
                        )
    return builder.write(output)


__all__ = [
    "HDS_BUILD_MANIFEST_SCHEMA_VERSION",
    "HdsBuildManifest",
    "HdsManifestPartition",
    "HdsManifestSampleBank",
    "HdsManifestSampleBankGroup",
    "HdsManifestProgram",
    "HdsManifestProgramAssignment",
    "HdsManifestVolume",
    "HdsManifestWaveform",
    "build_hds_from_manifest",
    "load_hds_build_manifest",
    "parse_hds_build_manifest",
]
