#!/usr/bin/env python3
"""Report Yamaha SFS partition, volume, directory, and object organization."""

from __future__ import annotations

import argparse
import csv
import json
from collections import defaultdict
from collections.abc import Iterator, Sequence
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, cast

from axklib.containers import sfs_dump as dumper
from axklib.containers import sfs_extents, sfs_scan

OBJECT_MAGIC = b"FSFSDEV3SPLX"
OBJECT_PAYLOAD_KINDS = {"object", "alternating-byte-object"}
LEGACY_OBJECT_EVEN_MAGIC = OBJECT_MAGIC[:12:2]
LEGACY_OBJECT_TYPES_BY_EVEN_LANE = {
    b"SP": "SMPL",
    b"SN": "SBNK",
    b"SA": "SBAC",
    b"PO": "PROG",
    b"SQ": "SEQU",
    b"PF": "PRF3",
}
ENTRY_STRIDE = 32
INDEX_RECORD_SIZE = 72
INDEX_BLOCK_SIZE = 1024
INDEX_RECORDS_PER_BLOCK = 14
TYPE_DIR_NAMES = {"SMPL", "SBNK", "SBAC", "SEQU", "PROG"}
YAMAHA_CATEGORY_NAMES = {
    "SMPL": "Samples",
    "SBNK": "Sample Banks",
    "SBAC": "Sample Bank Accessories",
    "SEQU": "Sequences",
    "PROG": "Programs",
}


def be16(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 2], "big")


def be32(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], "big")


def clean_ascii(data: bytes) -> str:
    return data.rstrip(b"\x00 ").decode("ascii", errors="replace")


def printable_ascii(data: bytes) -> str:
    return "".join(chr(byte) if 0x20 <= byte < 0x7F else "." for byte in data)


def has_legacy_alternating_byte_pattern(payload: bytes, byte_count: int = 16) -> bool:
    if len(payload) < byte_count:
        return False
    for offset in range(1, byte_count, 2):
        if payload[offset] != (0x55 if offset % 4 == 1 else 0xAA):
            return False
    return True


def classify_alternating_byte_object_type(payload: bytes) -> str:
    if len(payload) < 16 or not has_legacy_alternating_byte_pattern(payload, 16):
        return ""
    even_lane = payload[:16:2]
    if not even_lane.startswith(LEGACY_OBJECT_EVEN_MAGIC):
        return ""
    return LEGACY_OBJECT_TYPES_BY_EVEN_LANE.get(even_lane[6:8], "")


@dataclass
class IndexRecord:
    partition_index: int
    sfs_id: int
    record_offset: int
    record_offset_in_index: int
    extent_count: int
    cluster_count: int
    data_size: int
    cluster_offset: int
    first_data_cluster_offset: int
    duplicate_cluster_count: int
    duplicate_data_size: int
    extent_list_offset: int | None
    payload_offset: int
    payload_sector: int
    payload_kind: str
    object_type: str
    object_name: str
    directory_id: int | None
    directory_parent_id: int | None
    entry_count: int


@dataclass
class YNodeRecord:
    partition_index: int
    sfs_id: int
    record_offset: int
    record_offset_in_index: int
    extent_count: int
    cluster_count: int
    data_size: int
    cluster_offset: int
    first_data_cluster_offset: int
    duplicate_cluster_count: int
    duplicate_data_size: int
    extent_list_offset: int | None
    payload_offset: int
    payload_sector: int
    payload_kind: str
    object_type: str
    object_name: str
    directory_id: int | None
    directory_parent_id: int | None
    entry_count: int
    payload_prefix_hex: str = ""
    payload_prefix_ascii: str = ""
    visibility: str = "hidden-or-unreferenced"
    link_reference_count: int = 0
    visible_directory_entry_count: int = 0


@dataclass
class DirectoryEntry:
    source_image: str
    partition_index: int
    directory_id: int
    directory_path: str
    entry_offset: int
    entry_flags: int
    name_length_including_nul: int
    link_id: int
    name: str
    target_kind: str
    target_directory_id: int | None
    target_object_type: str
    target_object_offset: int | None
    target_object_name: str
    match_method: str
    target_sfs_id: int | None = None
    target_record_offset: int | None = None
    target_payload_kind: str = ""
    unmatched_reason: str = ""
    match_quality: str = "Unknown"
    candidate_sfs_ids: str = ""
    candidate_record_offsets: str = ""
    candidate_object_offsets: str = ""
    candidate_object_types: str = ""
    candidate_object_names: str = ""


@dataclass
class DirectoryReport:
    source_image: str
    partition_index: int
    directory_id: int
    parent_directory_id: int | None
    path: str
    record_offset: int
    payload_offset: int
    entry_count: int
    child_directory_count: int
    object_entry_count: int


@dataclass
class PartitionReport:
    source_image: str
    partition_index: int
    partition_name: str
    start_sector: int
    sector_count: int
    directory_index_offset: int
    first_object_offset: int
    scanned_index_bytes: int
    directory_record_count: int
    object_record_count: int
    top_level_volume_count: int


@dataclass
class VolumeCategorySummary:
    source_image: str
    partition_index: int
    volume_name: str
    volume_path: str
    category: str
    directory_id: int
    entry_count: int
    object_entry_count: int
    matched_object_count: int


