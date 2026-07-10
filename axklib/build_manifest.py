"""Versioned JSON manifest support for fresh HDS image creation."""

from __future__ import annotations

import json
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path

from axklib.write import HdsImageBuilder, HdsWriteResult, WaveformRef

HDS_BUILD_MANIFEST_SCHEMA_VERSION = "1.0"


@dataclass(frozen=True)
class HdsManifestWaveform:
    id: str
    name: str
    path: Path
    root_key: int


@dataclass(frozen=True)
class HdsManifestSampleBank:
    name: str
    waveform_id: str
    root_key: int
    key_low: int
    key_high: int
    level: int = 127


@dataclass(frozen=True)
class HdsManifestVolume:
    name: str
    waveforms: tuple[HdsManifestWaveform, ...]
    sample_banks: tuple[HdsManifestSampleBank, ...]


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
    _fields(row, context, required={"id", "name", "path", "root_key"})
    source_path = Path(_string(row["path"], f"{context}.path"))
    if not source_path.is_absolute():
        source_path = base_dir / source_path
    return HdsManifestWaveform(
        id=_string(row["id"], f"{context}.id"),
        name=_string(row["name"], f"{context}.name"),
        path=source_path,
        root_key=_integer(row["root_key"], f"{context}.root_key"),
    )


def _parse_sample_bank(value: object, context: str) -> HdsManifestSampleBank:
    row = _mapping(value, context)
    _fields(
        row,
        context,
        required={"name", "waveform_id", "root_key", "key_low", "key_high"},
        optional={"level"},
    )
    return HdsManifestSampleBank(
        name=_string(row["name"], f"{context}.name"),
        waveform_id=_string(row["waveform_id"], f"{context}.waveform_id"),
        root_key=_integer(row["root_key"], f"{context}.root_key"),
        key_low=_integer(row["key_low"], f"{context}.key_low"),
        key_high=_integer(row["key_high"], f"{context}.key_high"),
        level=_integer(row.get("level", 127), f"{context}.level"),
    )


def _parse_volume(value: object, context: str, base_dir: Path) -> HdsManifestVolume:
    row = _mapping(value, context)
    _fields(row, context, required={"name", "waveforms", "sample_banks"})
    waveforms = tuple(
        _parse_waveform(item, f"{context}.waveforms[{index}]", base_dir)
        for index, item in enumerate(_list(row["waveforms"], f"{context}.waveforms"))
    )
    waveform_ids = [waveform.id for waveform in waveforms]
    if len(waveform_ids) != len(set(waveform_ids)):
        raise ValueError(f"{context}.waveforms contains duplicate ids")
    sample_banks = tuple(
        _parse_sample_bank(item, f"{context}.sample_banks[{index}]")
        for index, item in enumerate(_list(row["sample_banks"], f"{context}.sample_banks"))
    )
    known_waveforms = set(waveform_ids)
    for index, bank in enumerate(sample_banks):
        if bank.waveform_id not in known_waveforms:
            raise ValueError(
                f"{context}.sample_banks[{index}].waveform_id references unknown waveform "
                f"{bank.waveform_id!r}"
            )
    return HdsManifestVolume(
        name=_string(row["name"], f"{context}.name"),
        waveforms=waveforms,
        sample_banks=sample_banks,
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
    _allow_unproven_geometry: bool = False,
) -> HdsWriteResult:
    """Build an HDS image from a validated manifest."""
    output = Path(output_path)
    if output.exists() and not overwrite:
        raise FileExistsError(f"output already exists: {output}")
    builder = HdsImageBuilder(
        size_bytes=manifest.size_bytes,
        _allow_unproven_geometry=_allow_unproven_geometry,
    )
    for partition_spec in manifest.partitions:
        partition = builder.add_partition(partition_spec.name)
        for volume_spec in partition_spec.volumes:
            volume = partition.add_volume(volume_spec.name)
            waveform_refs: dict[str, WaveformRef] = {}
            for waveform_spec in volume_spec.waveforms:
                waveform_refs[waveform_spec.id] = volume.add_waveform_from_wav(
                    name=waveform_spec.name,
                    path=waveform_spec.path,
                    root_key=waveform_spec.root_key,
                )
            for bank_spec in volume_spec.sample_banks:
                volume.add_sample_bank(
                    name=bank_spec.name,
                    waveform=waveform_refs[bank_spec.waveform_id],
                    root_key=bank_spec.root_key,
                    key_low=bank_spec.key_low,
                    key_high=bank_spec.key_high,
                    level=bank_spec.level,
                )
    return builder.write(output)


__all__ = [
    "HDS_BUILD_MANIFEST_SCHEMA_VERSION",
    "HdsBuildManifest",
    "HdsManifestPartition",
    "HdsManifestSampleBank",
    "HdsManifestVolume",
    "HdsManifestWaveform",
    "build_hds_from_manifest",
    "load_hds_build_manifest",
    "parse_hds_build_manifest",
]
