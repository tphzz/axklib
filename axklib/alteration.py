"""Transactional alteration of existing Yamaha SFS hard-disk images."""

from __future__ import annotations

import json
import os
import shutil
import tempfile
from collections import defaultdict
from collections.abc import Callable, Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from axklib.build_manifest import (
    HdsManifestVolume,
    parse_hds_manifest_volume,
    populate_volume_builder,
)
from axklib.containers import sfs_allocation, sfs_dump, sfs_extents, sfs_inventory
from axklib.objects import current as current_objects
from axklib.parameters import current as current_parameters
from axklib.parameters import sbnk_contract
from axklib.waveform_orphans import (
    WAVEFORM_STATUS_KNOWN_UNREFERENCED,
    WaveformOrphanReport,
    _classify_waveform_inputs,
    _CurrentObjectInput,
)
from axklib.write import (
    CLUSTER_SIZE,
    MIN_IMAGE_SIZE_BYTES,
    ROOT_DIRECTORY_ID,
    SECTOR_SIZE,
    HdsImageBuilder,
    _directory_entry,
    _index_record,
    _RecordPlan,
)

ALTERATION_MANIFEST_SCHEMA_VERSION = "1.0"
INDEX_RECORD_SIZE = 72
INDEX_RECORDS_PER_BLOCK = 14
INDEX_BLOCK_SIZE = 1024
CONTINUATION_EXTENTS_PER_CLUSTER = (
    CLUSTER_SIZE - sfs_extents.CONTINUATION_HEADER_SIZE
) // sfs_extents.EXTENT_SIZE


@dataclass(frozen=True)
class OperationReference:
    """Reference to the result scope of an earlier transaction operation."""

    operation_ref: str


@dataclass(frozen=True)
class InsertSampleBankSpec:
    """A new SBNK referencing existing waveform objects in one volume."""

    name: str
    waveform_name: str
    right_waveform_name: str | None
    root_key: int
    key_low: int
    key_high: int
    level: int


@dataclass(frozen=True)
class AlterationOperation:
    """One validated operation in an alteration transaction."""

    id: str
    type: str
    partition_index: int | OperationReference
    volume_name: str | None = None
    volume: HdsManifestVolume | None = None
    sample_bank_name: str | None = None
    sample_bank: InsertSampleBankSpec | None = None
    waveform_name: str | None = None


@dataclass(frozen=True)
class AlterationManifest:
    """A versioned, ordered alteration transaction."""

    schema_version: str
    operations: tuple[AlterationOperation, ...]


@dataclass(frozen=True)
class OperationReport:
    """Result of planning one operation."""

    id: str
    type: str
    partition_index: int
    volume_name: str
    object_name: str = ""
    removed_sfs_ids: tuple[int, ...] = ()
    inserted_sfs_ids: tuple[int, ...] = ()
    freed_clusters: int = 0
    allocated_clusters: int = 0


@dataclass(frozen=True)
class AlterationResult:
    """Dry-run or applied alteration result."""

    source_path: Path
    output_path: Path | None
    applied: bool
    operations: tuple[OperationReport, ...]


@dataclass
class _StoredRecord:
    sfs_id: int
    raw_index: bytes
    payload: bytes
    payload_kind: str
    extents: list[sfs_extents.SfsExtent]
    continuation_clusters: list[int]
    extent_warnings: tuple[str, ...] = ()
    original: bool = True


@dataclass
class _PartitionState:
    index: int
    name: str
    source: Path
    start_sector: int
    sectors_per_cluster: int
    cluster_count: int
    bitmap_offset: int
    bitmap_mirror_offset: int
    index_offset: int
    index_capacity_bytes: int
    first_payload_cluster: int
    records: dict[int, _StoredRecord]
    bitmap: bytearray
    deleted_ids: set[int] = field(default_factory=set)
    changed_ids: set[int] = field(default_factory=set)

    @property
    def root(self) -> _StoredRecord:
        try:
            return self.records[ROOT_DIRECTORY_ID]
        except KeyError as exc:
            raise ValueError(f"partition {self.index} has no readable root directory") from exc


@dataclass
class _TransactionState:
    source: Path
    partitions: dict[int, _PartitionState]
    known_object_edges: list[tuple[int, int, int]] = field(default_factory=list)
    reports: list[OperationReport] = field(default_factory=list)


OperationParser = Callable[
    [str, int | OperationReference, Mapping[str, object], str, Path], AlterationOperation
]
OperationHandler = Callable[[_TransactionState, AlterationOperation], OperationReport]


@dataclass(frozen=True)
class _OperationHandler:
    parse: OperationParser
    apply: OperationHandler


_HANDLERS: dict[str, _OperationHandler] = {}


def _register(
    name: str, *, parser: OperationParser
) -> Callable[[OperationHandler], OperationHandler]:
    def decorator(handler: OperationHandler) -> OperationHandler:
        _HANDLERS[name] = _OperationHandler(parser, handler)
        return handler

    return decorator


def _mapping(value: object, context: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping) or not all(isinstance(key, str) for key in value):
        raise ValueError(f"{context} must be a JSON object with string field names")
    return value