@dataclass
class VolumeReport:
    source_image: str
    partition_index: int
    partition_name: str
    volume_name: str
    volume_path: str
    directory_id: int
    category_count: int
    object_entry_count: int
    matched_object_count: int


@dataclass
class VolumeCategoryReport:
    source_image: str
    partition_index: int
    partition_name: str
    volume_name: str
    volume_path: str
    category_code: str
    category_name: str
    directory_id: int
    entry_count: int
    object_entry_count: int
    matched_object_count: int


@dataclass
class VolumeObjectReport:
    source_image: str
    partition_index: int
    partition_name: str
    volume_name: str
    volume_path: str
    category_code: str
    category_name: str
    directory_id: int
    link_id: int
    entry_name: str
    object_type: str
    object_offset: int | None
    object_name: str
    match_method: str
    match_quality: str
    candidate_sfs_ids: str
    candidate_object_offsets: str
    candidate_object_names: str


def joined(values: list[object]) -> str:
    return ";".join(str(value) for value in values)


def joined_hex(values: list[int]) -> str:
    return ";".join(f"0x{value:x}" for value in values)


def match_quality_for(method: str) -> str:
    if method in {
        "directory-id",
        "link-id",
        "link-id+type",
        "link-id+alternating-byte",
        "link-id+alternating-byte-type",
    }:
        return "Known"
    if method == "link-id-type-mismatch":
        return "Likely"
    if method in {"unmatched", ""}:
        return "Unknown"
    return "Tentative"


def safe_path_component(value: str) -> str:
    value = value.strip() or "unnamed"
    return value.replace("\\", "_").replace("/", "_")


def parse_directory_entries(payload: bytes) -> list[dict[str, object]]:
    entries: list[dict[str, object]] = []
    for rel in range(0, min(len(payload), 65536), ENTRY_STRIDE):
        entry = payload[rel : rel + ENTRY_STRIDE]
        if len(entry) < ENTRY_STRIDE or entry[:8] == b"\x00" * 8:
            break
        name_len = be16(entry, 0x02)
        if name_len == 0 or name_len > 24:
            return [] if not entries else entries
        raw_name = entry[0x08 : 0x08 + name_len]
        if not raw_name or any(byte and (byte < 0x20 or byte >= 0x7F) for byte in raw_name):
            return [] if not entries else entries
        entries.append(
            {
                "relative_offset": rel,
                "flags": be16(entry, 0x00),
                "name_length_including_nul": name_len,
                "link_id": be32(entry, 0x04),
                "name": clean_ascii(raw_name),
            }
        )
    return entries


def directory_dot_id(entries: list[dict[str, object]]) -> int | None:
    if len(entries) < 2:
        return None
    first = entries[0]
    if first.get("name") != ".":
        return None
    link = first.get("link_id")
    return link if isinstance(link, int) else None


def directory_parent_id(entries: list[dict[str, object]]) -> int | None:
    if len(entries) < 2:
        return None
    second = entries[1]
    if second.get("name") != "..":
        return None
    link = second.get("link_id")
    return link if isinstance(link, int) else None


@dataclass
class ParsedIndexRecord:
    extent_count: int
    cluster_count: int
    data_size: int
    cluster_offset: int
    first_extent_cluster_count: int
    first_extent_data_size: int
    payload_sector: int
    payload_offset: int


def parse_index_record(
    record: bytes,
    *,
    partition_index: int,
    record_offset: int,
    record_offset_in_index: int,
    partition_start_sector: int,
    sector_size: int,
    sectors_per_cluster: int,
) -> ParsedIndexRecord | None:
    if len(record) < INDEX_RECORD_SIZE:
        return None
    extent_count = be16(record, 0x00)
    if extent_count <= 0 or be16(record, 0x02) != 0:
        return None
    cluster_count = be16(record, 0x04)
    data_size = be32(record, 0x06)
    cluster_offset = be32(record, 0x0A)
    first_extent_cluster_count = be32(record, 0x0E)
    duplicate_data_size = be32(record, 0x12)
    if cluster_count == 0 or data_size == 0 or cluster_offset == 0:
        return None
    if data_size > cluster_count * sectors_per_cluster * sector_size:
        return None
    payload_sector = partition_start_sector + cluster_offset * sectors_per_cluster
    payload_offset = payload_sector * sector_size
    if first_extent_cluster_count == 0 or duplicate_data_size == 0:
        return None
    return ParsedIndexRecord(
        extent_count=extent_count,
        cluster_count=cluster_count,
        data_size=data_size,
        cluster_offset=cluster_offset,
        first_extent_cluster_count=first_extent_cluster_count,
        first_extent_data_size=duplicate_data_size,
        payload_sector=payload_sector,
        payload_offset=payload_offset,
    )


def index_record_offset_to_sfs_id(record_offset_in_index: int) -> int | None:
    block_index = record_offset_in_index // INDEX_BLOCK_SIZE
    offset_in_block = record_offset_in_index % INDEX_BLOCK_SIZE
    if offset_in_block >= INDEX_RECORDS_PER_BLOCK * INDEX_RECORD_SIZE:
        return None
    if offset_in_block % INDEX_RECORD_SIZE != 0:
        return None
    return block_index * INDEX_RECORDS_PER_BLOCK + offset_in_block // INDEX_RECORD_SIZE


