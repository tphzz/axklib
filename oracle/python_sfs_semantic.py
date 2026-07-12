"""Emit the bounded canonical SFS contract used for C++ differential tests."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from axklib.containers import sfs_allocation, sfs_dump, sfs_inventory


def _field(partition: dict[str, object], name: str) -> int:
    return int(sfs_inventory.field_value(partition, name))


def _int(value: object, context: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise ValueError(f"{context} must be an integer")
    return value


def _required_int(value: int | None, context: str) -> int:
    if value is None:
        raise ValueError(f"{context} is unavailable")
    return value


def semantic_value(path: Path) -> dict[str, object]:
    parsed = sfs_dump.parse_image(
        path,
        sfs_dump.ReadOptions(max_nodes=0, include_node_payloads=False),
    )
    classification = parsed.get("classification")
    if not isinstance(classification, dict) or classification.get("kind") != "yamaha_sfs":
        raise ValueError(f"{path} is not a Yamaha SFS image")
    sector_size = _int(parsed["sector_size_bytes"], "sector size")
    allocation_by_partition = {
        item.partition_index: item for item in sfs_allocation.analyze_image(path).summaries
    }
    partitions: list[dict[str, object]] = []
    raw_partitions = parsed.get("partitions")
    if not isinstance(raw_partitions, list):
        raise ValueError(f"{path} has no SFS partition list")
    for raw_partition in raw_partitions:
        if not isinstance(raw_partition, dict):
            continue
        partition_index = _int(raw_partition["index"], "partition index")
        index_span = _field(raw_partition, "unknown_static_0x0a8")
        sectors_per_cluster = _field(raw_partition, "sectors_per_cluster")
        max_sfs_id = index_span * sectors_per_cluster * sector_size // 1024 * 14
        rows = [
            row
            for row in sfs_inventory.scan_ynode_records(
                path,
                raw_partition,
                [],
                sector_size=sector_size,
            )
            if row.sfs_id < max_sfs_id
        ]
        records: list[dict[str, object]] = []
        for row in rows:
            entries: list[dict[str, object]] = []
            if row.payload_kind == "directory":
                record = sfs_inventory.ynode_to_index_record(row)
                entries = [
                    {
                        "flags": _int(entry["flags"], "directory flags"),
                        "link_id": _int(entry["link_id"], "directory link ID"),
                        "name": str(entry["name"]),
                    }
                    for entry in sfs_inventory.load_directory_entries(
                        path,
                        record,
                        raw_partition,
                        sector_size=sector_size,
                    )
                ]
            records.append(
                {
                    "sfs_id": row.sfs_id,
                    "extent_count": row.extent_count,
                    "cluster_count": row.cluster_count,
                    "data_size": row.data_size,
                    "payload_kind": row.payload_kind,
                    "object_type": row.object_type,
                    "object_name": row.object_name,
                    "directory_id": row.directory_id,
                    "parent_directory_id": row.directory_parent_id,
                    "directory_entries": entries,
                }
            )
        allocation = allocation_by_partition[partition_index]
        free_space: dict[str, int] | None = None
        if allocation.sampler_free_cluster_count is not None:
            free_space = {
                "reserved_cluster_count": _required_int(
                    allocation.reserved_cluster_count, "reserved cluster count"
                ),
                "allocated_cluster_count": allocation.stored_used_cluster_count,
                "free_cluster_count": allocation.sampler_free_cluster_count,
                "free_bytes": _required_int(allocation.sampler_free_bytes, "free bytes"),
                "sampler_visible_free_kib": _required_int(
                    allocation.sampler_visible_free_kib, "sampler-visible free KiB"
                ),
            }
        partitions.append(
            {
                "index": partition_index,
                "name": str(raw_partition["name"]),
                "start_sector": _int(raw_partition["start_sector"], "partition start"),
                "sector_count": _int(raw_partition["sector_count"], "partition size"),
                "cluster_count": _field(raw_partition, "number_of_clusters"),
                "sectors_per_cluster": sectors_per_cluster,
                "bitmap_cluster": _field(
                    raw_partition, "cluster_offset_to_cluster_bitmap"
                ),
                "directory_index_cluster": _field(
                    raw_partition, "cluster_offset_to_directory_index"
                ),
                "directory_index_span_clusters": index_span,
                "backup_header_matches": bool(
                    raw_partition["backup_header"]["matches_primary"]
                ),
                "records": records,
                "allocation": {
                    "stored_used_cluster_count": allocation.stored_used_cluster_count,
                    "reconstructed_used_cluster_count": (
                        allocation.reconstructed_used_cluster_count
                    ),
                    "invalid_extent_record_count": allocation.invalid_extent_record_count,
                    "extent_total_mismatch_count": allocation.extent_total_mismatch_count,
                    "free_space": free_space,
                },
            }
        )
    superblocks = parsed.get("superblocks")
    if not isinstance(superblocks, list) or not superblocks:
        raise ValueError(f"{path} has no parsed superblock")
    fields = superblocks[0]["fields"]
    return {
        "schema_version": "1.0",
        "container": "yamaha_sfs",
        "image_size_bytes": path.stat().st_size,
        "sector_size_bytes": sector_size,
        "total_sector_count": int(fields["total_number_of_sectors"]["value"]),
        "backup_superblock_matches": bool(parsed["superblock_backup_matches_primary"]),
        "partitions": partitions,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--pretty", action="store_true")
    args = parser.parse_args(argv)
    value: dict[str, Any] = semantic_value(args.image)
    print(json.dumps(value, indent=2 if args.pretty else None, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