def _string(value: object, context: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{context} must be a non-empty string")
    return value


def _integer(value: object, context: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{context} must be an integer")
    return value


def _partition_selector(
    value: object, context: str, previous_operation_ids: set[str]
) -> int | OperationReference:
    if isinstance(value, int) and not isinstance(value, bool):
        if value < 0 or value > 7:
            raise ValueError(f"{context} must be between 0 and 7")
        return value
    row = _mapping(value, context)
    if set(row) != {"operation_ref"}:
        raise ValueError(f"{context} reference must contain only operation_ref")
    operation_ref = _string(row["operation_ref"], f"{context}.operation_ref")
    if operation_ref not in previous_operation_ids:
        raise ValueError(
            f"{context}.operation_ref must name an earlier operation, got {operation_ref!r}"
        )
    return OperationReference(operation_ref)


def _parse_delete_volume_operation(
    operation_id: str,
    partition_index: int | OperationReference,
    item: Mapping[str, object],
    context: str,
    _base_dir: Path,
) -> AlterationOperation:
    expected = {"id", "type", "partition_index", "volume_name"}
    if set(item) != expected:
        raise ValueError(f"{context} fields must be exactly {', '.join(sorted(expected))}")
    return AlterationOperation(
        id=operation_id,
        type="delete_volume",
        partition_index=partition_index,
        volume_name=_string(item["volume_name"], f"{context}.volume_name"),
    )


def _parse_insert_volume_operation(
    operation_id: str,
    partition_index: int | OperationReference,
    item: Mapping[str, object],
    context: str,
    base_dir: Path,
) -> AlterationOperation:
    expected = {"id", "type", "partition_index", "volume"}
    if set(item) != expected:
        raise ValueError(f"{context} fields must be exactly {', '.join(sorted(expected))}")
    return AlterationOperation(
        id=operation_id,
        type="insert_volume",
        partition_index=partition_index,
        volume=parse_hds_manifest_volume(item["volume"], base_dir=base_dir),
    )


def _midi_value(value: object, context: str) -> int:
    result = _integer(value, context)
    if result < 0 or result > 127:
        raise ValueError(f"{context} must be between 0 and 127")
    return result


def _parse_delete_sbnk_operation(
    operation_id: str,
    partition_index: int | OperationReference,
    item: Mapping[str, object],
    context: str,
    _base_dir: Path,
) -> AlterationOperation:
    expected = {"id", "type", "partition_index", "volume_name", "sample_bank_name"}
    if set(item) != expected:
        raise ValueError(f"{context} fields must be exactly {', '.join(sorted(expected))}")
    return AlterationOperation(
        id=operation_id,
        type="delete_sbnk",
        partition_index=partition_index,
        volume_name=_string(item["volume_name"], f"{context}.volume_name"),
        sample_bank_name=_string(item["sample_bank_name"], f"{context}.sample_bank_name"),
    )


def _parse_insert_sbnk_operation(
    operation_id: str,
    partition_index: int | OperationReference,
    item: Mapping[str, object],
    context: str,
    _base_dir: Path,
) -> AlterationOperation:
    expected = {"id", "type", "partition_index", "volume_name", "sample_bank"}
    if set(item) != expected:
        raise ValueError(f"{context} fields must be exactly {', '.join(sorted(expected))}")
    bank_context = f"{context}.sample_bank"
    bank = _mapping(item["sample_bank"], bank_context)
    required = {"name", "waveform_name", "root_key", "key_low", "key_high"}
    optional = {"right_waveform_name", "level"}
    missing = required - bank.keys()
    unknown = bank.keys() - required - optional
    if missing:
        raise ValueError(f"{bank_context} is missing required fields: {', '.join(sorted(missing))}")
    if unknown:
        raise ValueError(f"{bank_context} has unknown fields: {', '.join(sorted(unknown))}")
    key_low = _midi_value(bank["key_low"], f"{bank_context}.key_low")
    key_high = _midi_value(bank["key_high"], f"{bank_context}.key_high")
    if key_high < key_low:
        raise ValueError(f"{bank_context}.key_high must not be below key_low")
    right_name = (
        _string(bank["right_waveform_name"], f"{bank_context}.right_waveform_name")
        if "right_waveform_name" in bank
        else None
    )
    return AlterationOperation(
        id=operation_id,
        type="insert_sbnk",
        partition_index=partition_index,
        volume_name=_string(item["volume_name"], f"{context}.volume_name"),
        sample_bank=InsertSampleBankSpec(
            name=_string(bank["name"], f"{bank_context}.name"),
            waveform_name=_string(bank["waveform_name"], f"{bank_context}.waveform_name"),
            right_waveform_name=right_name,
            root_key=_midi_value(bank["root_key"], f"{bank_context}.root_key"),
            key_low=key_low,
            key_high=key_high,
            level=_midi_value(bank.get("level", 100), f"{bank_context}.level"),
        ),
    )


def _parse_delete_waveform_operation(
    operation_id: str,
    partition_index: int | OperationReference,
    item: Mapping[str, object],
    context: str,
    _base_dir: Path,
) -> AlterationOperation:
    expected = {"id", "type", "partition_index", "volume_name", "waveform_name"}
    if set(item) != expected:
        raise ValueError(f"{context} fields must be exactly {', '.join(sorted(expected))}")
    return AlterationOperation(
        id=operation_id,
        type="delete_waveform",
        partition_index=partition_index,
        volume_name=_string(item["volume_name"], f"{context}.volume_name"),
        waveform_name=_string(item["waveform_name"], f"{context}.waveform_name"),
    )


def parse_alteration_manifest(
    value: object, *, base_dir: str | Path = "."
) -> AlterationManifest:
    """Parse a JSON-compatible alteration transaction."""
    row = _mapping(value, "manifest")
    if set(row) != {"schema_version", "operations"}:
        raise ValueError("manifest fields must be exactly schema_version and operations")
    version = _string(row["schema_version"], "manifest.schema_version")
    if version != ALTERATION_MANIFEST_SCHEMA_VERSION:
        raise ValueError(
            f"manifest.schema_version must be {ALTERATION_MANIFEST_SCHEMA_VERSION!r}"
        )
    raw_operations = row["operations"]
    if not isinstance(raw_operations, list) or not raw_operations:
        raise ValueError("manifest.operations must be a non-empty JSON array")
    seen_ids: set[str] = set()
    operations: list[AlterationOperation] = []
    for index, raw in enumerate(raw_operations):
        context = f"manifest.operations[{index}]"
        item = _mapping(raw, context)
        operation_id = _string(item.get("id"), f"{context}.id")
        if operation_id in seen_ids:
            raise ValueError(f"duplicate operation id: {operation_id!r}")
        operation_type = _string(item.get("type"), f"{context}.type")
        handler = _HANDLERS.get(operation_type)
        if handler is None:
            raise ValueError(f"unsupported alteration operation type: {operation_type!r}")
        partition_index = _partition_selector(
            item.get("partition_index"), f"{context}.partition_index", seen_ids
        )
        operation = handler.parse(operation_id, partition_index, item, context, Path(base_dir))
        seen_ids.add(operation_id)
        operations.append(operation)
    return AlterationManifest(version, tuple(operations))


def load_alteration_manifest(path: str | Path) -> AlterationManifest:
    """Load an alteration transaction from JSON."""
    source = Path(path)
    with source.open(encoding="utf-8") as handle:
        return parse_alteration_manifest(json.load(handle), base_dir=source.parent)


def _field_value(partition: Mapping[str, object], name: str) -> int:
    fields = partition.get("fields")
    if not isinstance(fields, Mapping):
        return 0
    field = fields.get(name)
    if not isinstance(field, Mapping):
        return 0
    value = field.get("value")
    return value if isinstance(value, int) else 0


def _derived_value(partition: Mapping[str, object], name: str) -> int:
    derived = partition.get("derived")
    if not isinstance(derived, Mapping):
        return 0
    value = derived.get(name)
    return value if isinstance(value, int) else 0


def _index_record_offset(sfs_id: int) -> int:
    return (sfs_id // INDEX_RECORDS_PER_BLOCK) * INDEX_BLOCK_SIZE + (
        sfs_id % INDEX_RECORDS_PER_BLOCK
    ) * INDEX_RECORD_SIZE


def _load_state(source: Path) -> _TransactionState:
    parsed = sfs_dump.parse_image(
        source, sfs_dump.ReadOptions(max_nodes=4, include_node_payloads=False)
    )
    classification = parsed.get("classification")
    if not isinstance(classification, Mapping) or classification.get("kind") != "yamaha_sfs":
        raise ValueError(f"source is not a supported Yamaha SFS image: {source}")
    sector_size = parsed.get("sector_size_bytes")
    if sector_size != SECTOR_SIZE:
        raise ValueError(f"alteration requires 512-byte sectors, got {sector_size!r}")
    raw_partitions = parsed.get("partitions")
    if not isinstance(raw_partitions, list):
        raise ValueError("source image has no readable partitions")
    partitions: dict[int, _PartitionState] = {}
    with sfs_dump.ImageReader(source) as reader:
        for raw_partition in raw_partitions:
            if not isinstance(raw_partition, Mapping):
                continue
            index = raw_partition.get("index")
            start_sector = raw_partition.get("start_sector")
            if not isinstance(index, int) or not isinstance(start_sector, int):
                continue
            sectors_per_cluster = _field_value(raw_partition, "sectors_per_cluster")
            cluster_count = _field_value(raw_partition, "number_of_clusters")
            index_clusters = _field_value(raw_partition, "unknown_static_0x0a8")
            index_cluster = _field_value(raw_partition, "cluster_offset_to_directory_index")
            bitmap_offset = _derived_value(raw_partition, "bitmap_absolute_offset")
            index_offset = _derived_value(raw_partition, "directory_index_absolute_offset")
            if not all(
                (
                    sectors_per_cluster,
                    cluster_count,
                    index_clusters,
                    index_cluster,
                    bitmap_offset,
                    index_offset,
                )
            ):
                raise ValueError(f"partition {index} has incomplete SFS geometry")
            index_capacity = index_clusters * sectors_per_cluster * SECTOR_SIZE
            bitmap_size = sfs_allocation.bitmap_byte_count(cluster_count)
            bitmap = bytearray(reader.read_at(bitmap_offset, bitmap_size))
            records: dict[int, _StoredRecord] = {}
            index_data = reader.read_at(index_offset, index_capacity)
            max_sfs_id = (index_capacity // INDEX_BLOCK_SIZE) * INDEX_RECORDS_PER_BLOCK
            for sfs_id in range(max_sfs_id):
                rel = _index_record_offset(sfs_id)
                raw_index = index_data[rel : rel + INDEX_RECORD_SIZE]
                if len(raw_index) < INDEX_RECORD_SIZE or raw_index[:2] == b"\x00\x00":
                    continue
                extent_read = sfs_extents.read_index_record_data(
                    reader,
                    raw_index,
                    partition_start_sector=start_sector,
                    sector_size=SECTOR_SIZE,
                    sectors_per_cluster=sectors_per_cluster,
                    cluster_count_limit=cluster_count,
                )
                kind, _object_type, _name, _directory_id, _parent_id, _count = (
                    sfs_inventory.classify_payload(extent_read.data[:0x200])
                )
                records[sfs_id] = _StoredRecord(
                    sfs_id=sfs_id,
                    raw_index=raw_index,
                    payload=extent_read.data,
                    payload_kind=kind,
                    extents=extent_read.extents,
                    continuation_clusters=extent_read.continuation_clusters,
                    extent_warnings=tuple(extent_read.warnings),
                )
            partitions[index] = _PartitionState(
                index=index,
                name=str(raw_partition.get("name", "")),
                source=source,
                start_sector=start_sector,
                sectors_per_cluster=sectors_per_cluster,
                cluster_count=cluster_count,
                bitmap_offset=bitmap_offset,
                bitmap_mirror_offset=start_sector * SECTOR_SIZE + 2048,
                index_offset=index_offset,
                index_capacity_bytes=index_capacity,
                first_payload_cluster=index_cluster + index_clusters,
                records=records,
                bitmap=bitmap,
            )
            root = partitions[index].root
            if root.extent_warnings or root.continuation_clusters:
                raise ValueError(
                    f"partition {index} root directory must use readable direct extents"
                )
    from axklib.containers import load_sfs_objects
    from axklib.relationships import build_relationship_graph

    objects = load_sfs_objects(source)
    object_ids = {
        item.object_key: (item.partition_index, item.sfs_id)
        for item in objects
        if item.partition_index is not None and item.sfs_id is not None
    }
    graph = build_relationship_graph(objects)
    known_edges: list[tuple[int, int, int]] = []
    for relationship in graph.relationships:
        source_ref = object_ids.get(relationship.source_key)
        target_ref = object_ids.get(relationship.target_key)
        if (
            relationship.quality == "Known"
            and source_ref is not None
            and target_ref is not None
            and source_ref[0] == target_ref[0]
        ):
            known_edges.append((source_ref[0], source_ref[1], target_ref[1]))
    allocation = sfs_allocation.analyze_image(source)
    unsafe_partitions = [
        summary.partition_index
        for summary in allocation.summaries
        if summary.reconstructed_used_not_stored_count
        or summary.extent_total_mismatch_count
    ]
    if unsafe_partitions:
        joined = ", ".join(str(index) for index in unsafe_partitions)
        raise ValueError(
            "source allocation metadata can expose live extents as free in partition(s): "
            + joined
        )
    return _TransactionState(source, partitions, known_edges)


def _directory_chunks(record: _StoredRecord) -> list[bytearray]:
    chunks: list[bytearray] = []
    for offset in range(0, len(record.payload), sfs_inventory.ENTRY_STRIDE):
        chunk = record.payload[offset : offset + sfs_inventory.ENTRY_STRIDE]
        if len(chunk) < sfs_inventory.ENTRY_STRIDE or chunk[:8] == b"\x00" * 8:
            break
        chunks.append(bytearray(chunk))
    if not chunks or _entry_name(chunks[0]) != ".":
        raise ValueError(f"SFS ID {record.sfs_id} is not a readable directory")
    return chunks


def _entry_name(chunk: bytearray) -> str:
    length = int.from_bytes(chunk[2:4], "big")
    return sfs_inventory.clean_ascii(bytes(chunk[8 : 8 + length]))


def _entry_link(chunk: bytearray) -> int:
    return int.from_bytes(chunk[4:8], "big")


def _set_bitmap(bitmap: bytearray, cluster: int, value: bool) -> None:
    mask = 1 << (7 - cluster % 8)
    if value:
        bitmap[cluster // 8] |= mask
    else:
        bitmap[cluster // 8] &= ~mask


def _release_record(partition: _PartitionState, record: _StoredRecord) -> int:
    released = 0
    for extent in record.extents:
        for cluster in range(extent.cluster_offset, extent.cluster_offset + extent.cluster_count):
            _set_bitmap(partition.bitmap, cluster, False)
            released += 1
    for cluster in record.continuation_clusters:
        _set_bitmap(partition.bitmap, cluster, False)
        released += 1
    partition.deleted_ids.add(record.sfs_id)
    partition.records.pop(record.sfs_id)
    return released


def _replace_directory_payload(partition: _PartitionState, record: _StoredRecord, payload: bytes) -> None:
    capacity = sum(extent.cluster_count * CLUSTER_SIZE for extent in record.extents)
    if len(payload) > capacity:
        raise ValueError(
            f"partition {partition.index} root directory needs {len(payload)} bytes but its "
            f"existing extent capacity is {capacity} bytes; root relocation is not enabled"
        )
    remaining = len(payload)
    updated = bytearray(record.raw_index)
    updated[6:10] = len(payload).to_bytes(4, "big")
    for extent_index, extent in enumerate(record.extents):
        capacity_for_extent = extent.cluster_count * CLUSTER_SIZE
        byte_count = min(remaining, capacity_for_extent) if remaining else capacity_for_extent
        remaining -= byte_count
        remaining = max(0, remaining)
        if len(record.extents) <= sfs_extents.DIRECT_EXTENT_LIMIT:
            offset = sfs_extents.EXTENT_RECORD_START + extent_index * sfs_extents.EXTENT_SIZE
            updated[offset + 8 : offset + 12] = byte_count.to_bytes(4, "big")
    if len(record.extents) == 1:
        updated[0x12:0x16] = len(payload).to_bytes(4, "big")
    if record.sfs_id == ROOT_DIRECTORY_ID:
        entry_count = len(sfs_inventory.parse_directory_entries(payload))
        updated[0x46:0x48] = max(0, entry_count - 2).to_bytes(2, "big")
    record.payload = payload
    record.raw_index = bytes(updated)
    partition.changed_ids.add(record.sfs_id)


def _volume_entry(partition: _PartitionState, volume_name: str) -> tuple[int, bytearray]:
    matches = [
        chunk
        for chunk in _directory_chunks(partition.root)
        if _entry_name(chunk) == volume_name and _entry_name(chunk) not in {".", ".."}
    ]
    if len(matches) != 1:
        raise ValueError(
            f"partition {partition.index} requires exactly one volume named {volume_name!r}; "
            f"found {len(matches)}"
        )
    return _entry_link(matches[0]), matches[0]


def _object_type(record: _StoredRecord) -> str:
    if len(record.payload) < 0x10 or not record.payload.startswith(b"FSFSDEV3SPLX"):
        return ""
    return record.payload[0x0C:0x10].decode("ascii", errors="replace")


def _volume_category(
    partition: _PartitionState, volume_name: str, category_name: str
) -> _StoredRecord:
    volume_id, _entry = _volume_entry(partition, volume_name)
    volume = partition.records.get(volume_id)
    if volume is None or volume.payload_kind != "directory" or volume.extent_warnings:
        raise ValueError(
            f"partition {partition.index} volume {volume_name!r} has no readable directory"
        )
    matches = [
        chunk
        for chunk in _directory_chunks(volume)
        if _entry_name(chunk) == category_name
    ]
    if len(matches) != 1:
        raise ValueError(
            f"partition {partition.index} volume {volume_name!r} requires exactly one "
            f"{category_name} category; found {len(matches)}"
        )
    category_id = _entry_link(matches[0])
    category = partition.records.get(category_id)
    if category is None or category.payload_kind != "directory" or category.extent_warnings:
        raise ValueError(
            f"partition {partition.index} volume {volume_name!r} {category_name} category "
            "is not a readable directory"
        )
    return category


def _category_object(
    partition: _PartitionState,
    volume_name: str,
    category_name: str,
    object_name: str,
    object_type: str,
) -> tuple[_StoredRecord, _StoredRecord, bytearray]:
    category = _volume_category(partition, volume_name, category_name)
    matches = [
        chunk
        for chunk in _directory_chunks(category)
        if _entry_name(chunk) == object_name
    ]
    if len(matches) != 1:
        raise ValueError(
            f"partition {partition.index} volume {volume_name!r} requires exactly one "
            f"{object_type} named {object_name!r}; found {len(matches)}"
        )
    record = partition.records.get(_entry_link(matches[0]))
    if record is None or record.extent_warnings or _object_type(record) != object_type:
        raise ValueError(
            f"partition {partition.index} volume {volume_name!r} {object_type} "
            f"{object_name!r} does not resolve to one readable {object_type} record"
        )
    return category, record, matches[0]


def _closure(partition: _PartitionState, volume_id: int) -> set[int]:
    closure: set[int] = set()
    queue = [volume_id]
    while queue:
        sfs_id = queue.pop(0)
        if sfs_id in closure:
            continue
        record = partition.records.get(sfs_id)
        if record is None:
            raise ValueError(
                f"partition {partition.index} volume closure references missing SFS ID {sfs_id}"
            )
        if record.extent_warnings:
            raise ValueError(
                f"partition {partition.index} volume closure contains unresolved SFS ID "
                f"{sfs_id}: {'; '.join(record.extent_warnings)}"
            )
        closure.add(sfs_id)
        if record.payload_kind != "directory":
            continue
        for chunk in _directory_chunks(record):
            name = _entry_name(chunk)
            if name not in {".", ".."}:
                queue.append(_entry_link(chunk))
    for record in partition.records.values():
        if (
            record.sfs_id in closure
            or record.sfs_id == ROOT_DIRECTORY_ID
            or record.payload_kind != "directory"
        ):
            continue
        for chunk in _directory_chunks(record):
            if _entry_name(chunk) not in {".", ".."} and _entry_link(chunk) in closure:
                raise ValueError(
                    f"partition {partition.index} SFS ID {record.sfs_id} references the volume "
                    "closure from outside it"
                )
    return closure


def _resolve_partition_index(
    state: _TransactionState, selector: int | OperationReference
) -> int:
    if isinstance(selector, int):
        return selector
    for report in state.reports:
        if report.id == selector.operation_ref:
            return report.partition_index
    raise ValueError(f"operation reference has no planned result: {selector.operation_ref!r}")


@_register("delete_volume", parser=_parse_delete_volume_operation)
def _delete_volume(state: _TransactionState, operation: AlterationOperation) -> OperationReport:
    partition_index = _resolve_partition_index(state, operation.partition_index)
    partition = state.partitions.get(partition_index)
    if partition is None:
        raise ValueError(f"partition index {partition_index} does not exist")
    assert operation.volume_name is not None
    volume_id, _entry = _volume_entry(partition, operation.volume_name)
    closure = _closure(partition, volume_id)
    current_ids = set(partition.records)
    for edge_partition, source_id, target_id in state.known_object_edges:
        if edge_partition != partition.index:
            continue
        if source_id not in current_ids or target_id not in current_ids:
            continue
        if (source_id in closure) != (target_id in closure):
            raise ValueError(
                f"partition {partition.index} volume {operation.volume_name!r} has a Known "
                f"object relationship crossing its closure: SFS ID {source_id} -> {target_id}"
            )
    root_chunks = [
        chunk
        for chunk in _directory_chunks(partition.root)
        if not (_entry_name(chunk) == operation.volume_name and _entry_link(chunk) == volume_id)
    ]
    _replace_directory_payload(partition, partition.root, b"".join(root_chunks))
    freed = sum(_release_record(partition, partition.records[sfs_id]) for sfs_id in sorted(closure))
    return OperationReport(
        id=operation.id,
        type=operation.type,
        partition_index=partition.index,
        volume_name=operation.volume_name,
        removed_sfs_ids=tuple(sorted(closure)),
        freed_clusters=freed,
    )


def _template_records(volume_spec: HdsManifestVolume) -> list[_RecordPlan]:
    builder = HdsImageBuilder(size_bytes=MIN_IMAGE_SIZE_BYTES)
    partition = builder.add_partition("AXK ALTER")
    volume = partition.add_volume(volume_spec.name)
    populate_volume_builder(volume, volume_spec)
    return [record for record in builder.plan()[0].records if record.sfs_id >= 3]


def _free_sfs_ids(partition: _PartitionState, count: int) -> list[int]:
    capacity = (partition.index_capacity_bytes // INDEX_BLOCK_SIZE) * INDEX_RECORDS_PER_BLOCK
    result = [sfs_id for sfs_id in range(3, capacity) if sfs_id not in partition.records]
    if len(result) < count:
        raise ValueError(
            f"partition {partition.index} needs {count} free SFS records but has {len(result)}"
        )
    return result[:count]


def _allocate_extents(
    partition: _PartitionState, cluster_count: int
) -> list[sfs_extents.SfsExtent]:
    free = [
        cluster
        for cluster in range(partition.first_payload_cluster, partition.cluster_count)
        if not sfs_allocation.bitmap_test(partition.bitmap, cluster)
    ]
    if len(free) < cluster_count:
        raise ValueError(
            f"partition {partition.index} needs {cluster_count} payload clusters but has "
            f"{len(free)} free"
        )
    selected = free[:cluster_count]
    runs: list[tuple[int, int]] = []
    for cluster in selected:
        if runs and runs[-1][0] + runs[-1][1] == cluster:
            start, count = runs[-1]
            runs[-1] = (start, count + 1)
        else:
            runs.append((cluster, 1))
        _set_bitmap(partition.bitmap, cluster, True)
    return [sfs_extents.SfsExtent(start, count, count * CLUSTER_SIZE) for start, count in runs]


def _allocate_list_clusters(partition: _PartitionState, count: int) -> list[int]:
    free = [
        cluster
        for cluster in range(partition.first_payload_cluster, partition.cluster_count)
        if not sfs_allocation.bitmap_test(partition.bitmap, cluster)
    ]
    if len(free) < count:
        raise ValueError(
            f"partition {partition.index} needs {count} continuation-list clusters but has "
            f"{len(free)} free"
        )
    result = free[:count]
    for cluster in result:
        _set_bitmap(partition.bitmap, cluster, True)
    return result


def _index_for_extents(template: _RecordPlan, extents: list[sfs_extents.SfsExtent], size: int) -> bytes:
    remaining = size
    normalized: list[sfs_extents.SfsExtent] = []
    for extent in extents:
        capacity = extent.cluster_count * CLUSTER_SIZE
        byte_count = min(remaining, capacity) if remaining else capacity
        remaining -= byte_count
        remaining = max(0, remaining)
        normalized.append(sfs_extents.SfsExtent(extent.cluster_offset, extent.cluster_count, byte_count))
    if len(normalized) > sfs_extents.DIRECT_EXTENT_LIMIT:
        raise ValueError("continuation index records require a list cluster")
    direct = _RecordPlan(
        template.sfs_id,
        template.payload,
        template.payload_kind,
        template.object_type,
        template.object_name,
        directory_tail_value=template.directory_tail_value,
    )
    direct.cluster_offset = normalized[0].cluster_offset
    direct.cluster_count = sum(extent.cluster_count for extent in normalized)
    data = bytearray(_index_record(direct))
    data[0:2] = len(normalized).to_bytes(2, "big")
    data[6:10] = size.to_bytes(4, "big")
    for index, extent in enumerate(normalized):
        offset = sfs_extents.EXTENT_RECORD_START + index * sfs_extents.EXTENT_SIZE
        data[offset : offset + 4] = extent.cluster_offset.to_bytes(4, "big")
        data[offset + 4 : offset + 8] = extent.cluster_count.to_bytes(4, "big")
        data[offset + 8 : offset + 12] = extent.byte_count.to_bytes(4, "big")
    return bytes(data)


def _continuation_index(
    template: _RecordPlan,
    extents: list[sfs_extents.SfsExtent],
    list_clusters: list[int],
) -> bytes:
    data = bytearray(_index_record(template))
    data[0:2] = len(extents).to_bytes(2, "big")
    data[4:6] = sum(extent.cluster_count for extent in extents).to_bytes(2, "big")
    data[6:10] = len(template.payload).to_bytes(4, "big")
    data[0x0A:0x0E] = list_clusters[0].to_bytes(4, "big")
    data[0x0E:0x12] = sum(extent.cluster_count for extent in extents).to_bytes(4, "big")
    data[0x12:0x16] = len(template.payload).to_bytes(4, "big")
    return bytes(data)


def _allocate_new_record(
    partition: _PartitionState, template: _RecordPlan
) -> tuple[_StoredRecord, int]:
    required_clusters = max(2, (len(template.payload) + CLUSTER_SIZE - 1) // CLUSTER_SIZE)
    extents = _allocate_extents(partition, required_clusters)
    list_clusters: list[int] = []
    if len(extents) <= sfs_extents.DIRECT_EXTENT_LIMIT:
        raw_index = _index_for_extents(template, extents, len(template.payload))
    else:
        list_count = (
            len(extents) + CONTINUATION_EXTENTS_PER_CLUSTER - 1
        ) // CONTINUATION_EXTENTS_PER_CLUSTER
        list_clusters = _allocate_list_clusters(partition, list_count)
        raw_index = _continuation_index(template, extents, list_clusters)
    record = _StoredRecord(
        sfs_id=template.sfs_id,
        raw_index=raw_index,
        payload=template.payload,
        payload_kind=template.payload_kind,
        extents=extents,
        continuation_clusters=list_clusters,
        original=False,
    )
    partition.records[record.sfs_id] = record
    partition.changed_ids.add(record.sfs_id)
    return record, required_clusters + len(list_clusters)


def _remap_directory(payload: bytes, ids: Mapping[int, int]) -> bytes:
    result = bytearray(payload)
    for offset in range(0, len(result), sfs_inventory.ENTRY_STRIDE):
        if result[offset : offset + 8] == b"\x00" * 8:
            break
        old = int.from_bytes(result[offset + 4 : offset + 8], "big")
        result[offset + 4 : offset + 8] = ids.get(old, old).to_bytes(4, "big")
        name_length = int.from_bytes(result[offset + 2 : offset + 4], "big")
        name = sfs_inventory.clean_ascii(bytes(result[offset + 8 : offset + 8 + name_length]))
        if name == ".." and old == ROOT_DIRECTORY_ID:
            result[offset + 11] = ids.get(int.from_bytes(payload[4:8], "big"), 0) & 0xFF
    return bytes(result)


@_register("insert_volume", parser=_parse_insert_volume_operation)
def _insert_volume(state: _TransactionState, operation: AlterationOperation) -> OperationReport:
    partition_index = _resolve_partition_index(state, operation.partition_index)
    partition = state.partitions.get(partition_index)
    if partition is None:
        raise ValueError(f"partition index {partition_index} does not exist")
    assert operation.volume is not None
    existing_names = {
        _entry_name(chunk)
        for chunk in _directory_chunks(partition.root)
        if _entry_name(chunk) not in {".", "..", "sfserrlog", "sfserram"}
    }
    if operation.volume.name in existing_names:
        raise ValueError(
            f"partition {partition.index} already contains volume {operation.volume.name!r}"
        )
    templates = _template_records(operation.volume)
    new_ids = _free_sfs_ids(partition, len(templates))
    id_map = {record.sfs_id: new_id for record, new_id in zip(templates, new_ids, strict=True)}
    allocated = 0
    inserted: list[int] = []
    for template, new_id in zip(templates, new_ids, strict=True):
        payload = (
            _remap_directory(template.payload, id_map)
            if template.payload_kind == "directory"
            else template.payload
        )
        template.sfs_id = new_id
        template.payload = payload
        _record, record_clusters = _allocate_new_record(partition, template)
        inserted.append(new_id)
        allocated += record_clusters
    root_chunks = _directory_chunks(partition.root)
    root_chunks.append(
        bytearray(_directory_entry(operation.volume.name, id_map[3], fixed_name_width=16))
    )
    _replace_directory_payload(partition, partition.root, b"".join(root_chunks))
    return OperationReport(
        id=operation.id,
        type=operation.type,
        partition_index=partition.index,
        volume_name=operation.volume.name,
        inserted_sfs_ids=tuple(inserted),
        allocated_clusters=allocated,
    )


def _assert_sbnk_unreferenced(
    state: _TransactionState,
    partition: _PartitionState,
    volume_name: str,
    bank_name: str,
    bank: _StoredRecord,
) -> None:
    decoded = current_parameters.decode_current_sbnk_members(bank.payload)
    if decoded.linked_program_numbers:
        programs = ", ".join(f"{number:03d}" for number in decoded.linked_program_numbers)
        raise ValueError(
            f"partition {partition.index} volume {volume_name!r} sample bank {bank_name!r} "
            f"has a Program-link bitmap for Program(s) {programs}"
        )
    for edge_partition, source_id, target_id in state.known_object_edges:
        if edge_partition != partition.index or target_id != bank.sfs_id:
            continue
        source = partition.records.get(source_id)
        if source is not None and _object_type(source) in {"SBAC", "PROG"}:
            raise ValueError(
                f"partition {partition.index} volume {volume_name!r} sample bank "
                f"{bank_name!r} is referenced by {_object_type(source)} SFS ID {source_id}"
            )
    sbac_directory = _volume_category(partition, volume_name, "SBAC")
    for chunk in _directory_chunks(sbac_directory):
        if _entry_name(chunk) in {".", ".."}:
            continue
        sbac = partition.records.get(_entry_link(chunk))
        if sbac is None or _object_type(sbac) != "SBAC":
            raise ValueError(
                f"partition {partition.index} volume {volume_name!r} has an unresolved SBAC "
                "record; referenced-bank deletion is unsafe"
            )
        _slot_count, _max_slots, slots = current_parameters.iter_sbac_slots(sbac.payload)
        if any(slot.name == bank_name for slot in slots):
            raise ValueError(
                f"partition {partition.index} volume {volume_name!r} sample bank "
                f"{bank_name!r} is referenced by SBAC {_entry_name(chunk)!r}"
            )
    prog_directory = _volume_category(partition, volume_name, "PROG")
    for chunk in _directory_chunks(prog_directory):
        if _entry_name(chunk) in {".", ".."}:
            continue
        prog = partition.records.get(_entry_link(chunk))
        if prog is None or _object_type(prog) != "PROG":
            raise ValueError(
                f"partition {partition.index} volume {volume_name!r} has an unresolved PROG "
                "record; referenced-bank deletion is unsafe"
            )
        if any(
            assignment.expected_category == "SBNK" and assignment.name == bank_name
            for assignment in current_parameters.iter_prog_assignments(prog.payload)
        ):
            raise ValueError(
                f"partition {partition.index} volume {volume_name!r} sample bank "
                f"{bank_name!r} is referenced by Program {_entry_name(chunk)!r}"
            )


@_register("delete_sbnk", parser=_parse_delete_sbnk_operation)
def _delete_sbnk(state: _TransactionState, operation: AlterationOperation) -> OperationReport:
    partition_index = _resolve_partition_index(state, operation.partition_index)
    partition = state.partitions.get(partition_index)
    if partition is None:
        raise ValueError(f"partition index {partition_index} does not exist")
    assert operation.volume_name is not None
    assert operation.sample_bank_name is not None
    directory, bank, _entry = _category_object(
        partition,
        operation.volume_name,
        "SBNK",
        operation.sample_bank_name,
        "SBNK",
    )
    _assert_sbnk_unreferenced(
        state, partition, operation.volume_name, operation.sample_bank_name, bank
    )
    chunks = [
        chunk
        for chunk in _directory_chunks(directory)
        if not (
            _entry_name(chunk) == operation.sample_bank_name
            and _entry_link(chunk) == bank.sfs_id
        )
    ]
    _replace_directory_payload(partition, directory, b"".join(chunks))
    freed = _release_record(partition, bank)
    state.known_object_edges = [
        edge
        for edge in state.known_object_edges
        if not (edge[0] == partition.index and bank.sfs_id in edge[1:])
    ]
    return OperationReport(
        id=operation.id,
        type=operation.type,
        partition_index=partition.index,
        volume_name=operation.volume_name,
        object_name=operation.sample_bank_name,
        removed_sfs_ids=(bank.sfs_id,),
        freed_clusters=freed,
    )


def _sbnk_member(
    waveform_name: str,
    waveform: _StoredRecord,
    root_key: int,
) -> sbnk_contract.CurrentSbnkMemberSpec:
    metadata = current_objects.decode_current_smpl_metadata(waveform.payload)
    if not metadata.smpl_link_id_0x078:
        raise ValueError(f"waveform {waveform_name!r} has no current SMPL link ID")
    return sbnk_contract.CurrentSbnkMemberSpec(
        sample_name=waveform_name,
        smpl_link_id_0x078=metadata.smpl_link_id_0x078,
        root_key_0x0d6=root_key,
        sample_rate_0x0d8=metadata.sample_rate_duplicate_0x07c,
        fine_tune_cents_0x0dc=metadata.fine_tune_cents_guess,
        wave_length_frames_0x0f0=metadata.wave_length_frames_0x092,
        loop_start_frame_0x0f8=metadata.loop_start_frame_0x096,
        loop_length_frames_0x100=metadata.loop_length_frames_0x09a,
    )


@_register("insert_sbnk", parser=_parse_insert_sbnk_operation)
def _insert_sbnk(state: _TransactionState, operation: AlterationOperation) -> OperationReport:
    partition_index = _resolve_partition_index(state, operation.partition_index)
    partition = state.partitions.get(partition_index)
    if partition is None:
        raise ValueError(f"partition index {partition_index} does not exist")
    assert operation.volume_name is not None
    assert operation.sample_bank is not None
    spec = operation.sample_bank
    directory = _volume_category(partition, operation.volume_name, "SBNK")
    if any(_entry_name(chunk) == spec.name for chunk in _directory_chunks(directory)):
        raise ValueError(
            f"partition {partition.index} volume {operation.volume_name!r} already contains "
            f"sample bank {spec.name!r}"
        )
    _smpl_directory, left_waveform, _left_entry = _category_object(
        partition,
        operation.volume_name,
        "SMPL",
        spec.waveform_name,
        "SMPL",
    )
    left = _sbnk_member(spec.waveform_name, left_waveform, spec.root_key)
    right_waveform: _StoredRecord | None = None
    right: sbnk_contract.CurrentSbnkMemberSpec | None = None
    if spec.right_waveform_name is not None:
        _right_dir, right_waveform, _right_entry = _category_object(
            partition,
            operation.volume_name,
            "SMPL",
            spec.right_waveform_name,
            "SMPL",
        )
        right = _sbnk_member(spec.right_waveform_name, right_waveform, spec.root_key)
        if (
            left.sample_rate_0x0d8 != right.sample_rate_0x0d8
            or left.wave_length_frames_0x0f0 != right.wave_length_frames_0x0f0
        ):
            raise ValueError(
                f"stereo sample bank {spec.name!r} requires waveform members with matching "
                "sample rates and frame counts"
            )
    if right is None:
        payload = sbnk_contract.serialize_current_single_member_sbnk_payload(
            bank_name=spec.name,
            left=left,
            inactive_right_policy=(
                sbnk_contract.CURRENT_SBNK_SINGLE_MEMBER_INACTIVE_RIGHT_POLICY_ZERO
            ),
            key_range_high_0x0e2=spec.key_high,
            key_range_low_0x0e3=spec.key_low,
            sample_level_0x116=spec.level,
        )
    else:
        payload = sbnk_contract.serialize_current_two_member_sbnk_payload(
            bank_name=spec.name,
            left=left,
            right=right,
            key_range_high_0x0e2=spec.key_high,
            key_range_low_0x0e3=spec.key_low,
            sample_level_0x116=spec.level,
        )
    new_id = _free_sfs_ids(partition, 1)[0]
    record, allocated = _allocate_new_record(
        partition,
        _RecordPlan(new_id, payload, "object", "SBNK", spec.name),
    )
    chunks = _directory_chunks(directory)
    chunks.append(bytearray(_directory_entry(spec.name, new_id, fixed_name_width=16)))
    _replace_directory_payload(partition, directory, b"".join(chunks))
    state.known_object_edges.append((partition.index, record.sfs_id, left_waveform.sfs_id))
    if right_waveform is not None:
        state.known_object_edges.append(
            (partition.index, record.sfs_id, right_waveform.sfs_id)
        )
    return OperationReport(
        id=operation.id,
        type=operation.type,
        partition_index=partition.index,
        volume_name=operation.volume_name,
        object_name=spec.name,
        inserted_sfs_ids=(new_id,),
        allocated_clusters=allocated,
    )


def _logical_waveform_orphan_report(state: _TransactionState) -> WaveformOrphanReport:
    waveforms: list[_CurrentObjectInput] = []
    banks: list[_CurrentObjectInput] = []
    uncertainties: dict[int, list[str]] = defaultdict(list)
    for partition in state.partitions.values():
        placements: dict[int, list[tuple[str, str]]] = defaultdict(list)
        try:
            root_chunks = _directory_chunks(partition.root)
        except ValueError as exc:
            uncertainties[partition.index].append(str(exc))
            root_chunks = []
        for volume_chunk in root_chunks:
            volume_name = _entry_name(volume_chunk)
            if volume_name in {".", "..", "sfserrlog", "sfserram"}:
                continue
            volume = partition.records.get(_entry_link(volume_chunk))
            if volume is None or volume.payload_kind != "directory" or volume.extent_warnings:
                uncertainties[partition.index].append(
                    f"volume {volume_name!r} has an unresolved directory"
                )
                continue
            for category_chunk in _directory_chunks(volume):
                category_name = _entry_name(category_chunk)
                if category_name not in {"SMPL", "SBNK"}:
                    continue
                category = partition.records.get(_entry_link(category_chunk))
                if (
                    category is None
                    or category.payload_kind != "directory"
                    or category.extent_warnings
                ):
                    uncertainties[partition.index].append(
                        f"volume {volume_name!r} {category_name} directory is unresolved"
                    )
                    continue
                for object_chunk in _directory_chunks(category):
                    if _entry_name(object_chunk) in {".", ".."}:
                        continue
                    target_id = _entry_link(object_chunk)
                    if target_id not in partition.records:
                        uncertainties[partition.index].append(
                            f"volume {volume_name!r} {category_name} entry "
                            f"{_entry_name(object_chunk)!r} has no SFS record"
                        )
                        continue
                    placements[target_id].append((volume_name, category_name))

        for record in partition.records.values():
            if record.sfs_id and (record.extent_warnings or record.payload_kind == "unknown"):
                uncertainties[partition.index].append(
                    f"SFS ID {record.sfs_id} has unresolved payload or extents"
                )
            object_type = _object_type(record)
            if object_type not in {"SMPL", "SBNK"}:
                continue
            candidates = placements.get(record.sfs_id, [])
            expected_category = object_type
            exact = len(candidates) == 1 and candidates[0][1] == expected_category
            volume_name = candidates[0][0] if exact else ""
            item = _CurrentObjectInput(
                object_key=f"p{partition.index}:sfs{record.sfs_id}",
                partition_index=partition.index,
                partition_name=partition.name,
                volume_name=volume_name,
                name=sfs_inventory.clean_ascii(record.payload[0x32:0x42]),
                sfs_id=record.sfs_id,
                payload=record.payload,
                has_exact_placement=exact,
            )
            if object_type == "SMPL":
                waveforms.append(item)
            else:
                banks.append(item)
                if not exact:
                    uncertainties[partition.index].append(
                        f"sample bank SFS ID {record.sfs_id} has no exact SBNK placement"
                    )
    return _classify_waveform_inputs(
        source_path=str(state.source),
        waveforms=waveforms,
        banks=banks,
        partition_uncertainties=uncertainties,
        global_uncertainties=[],
    )


@_register("delete_waveform", parser=_parse_delete_waveform_operation)
def _delete_waveform(state: _TransactionState, operation: AlterationOperation) -> OperationReport:
    partition_index = _resolve_partition_index(state, operation.partition_index)
    partition = state.partitions.get(partition_index)
    if partition is None:
        raise ValueError(f"partition index {partition_index} does not exist")
    assert operation.volume_name is not None
    assert operation.waveform_name is not None
    directory, waveform, _entry = _category_object(
        partition,
        operation.volume_name,
        "SMPL",
        operation.waveform_name,
        "SMPL",
    )
    report = _logical_waveform_orphan_report(state)
    matching_rows = [
        row
        for row in report.rows
        if row.partition_index == partition.index
        and row.volume_name == operation.volume_name
        and row.waveform_name == operation.waveform_name
        and row.sfs_id == waveform.sfs_id
    ]
    if len(matching_rows) != 1:
        raise ValueError(
            f"partition {partition.index} volume {operation.volume_name!r} waveform "
            f"{operation.waveform_name!r} has no unique orphan-check row"
        )
    row = matching_rows[0]
    if row.status != WAVEFORM_STATUS_KNOWN_UNREFERENCED:
        detail = row.referencing_sample_banks or row.notes or row.basis
        raise ValueError(
            f"partition {partition.index} volume {operation.volume_name!r} waveform "
            f"{operation.waveform_name!r} is {row.status}, not "
            f"{WAVEFORM_STATUS_KNOWN_UNREFERENCED}: {detail}"
        )
    chunks = [
        chunk
        for chunk in _directory_chunks(directory)
        if not (
            _entry_name(chunk) == operation.waveform_name
            and _entry_link(chunk) == waveform.sfs_id
        )
    ]
    _replace_directory_payload(partition, directory, b"".join(chunks))
    freed = _release_record(partition, waveform)
    state.known_object_edges = [
        edge
        for edge in state.known_object_edges
        if not (edge[0] == partition.index and waveform.sfs_id in edge[1:])
    ]
    return OperationReport(
        id=operation.id,
        type=operation.type,
        partition_index=partition.index,
        volume_name=operation.volume_name,
        object_name=operation.waveform_name,
        removed_sfs_ids=(waveform.sfs_id,),
        freed_clusters=freed,
    )


def _cluster_absolute_offset(partition: _PartitionState, cluster: int) -> int:
    return (partition.start_sector + cluster * partition.sectors_per_cluster) * SECTOR_SIZE


def _write_record(handle: Any, partition: _PartitionState, record: _StoredRecord) -> None:
    remaining = record.payload
    for extent in record.extents:
        capacity = extent.cluster_count * CLUSTER_SIZE
        chunk, remaining = remaining[:capacity], remaining[capacity:]
        handle.seek(_cluster_absolute_offset(partition, extent.cluster_offset))
        handle.write(chunk)
    for index, cluster in enumerate(record.continuation_clusters):
        extent_slice = record.extents[
            index * CONTINUATION_EXTENTS_PER_CLUSTER : (index + 1) * CONTINUATION_EXTENTS_PER_CLUSTER
        ]
        block = bytearray(CLUSTER_SIZE)
        block[0:4] = len(extent_slice).to_bytes(4, "big")
        next_cluster = (
            record.continuation_clusters[index + 1]
            if index + 1 < len(record.continuation_clusters)
            else 0
        )
        block[8:12] = next_cluster.to_bytes(4, "big")
        for extent_index, extent in enumerate(extent_slice):
            offset = 12 + extent_index * 12
            block[offset : offset + 4] = extent.cluster_offset.to_bytes(4, "big")
            block[offset + 4 : offset + 8] = extent.cluster_count.to_bytes(4, "big")
            block[offset + 8 : offset + 12] = extent.byte_count.to_bytes(4, "big")
        handle.seek(_cluster_absolute_offset(partition, cluster))
        handle.write(block)


def _apply_state(state: _TransactionState, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        raise FileExistsError(f"output already exists: {output}")
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            prefix=f".{output.name}.", suffix=".tmp", dir=output.parent, delete=False
        ) as temporary:
            temporary_path = Path(temporary.name)
        shutil.copyfile(state.source, temporary_path)
        with temporary_path.open("r+b") as handle:
            for partition in state.partitions.values():
                for sfs_id in sorted(partition.deleted_ids):
                    handle.seek(partition.index_offset + _index_record_offset(sfs_id))
                    handle.write(b"\x00" * INDEX_RECORD_SIZE)
                for sfs_id in sorted(partition.changed_ids):
                    record = partition.records[sfs_id]
                    handle.seek(partition.index_offset + _index_record_offset(sfs_id))
                    handle.write(record.raw_index)
                    _write_record(handle, partition, record)
                handle.seek(partition.bitmap_offset)
                handle.write(partition.bitmap)
                handle.seek(partition.bitmap_mirror_offset)
                handle.write(partition.bitmap[:SECTOR_SIZE])
            handle.flush()
            os.fsync(handle.fileno())
        _validate_result(temporary_path, state)
        os.replace(temporary_path, output)
        temporary_path = None
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def _validate_result(path: Path, planned: _TransactionState) -> None:
    reloaded = _load_state(path)
    for index, partition in planned.partitions.items():
        actual = reloaded.partitions.get(index)
        if actual is None:
            raise ValueError(f"post-write validation lost partition {index}")
        if set(actual.records) != set(partition.records):
            raise ValueError(f"post-write SFS record set differs in partition {index}")
        if actual.root.payload != partition.root.payload:
            raise ValueError(f"post-write root directory differs in partition {index}")
        if actual.bitmap != partition.bitmap:
            raise ValueError(f"post-write allocation bitmap differs in partition {index}")


def alter_hds(
    source_path: str | Path,
    manifest: AlterationManifest,
    *,
    output_path: str | Path | None = None,
) -> AlterationResult:
    """Plan a transaction, and atomically publish it when an output is supplied."""
    source = Path(source_path)
    state = _load_state(source)
    for operation in manifest.operations:
        report = _HANDLERS[operation.type].apply(state, operation)
        state.reports.append(report)
    output = Path(output_path) if output_path is not None else None
    if output is not None:
        if output.resolve() == source.resolve():
            raise ValueError("alteration output must differ from the source image")
        _apply_state(state, output)
    return AlterationResult(
        source_path=source,
        output_path=output,
        applied=output is not None,
        operations=tuple(state.reports),
    )


__all__ = [
    "ALTERATION_MANIFEST_SCHEMA_VERSION",
    "AlterationManifest",
    "AlterationOperation",
    "AlterationResult",
    "InsertSampleBankSpec",
    "OperationReport",
    "OperationReference",
    "alter_hds",
    "load_alteration_manifest",
    "parse_alteration_manifest",
]