def iter_index_record_offsets(scan_size: int) -> Iterator[int]:
    for block_start in range(0, scan_size, INDEX_BLOCK_SIZE):
        for slot_index in range(INDEX_RECORDS_PER_BLOCK):
            rel = block_start + slot_index * INDEX_RECORD_SIZE
            if rel + INDEX_RECORD_SIZE > scan_size:
                return
            yield rel


def continuation_extents(payload: bytes, expected_count: int) -> list[tuple[int, int, int]]:
    if len(payload) < 12:
        return []
    count = be32(payload, 0)
    if count != expected_count or be32(payload, 4) != 0 or be32(payload, 8) != 0:
        return []
    extents: list[tuple[int, int, int]] = []
    offset = 12
    for _index in range(count):
        if offset + 12 > len(payload):
            return []
        cluster_offset = be32(payload, offset)
        cluster_count = be32(payload, offset + 4)
        byte_count = be32(payload, offset + 8)
        if cluster_offset == 0 or cluster_count == 0 or byte_count == 0:
            return []
        extents.append((cluster_offset, cluster_count, byte_count))
        offset += 12
    return extents


def classify_index_payload(
    reader: dumper.ImageReader,
    parsed: ParsedIndexRecord,
    *,
    partition_start_sector: int,
    sector_size: int,
    sectors_per_cluster: int,
) -> tuple[str, str, str, int | None, int | None, int, int, int | None, int]:
    payload = reader.read_at(parsed.payload_offset, min(parsed.data_size, 0x200))
    payload_kind, object_type, object_name, directory_id, parent_id, entry_count = classify_payload(
        payload
    )
    if payload_kind != "unknown":
        return (
            payload_kind,
            object_type,
            object_name,
            directory_id,
            parent_id,
            entry_count,
            parsed.cluster_offset,
            None,
            parsed.payload_offset,
        )

    continuation_count = be32(payload, 0) if len(payload) >= 24 else 0
    max_continuation_count = (
        sector_size * sectors_per_cluster - sfs_extents.CONTINUATION_HEADER_SIZE
    ) // sfs_extents.EXTENT_SIZE
    if (
        continuation_count <= 0
        or continuation_count > max_continuation_count
        or be32(payload, 4) != 0
    ):
        return "unknown", "", "", None, None, 0, parsed.cluster_offset, None, parsed.payload_offset
    first_data_cluster = be32(payload, sfs_extents.CONTINUATION_HEADER_SIZE)
    first_data_sector = partition_start_sector + first_data_cluster * sectors_per_cluster
    first_data_offset = first_data_sector * sector_size
    first_payload = reader.read_at(first_data_offset, min(parsed.data_size, 0x200))
    payload_kind, object_type, object_name, directory_id, parent_id, entry_count = classify_payload(
        first_payload
    )
    if payload_kind == "unknown":
        return (
            "unknown",
            "",
            "",
            None,
            None,
            0,
            first_data_cluster,
            parsed.payload_offset,
            first_data_offset,
        )
    return (
        payload_kind,
        object_type,
        object_name,
        directory_id,
        parent_id,
        entry_count,
        first_data_cluster,
        parsed.payload_offset,
        first_data_offset,
    )


def classify_payload(payload: bytes) -> tuple[str, str, str, int | None, int | None, int]:
    if payload.startswith(OBJECT_MAGIC):
        return (
            "object",
            payload[0x0C:0x10].decode("ascii", errors="replace"),
            clean_ascii(payload[0x32:0x42]),
            None,
            None,
            0,
        )
    alternating_byte_object_type = classify_alternating_byte_object_type(payload)
    if alternating_byte_object_type:
        return "alternating-byte-object", alternating_byte_object_type, "", None, None, 0
    entries = parse_directory_entries(payload)
    dot_id = directory_dot_id(entries)
    if dot_id is not None:
        return "directory", "", "", dot_id, directory_parent_id(entries), len(entries)
    return "unknown", "", "", None, None, 0


def field_value(partition: dict[str, object], name: str) -> int:
    fields = partition.get("fields", {})
    if not isinstance(fields, dict):
        return 0
    field = fields.get(name, {})
    if not isinstance(field, dict):
        return 0
    value = field.get("value")
    return value if isinstance(value, int) else 0


def int_mapping_value(mapping: dict[str, object], key: str, default: int = 0) -> int:
    value = mapping.get(key, default)
    return value if isinstance(value, int) else default


def first_object_offset(
    object_rows: list[dict[str, object]], partition_index: int, fallback: int
) -> int:
    offsets = [
        offset
        for row in object_rows
        if isinstance((offset := row.get("offset")), int)
        and row.get("partition") == partition_index
    ]
    return min(offsets) if offsets else fallback


def ynode_to_index_record(record: YNodeRecord) -> IndexRecord:
    return IndexRecord(
        partition_index=record.partition_index,
        sfs_id=record.sfs_id,
        record_offset=record.record_offset,
        record_offset_in_index=record.record_offset_in_index,
        extent_count=record.extent_count,
        cluster_count=record.cluster_count,
        data_size=record.data_size,
        cluster_offset=record.cluster_offset,
        first_data_cluster_offset=record.first_data_cluster_offset,
        duplicate_cluster_count=record.duplicate_cluster_count,
        duplicate_data_size=record.duplicate_data_size,
        extent_list_offset=record.extent_list_offset,
        payload_offset=record.payload_offset,
        payload_sector=record.payload_sector,
        payload_kind=record.payload_kind,
        object_type=record.object_type,
        object_name=record.object_name,
        directory_id=record.directory_id,
        directory_parent_id=record.directory_parent_id,
        entry_count=record.entry_count,
    )


def scan_ynode_records(
    image: Path,
    partition: dict[str, object],
    object_rows: list[dict[str, object]],
    *,
    sector_size: int,
) -> list[YNodeRecord]:
    partition_index = int_mapping_value(partition, "index")
    start_sector = int_mapping_value(partition, "start_sector")
    sectors_per_cluster = field_value(partition, "sectors_per_cluster") or 2
    derived = partition.get("derived", {})
    if not isinstance(derived, dict):
        return []
    index_offset = int_mapping_value(derived, "directory_index_absolute_offset")
    fallback_end = index_offset + 1024 * 1024
    scan_end = first_object_offset(object_rows, partition_index, fallback_end)
    scan_size = max(0, scan_end - index_offset)
    records: list[YNodeRecord] = []
    if scan_size <= 0:
        return records

    with dumper.ImageReader(image) as reader:
        data = reader.read_at(index_offset, scan_size)
        for rel in iter_index_record_offsets(len(data)):
            sfs_id = index_record_offset_to_sfs_id(rel)
            if sfs_id is None:
                continue
            record = data[rel : rel + INDEX_RECORD_SIZE]
            parsed = parse_index_record(
                record,
                partition_index=partition_index,
                record_offset=index_offset + rel,
                record_offset_in_index=rel,
                partition_start_sector=start_sector,
                sector_size=sector_size,
                sectors_per_cluster=sectors_per_cluster,
            )
            if parsed is None:
                continue
            (
                payload_kind,
                object_type,
                object_name,
                directory_id,
                parent_id,
                entry_count,
                first_data_cluster,
                extent_list_offset,
                payload_offset,
            ) = classify_index_payload(
                reader,
                parsed,
                partition_start_sector=start_sector,
                sector_size=sector_size,
                sectors_per_cluster=sectors_per_cluster,
            )
            prefix = reader.read_at(payload_offset, 16)
            records.append(
                YNodeRecord(
                    partition_index=partition_index,
                    sfs_id=sfs_id,
                    record_offset=index_offset + rel,
                    record_offset_in_index=rel,
                    extent_count=parsed.extent_count,
                    cluster_count=parsed.cluster_count,
                    data_size=parsed.data_size,
                    cluster_offset=parsed.cluster_offset,
                    first_data_cluster_offset=first_data_cluster,
                    duplicate_cluster_count=parsed.first_extent_cluster_count,
                    duplicate_data_size=parsed.first_extent_data_size,
                    extent_list_offset=extent_list_offset,
                    payload_offset=payload_offset,
                    payload_sector=payload_offset // sector_size,
                    payload_kind=payload_kind,
                    object_type=object_type,
                    object_name=object_name,
                    directory_id=directory_id,
                    directory_parent_id=parent_id,
                    entry_count=entry_count,
                    payload_prefix_hex=prefix.hex(" "),
                    payload_prefix_ascii=printable_ascii(prefix),
                )
            )
    return records


def scan_index_records(
    image: Path,
    partition: dict[str, object],
    object_rows: list[dict[str, object]],
    *,
    sector_size: int,
) -> list[IndexRecord]:
    return [
        ynode_to_index_record(record)
        for record in scan_ynode_records(image, partition, object_rows, sector_size=sector_size)
        if record.payload_kind != "unknown"
    ]


def load_directory_entries(
    image: Path,
    record: IndexRecord,
    partition: dict[str, object] | None = None,
    *,
    sector_size: int = dumper.DEFAULT_SECTOR_SIZE,
) -> list[dict[str, object]]:
    with dumper.ImageReader(image) as reader:
        if partition is not None:
            index_record = reader.read_at(record.record_offset, INDEX_RECORD_SIZE)
            extent_read = sfs_extents.read_index_record_data(
                reader,
                index_record,
                partition_start_sector=int_mapping_value(partition, "start_sector"),
                sector_size=sector_size,
                sectors_per_cluster=field_value(partition, "sectors_per_cluster") or 2,
                cluster_count_limit=field_value(partition, "number_of_clusters") or 0,
            )
            return parse_directory_entries(extent_read.data)
        payload = reader.read_at(record.payload_offset, record.data_size)
    return parse_directory_entries(payload)


def object_type_for_directory(path: str) -> str:
    tail = path.rsplit("/", 1)[-1]
    return tail if tail in TYPE_DIR_NAMES else ""


def choose_object_record(
    *,
    name: str,
    expected_type: str,
    link_id: int,
    records_by_sfs_id: dict[int, IndexRecord],
    ynodes_by_sfs_id: dict[int, YNodeRecord],
    records: list[IndexRecord],
    used_offsets: set[int],
) -> tuple[IndexRecord | None, str, str, YNodeRecord | None, list[IndexRecord]]:
    target_ynode = ynodes_by_sfs_id.get(link_id)
    exact = records_by_sfs_id.get(link_id)
    if exact is not None and exact.payload_kind in OBJECT_PAYLOAD_KINDS:
        if not expected_type or exact.object_type == expected_type:
            if exact.payload_kind == "alternating-byte-object":
                return (
                    exact,
                    "link-id+alternating-byte-type"
                    if expected_type
                    else "link-id+alternating-byte",
                    "",
                    target_ynode,
                    [],
                )
            return exact, "link-id+type" if expected_type else "link-id", "", target_ynode, []
        return exact, "link-id-type-mismatch", "", target_ynode, []

    link_reason = "link-id-missing"
    if target_ynode is not None:
        link_reason = f"link-id-target-{target_ynode.payload_kind}"

    candidates = [
        record
        for record in records
        if record.payload_kind in OBJECT_PAYLOAD_KINDS
        and name
        and record.object_name
        and record.object_name == name
        and (not expected_type or record.object_type == expected_type)
        and record.record_offset not in used_offsets
    ]
    if len(candidates) == 1:
        return (
            None,
            "name+type-candidate" if expected_type else "name-candidate",
            link_reason,
            target_ynode,
            candidates,
        )
    if candidates:
        return (
            None,
            "name+type-ambiguous" if expected_type else "name-ambiguous",
            link_reason,
            target_ynode,
            sorted(
                candidates,
                key=lambda item: item.record_offset,
            ),
        )
    fallback = [
        record
        for record in records
        if record.payload_kind in OBJECT_PAYLOAD_KINDS
        and name
        and record.object_name
        and record.object_name == name
        and record.record_offset not in used_offsets
    ]
    if len(fallback) == 1:
        return None, "name-fallback-candidate", link_reason, target_ynode, fallback
    if fallback:
        return (
            None,
            "name-fallback-ambiguous",
            link_reason,
            target_ynode,
            sorted(
                fallback,
                key=lambda item: item.record_offset,
            ),
        )
    return None, "unmatched", link_reason, target_ynode, []


def walk_directories(
    image: Path,
    partition: dict[str, object],
    records: list[IndexRecord],
    ynodes: list[YNodeRecord],
    *,
    sector_size: int = dumper.DEFAULT_SECTOR_SIZE,
) -> tuple[list[DirectoryReport], list[DirectoryEntry]]:
    partition_index = int_mapping_value(partition, "index")
    directories = {
        record.directory_id: record for record in records if record.payload_kind == "directory"
    }
    object_records = [record for record in records if record.payload_kind in OBJECT_PAYLOAD_KINDS]
    records_by_sfs_id = {record.sfs_id: record for record in records}
    ynodes_by_sfs_id = {record.sfs_id: record for record in ynodes}
    used_object_offsets: set[int] = set()
    directory_reports: list[DirectoryReport] = []
    entry_reports: list[DirectoryEntry] = []

    queue: list[tuple[int, str]] = [(1, "/")]
    seen_dirs: set[int] = set()
    while queue:
        directory_id, path = queue.pop(0)
        if directory_id in seen_dirs:
            continue
        seen_dirs.add(directory_id)
        record = directories.get(directory_id)
        if record is None:
            continue

        entries = load_directory_entries(image, record, partition, sector_size=sector_size)
        child_dirs = 0
        object_entries = 0
        expected_type = object_type_for_directory(path)
        for entry in entries:
            name = str(entry["name"])
            if name in {".", ".."}:
                continue
            link_id = int_mapping_value(entry, "link_id")
            child_record = directories.get(link_id)
            target_kind = "directory" if child_record is not None else "object"
            object_record = None
            match_method = ""
            unmatched_reason = ""
            target_ynode = ynodes_by_sfs_id.get(link_id)
            candidate_records: list[IndexRecord] = []
            target_path = (
                f"{path.rstrip('/')}/{safe_path_component(name)}"
                if path != "/"
                else f"/{safe_path_component(name)}"
            )
            if child_record is not None:
                child_dirs += 1
                queue.append((link_id, target_path))
            else:
                object_entries += 1
                object_record, match_method, unmatched_reason, target_ynode, candidate_records = (
                    choose_object_record(
                        name=name,
                        expected_type=expected_type,
                        link_id=link_id,
                        records_by_sfs_id=records_by_sfs_id,
                        ynodes_by_sfs_id=ynodes_by_sfs_id,
                        records=object_records,
                        used_offsets=used_object_offsets,
                    )
                )
                if object_record is not None:
                    used_object_offsets.add(object_record.record_offset)
            match_quality = "directory-id" if child_record is not None else match_method
            entry_reports.append(
                DirectoryEntry(
                    source_image=str(image),
                    partition_index=partition_index,
                    directory_id=directory_id,
                    directory_path=path,
                    entry_offset=int_mapping_value(entry, "relative_offset"),
                    entry_flags=int_mapping_value(entry, "flags"),
                    name_length_including_nul=int_mapping_value(entry, "name_length_including_nul"),
                    link_id=link_id,
                    name=name,
                    target_kind=target_kind,
                    target_directory_id=link_id if child_record is not None else None,
                    target_object_type=object_record.object_type if object_record else "",
                    target_object_offset=object_record.payload_offset if object_record else None,
                    target_object_name=object_record.object_name if object_record else "",
                    match_method=match_quality,
                    target_sfs_id=(
                        child_record.sfs_id
                        if child_record is not None
                        else object_record.sfs_id
                        if object_record is not None
                        else target_ynode.sfs_id
                        if target_ynode is not None
                        else None
                    ),
                    target_record_offset=(
                        child_record.record_offset
                        if child_record is not None
                        else object_record.record_offset
                        if object_record is not None
                        else target_ynode.record_offset
                        if target_ynode is not None
                        else None
                    ),
                    target_payload_kind=(
                        child_record.payload_kind
                        if child_record is not None
                        else object_record.payload_kind
                        if object_record is not None
                        else target_ynode.payload_kind
                        if target_ynode is not None
                        else ""
                    ),
                    unmatched_reason=unmatched_reason,
                    match_quality=match_quality_for(match_quality),
                    candidate_sfs_ids=joined([record.sfs_id for record in candidate_records]),
                    candidate_record_offsets=joined_hex(
                        [record.record_offset for record in candidate_records]
                    ),
                    candidate_object_offsets=joined_hex(
                        [record.payload_offset for record in candidate_records]
                    ),
                    candidate_object_types=joined(
                        [record.object_type for record in candidate_records]
                    ),
                    candidate_object_names=joined(
                        [record.object_name for record in candidate_records]
                    ),
                )
            )

        directory_reports.append(
            DirectoryReport(
                source_image=str(image),
                partition_index=partition_index,
                directory_id=directory_id,
                parent_directory_id=record.directory_parent_id,
                path=path,
                record_offset=record.record_offset,
                payload_offset=record.payload_offset,
                entry_count=len(entries),
                child_directory_count=child_dirs,
                object_entry_count=object_entries,
            )
        )
    return directory_reports, entry_reports


def dataclass_asdict(row: object) -> dict[str, Any]:
    return cast(dict[str, Any], asdict(row))  # type: ignore[call-overload]


def write_csv(path: Path, rows: Sequence[object]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fields = list(dataclass_asdict(rows[0]).keys())
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow(dataclass_asdict(row))


def write_json(path: Path, rows: Sequence[object]) -> None:
    path.write_text(
        json.dumps([dataclass_asdict(row) for row in rows], indent=2) + "\n", encoding="utf-8"
    )


def update_ynode_visibility(
    ynodes: list[YNodeRecord],
    entries: list[DirectoryEntry],
    directories: list[DirectoryReport] | None = None,
) -> None:
    reference_counts: dict[int, int] = defaultdict(int)
    visible_counts: dict[int, int] = defaultdict(int)
    walked_directory_ids = {directory.directory_id for directory in directories or []}
    for entry in entries:
        reference_counts[entry.link_id] += 1
        if entry.name not in {".", ".."}:
            visible_counts[entry.link_id] += 1

    for record in ynodes:
        record.link_reference_count = reference_counts[record.sfs_id]
        record.visible_directory_entry_count = visible_counts[record.sfs_id]
        if record.sfs_id in walked_directory_ids:
            record.visibility = "visible"
        elif record.sfs_id == 0 and record.link_reference_count == 0:
            record.visibility = "hidden-system"
        elif record.link_reference_count == 0:
            record.visibility = "hidden-or-unreferenced"
        elif record.payload_kind == "unknown":
            record.visibility = "referenced-unknown"
        elif record.visible_directory_entry_count == 0:
            record.visibility = "self-or-parent-reference-only"
        else:
            record.visibility = "visible"


def write_summary(
    path: Path,
    *,
    partitions: list[PartitionReport],
    directories: list[DirectoryReport],
    entries: list[DirectoryEntry],
    records: list[IndexRecord],
    ynodes: list[YNodeRecord],
) -> None:
    by_type: dict[str, int] = defaultdict(int)
    legacy_by_type: dict[str, int] = defaultdict(int)
    for record in records:
        if record.payload_kind == "object":
            by_type[record.object_type] += 1
        elif record.payload_kind == "alternating-byte-object":
            legacy_by_type[record.object_type] += 1
    summary = {
        "partition_count": len(partitions),
        "directory_count": len(directories),
        "directory_entry_count": len(entries),
        "index_record_count": len(records),
        "valid_ynode_record_count": len(ynodes),
        "unknown_ynode_record_count": sum(
            1 for record in ynodes if record.payload_kind == "unknown"
        ),
        "legacy_object_record_count": sum(
            1 for record in records if record.payload_kind == "alternating-byte-object"
        ),
        "hidden_system_ynode_count": sum(
            1 for record in ynodes if record.visibility == "hidden-system"
        ),
        "hidden_or_unreferenced_ynode_count": sum(
            1 for record in ynodes if record.visibility == "hidden-or-unreferenced"
        ),
        "referenced_unknown_ynode_count": sum(
            1 for record in ynodes if record.visibility == "referenced-unknown"
        ),
        "object_record_count": sum(1 for record in records if record.payload_kind == "object"),
        "directory_record_count": sum(
            1 for record in records if record.payload_kind == "directory"
        ),
        "object_records_by_type": dict(sorted(by_type.items())),
        "legacy_object_records_by_type": dict(sorted(legacy_by_type.items())),
        "top_level_volumes": [
            entry.name
            for entry in entries
            if entry.directory_id == 1 and entry.name not in {".", "..", "sfserrlog", "sfserram"}
        ],
        "unmatched_object_entries": sum(
            1
            for entry in entries
            if entry.target_kind == "object" and not entry.target_object_offset
        ),
        "unmatched_object_entries_by_reason": dict(
            sorted(
                (reason, count)
                for reason, count in (
                    (reason, sum(1 for entry in entries if entry.unmatched_reason == reason))
                    for reason in {
                        entry.unmatched_reason for entry in entries if entry.unmatched_reason
                    }
                )
            )
        ),
    }
    path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


def volume_category_summaries(
    directories: list[DirectoryReport],
    entries: list[DirectoryEntry],
) -> list[VolumeCategorySummary]:
    entries_by_directory: dict[tuple[int, int], list[DirectoryEntry]] = defaultdict(list)
    for entry in entries:
        entries_by_directory[(entry.partition_index, entry.directory_id)].append(entry)

    rows: list[VolumeCategorySummary] = []
    for directory in directories:
        parts = [part for part in directory.path.split("/") if part]
        if len(parts) != 2 or parts[1] not in TYPE_DIR_NAMES:
            continue
        directory_entries = entries_by_directory[
            (directory.partition_index, directory.directory_id)
        ]
        rows.append(
            VolumeCategorySummary(
                source_image=directory.source_image,
                partition_index=directory.partition_index,
                volume_name=parts[0],
                volume_path="/" + parts[0],
                category=parts[1],
                directory_id=directory.directory_id,
                entry_count=directory.entry_count,
                object_entry_count=directory.object_entry_count,
                matched_object_count=sum(
                    1 for entry in directory_entries if entry.target_object_offset is not None
                ),
            )
        )
    return rows


def yamaha_volume_reports(
    partitions: list[PartitionReport],
    directories: list[DirectoryReport],
    entries: list[DirectoryEntry],
) -> tuple[list[VolumeReport], list[VolumeCategoryReport], list[VolumeObjectReport]]:
    partition_names = {row.partition_index: row.partition_name for row in partitions}
    entries_by_directory: dict[tuple[int, int], list[DirectoryEntry]] = defaultdict(list)
    directories_by_key = {(row.partition_index, row.directory_id): row for row in directories}
    for entry in entries:
        entries_by_directory[(entry.partition_index, entry.directory_id)].append(entry)

    volume_dirs: list[DirectoryReport] = []
    for directory in directories:
        parts = [part for part in directory.path.split("/") if part]
        if len(parts) == 1:
            volume_dirs.append(directory)

    volume_rows: list[VolumeReport] = []
    category_rows: list[VolumeCategoryReport] = []
    object_rows: list[VolumeObjectReport] = []
    for volume in volume_dirs:
        volume_name = volume.path.strip("/")
        partition_name = partition_names.get(volume.partition_index, "")
        category_count = 0
        object_entry_count = 0
        matched_object_count = 0
        for entry in entries_by_directory[(volume.partition_index, volume.directory_id)]:
            if entry.target_kind != "directory" or entry.name not in TYPE_DIR_NAMES:
                continue
            category_directory = directories_by_key.get(
                (volume.partition_index, entry.target_directory_id or -1)
            )
            if category_directory is None:
                continue
            category_entries = entries_by_directory[
                (volume.partition_index, category_directory.directory_id)
            ]
            category_object_entries = [
                item
                for item in category_entries
                if item.target_kind == "object" and item.name not in {".", ".."}
            ]
            category_matched = sum(
                1 for item in category_object_entries if item.target_object_offset is not None
            )
            category_count += 1
            object_entry_count += len(category_object_entries)
            matched_object_count += category_matched
            category_name = YAMAHA_CATEGORY_NAMES.get(entry.name, entry.name)
            category_rows.append(
                VolumeCategoryReport(
                    source_image=volume.source_image,
                    partition_index=volume.partition_index,
                    partition_name=partition_name,
                    volume_name=volume_name,
                    volume_path=volume.path,
                    category_code=entry.name,
                    category_name=category_name,
                    directory_id=category_directory.directory_id,
                    entry_count=category_directory.entry_count,
                    object_entry_count=len(category_object_entries),
                    matched_object_count=category_matched,
                )
            )
            for item in category_object_entries:
                object_rows.append(
                    VolumeObjectReport(
                        source_image=volume.source_image,
                        partition_index=volume.partition_index,
                        partition_name=partition_name,
                        volume_name=volume_name,
                        volume_path=volume.path,
                        category_code=entry.name,
                        category_name=category_name,
                        directory_id=category_directory.directory_id,
                        link_id=item.link_id,
                        entry_name=item.name,
                        object_type=item.target_object_type,
                        object_offset=item.target_object_offset,
                        object_name=item.target_object_name,
                        match_method=item.match_method,
                        match_quality=item.match_quality,
                        candidate_sfs_ids=item.candidate_sfs_ids,
                        candidate_object_offsets=item.candidate_object_offsets,
                        candidate_object_names=item.candidate_object_names,
                    )
                )
        volume_rows.append(
            VolumeReport(
                source_image=volume.source_image,
                partition_index=volume.partition_index,
                partition_name=partition_name,
                volume_name=volume_name,
                volume_path=volume.path,
                directory_id=volume.directory_id,
                category_count=category_count,
                object_entry_count=object_entry_count,
                matched_object_count=matched_object_count,
            )
        )
    return volume_rows, category_rows, object_rows


def build_inventory(image: Path, output_dir: Path) -> None:
    parsed = dumper.parse_image(image, dumper.ReadOptions(max_nodes=4, include_node_payloads=False))
    sector_size_value = parsed.get("sector_size_bytes", dumper.DEFAULT_SECTOR_SIZE)
    sector_size = (
        sector_size_value if isinstance(sector_size_value, int) else dumper.DEFAULT_SECTOR_SIZE
    )
    scan_object_rows = sfs_scan.scan_image(image, max_nodes=4)
    parsed_partitions = parsed.get("partitions", [])
    if not isinstance(parsed_partitions, list):
        parsed_partitions = []

    partition_reports: list[PartitionReport] = []
    all_records: list[IndexRecord] = []
    all_ynodes: list[YNodeRecord] = []
    all_directories: list[DirectoryReport] = []
    all_entries: list[DirectoryEntry] = []
    for partition in parsed_partitions:
        if not isinstance(partition, dict):
            continue
        partition_index = int_mapping_value(partition, "index")
        ynodes = scan_ynode_records(image, partition, scan_object_rows, sector_size=sector_size)
        records = [
            ynode_to_index_record(record) for record in ynodes if record.payload_kind != "unknown"
        ]
        directories, entries = walk_directories(
            image, partition, records, ynodes, sector_size=sector_size
        )
        update_ynode_visibility(ynodes, entries, directories)
        derived = partition.get("derived", {})
        index_offset = (
            int(derived.get("directory_index_absolute_offset", 0))
            if isinstance(derived, dict)
            else 0
        )
        first_offset = first_object_offset(scan_object_rows, partition_index, index_offset)
        partition_reports.append(
            PartitionReport(
                source_image=str(image),
                partition_index=partition_index,
                partition_name=str(partition.get("name", "")),
                start_sector=int_mapping_value(partition, "start_sector"),
                sector_count=int_mapping_value(partition, "sector_count"),
                directory_index_offset=index_offset,
                first_object_offset=first_offset,
                scanned_index_bytes=max(0, first_offset - index_offset),
                directory_record_count=sum(
                    1 for record in records if record.payload_kind == "directory"
                ),
                object_record_count=sum(1 for record in records if record.payload_kind == "object"),
                top_level_volume_count=sum(
                    1
                    for entry in entries
                    if entry.directory_id == 1
                    and entry.name not in {".", "..", "sfserrlog", "sfserram"}
                ),
            )
        )
        all_ynodes.extend(ynodes)
        all_records.extend(records)
        all_directories.extend(directories)
        all_entries.extend(entries)

    output_dir.mkdir(parents=True, exist_ok=True)
    write_csv(output_dir / "partitions.csv", partition_reports)
    write_json(output_dir / "partitions.json", partition_reports)
    write_csv(output_dir / "index_records.csv", all_records)
    write_json(output_dir / "index_records.json", all_records)
    write_csv(output_dir / "ynode_records.csv", all_ynodes)
    write_json(output_dir / "ynode_records.json", all_ynodes)
    write_csv(output_dir / "directories.csv", all_directories)
    write_json(output_dir / "directories.json", all_directories)
    write_csv(output_dir / "directory_entries.csv", all_entries)
    write_json(output_dir / "directory_entries.json", all_entries)
    category_summaries = volume_category_summaries(all_directories, all_entries)
    write_csv(output_dir / "volume_category_summary.csv", category_summaries)
    write_json(output_dir / "volume_category_summary.json", category_summaries)
    volume_rows, category_rows, volume_object_rows = yamaha_volume_reports(
        partition_reports,
        all_directories,
        all_entries,
    )
    write_csv(output_dir / "volumes.csv", volume_rows)
    write_json(output_dir / "volumes.json", volume_rows)
    write_csv(output_dir / "volume_categories.csv", category_rows)
    write_json(output_dir / "volume_categories.json", category_rows)
    write_csv(output_dir / "volume_objects.csv", volume_object_rows)
    write_json(output_dir / "volume_objects.json", volume_object_rows)
    write_summary(
        output_dir / "summary.json",
        partitions=partition_reports,
        directories=all_directories,
        entries=all_entries,
        records=all_records,
        ynodes=all_ynodes,
    )


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--output-dir", "-o", type=Path, required=True)
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    build_inventory(args.image, args.output_dir)
    summary = json.loads((args.output_dir / "summary.json").read_text(encoding="utf-8"))
    print(f"partitions: {summary['partition_count']}")
    print(f"directories: {summary['directory_count']}")
    print(f"directory entries: {summary['directory_entry_count']}")
    print(f"object records: {summary['object_record_count']}")
    print(f"top-level volumes: {len(summary['top_level_volumes'])}")
    print(f"unmatched object entries: {summary['unmatched_object_entries']}")
    print(f"reports written to {args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
