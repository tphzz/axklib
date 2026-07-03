"""Yamaha SFS allocation bitmap analysis."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from axklib.containers import sfs_dump as dumper
from axklib.containers import sfs_extents
from axklib.containers import sfs_inventory as inventory
from axklib.containers import sfs_scan as scan_sfs_objects

DIRECT_EXTENT_LIMIT = sfs_extents.DIRECT_EXTENT_LIMIT
EXTENT_SIZE = sfs_extents.EXTENT_SIZE
EXTENT_RECORD_START = sfs_extents.EXTENT_RECORD_START
CONTINUATION_HEADER_SIZE = sfs_extents.CONTINUATION_HEADER_SIZE
MAX_MISMATCH_RANGES_PER_PARTITION = 512


@dataclass
class AllocationExtent:
    source_image: str
    partition_index: int
    sfs_id: int
    record_offset: int
    extent_kind: str
    extent_index: int
    cluster_offset: int
    cluster_count: int
    byte_count: int
    continuation_cluster: int | None


@dataclass
class AllocationMismatchRange:
    source_image: str
    partition_index: int
    direction: str
    start_cluster: int
    end_cluster: int
    cluster_count: int


@dataclass
class AllocationPartitionSummary:
    source_image: str
    partition_index: int
    partition_name: str
    start_sector: int
    sectors_per_cluster: int
    cluster_count: int
    bitmap_offset: int
    index_offset: int
    scanned_index_bytes: int
    valid_index_record_count: int
    invalid_extent_record_count: int
    direct_extent_record_count: int
    continuation_extent_record_count: int
    data_extent_count: int
    continuation_list_cluster_count: int
    stored_used_cluster_count: int
    reconstructed_used_cluster_count: int
    stored_used_not_reconstructed_count: int
    reconstructed_used_not_stored_count: int
    extent_total_mismatch_count: int
    warning_count: int
    warnings: str



def int_mapping_value(mapping: dict[str, object], key: str, default: int = 0) -> int:
    value = mapping.get(key, default)
    return value if isinstance(value, int) else default

def bitmap_byte_count(cluster_count: int) -> int:
    return (cluster_count + 7) // 8


def bitmap_test(data: bytes | bytearray, cluster: int) -> bool:
    byte_index = cluster // 8
    if byte_index >= len(data):
        return False
    return bool(data[byte_index] & (0x80 >> (cluster & 7)))


def bitmap_set(data: bytearray, cluster: int) -> None:
    data[cluster // 8] |= 0x80 >> (cluster & 7)


def count_bitmap_bits(data: bytes | bytearray, cluster_count: int) -> int:
    return sum(1 for cluster in range(cluster_count) if bitmap_test(data, cluster))


def mismatch_ranges(left: bytes | bytearray, right: bytes | bytearray, cluster_count: int) -> list[tuple[int, int]]:
    ranges: list[tuple[int, int]] = []
    start: int | None = None
    for cluster in range(cluster_count):
        differs = bitmap_test(left, cluster) and not bitmap_test(right, cluster)
        if differs and start is None:
            start = cluster
        elif not differs and start is not None:
            ranges.append((start, cluster - 1))
            start = None
    if start is not None:
        ranges.append((start, cluster_count - 1))
    return ranges


def partition_field(partition: dict[str, object], name: str) -> int:
    return int(inventory.field_value(partition, name))


def first_object_offset(image: Path, partition_index: int, fallback: int) -> int:
    rows = scan_sfs_objects.scan_image(image, max_nodes=4)
    return int(inventory.first_object_offset(rows, partition_index, fallback))


def direct_extents(record: bytes | bytearray, extent_count: int) -> list[tuple[int, int, int]]:
    return [
        (extent.cluster_offset, extent.cluster_count, extent.byte_count)
        for extent in sfs_extents.direct_extents(bytes(record), extent_count)
    ]


def read_cluster(
    reader: dumper.ImageReader,
    *,
    partition_start_sector: int,
    sector_size: int,
    sectors_per_cluster: int,
    cluster_offset: int,
) -> bytes:
    return sfs_extents.read_cluster(
        reader,
        partition_start_sector=partition_start_sector,
        sector_size=sector_size,
        sectors_per_cluster=sectors_per_cluster,
        cluster_offset=cluster_offset,
    )


def continuation_extents(
    reader: dumper.ImageReader,
    *,
    partition_start_sector: int,
    sector_size: int,
    sectors_per_cluster: int,
    cluster_count_limit: int,
    list_cluster: int,
    expected_extent_count: int,
) -> tuple[list[tuple[int, int, int]], list[int], list[str]]:
    extents, list_clusters, warnings = sfs_extents.continuation_extents(
        reader,
        partition_start_sector=partition_start_sector,
        sector_size=sector_size,
        sectors_per_cluster=sectors_per_cluster,
        cluster_count_limit=cluster_count_limit,
        list_cluster=list_cluster,
        expected_extent_count=expected_extent_count,
    )
    return [
        (extent.cluster_offset, extent.cluster_count, extent.byte_count)
        for extent in extents
    ], list_clusters, warnings

def add_extent_to_bitmap(
    bitmap: bytearray,
    *,
    cluster_offset: int,
    cluster_count: int,
    cluster_count_limit: int,
) -> int:
    added = 0
    end = min(cluster_offset + cluster_count, cluster_count_limit)
    for cluster in range(max(0, cluster_offset), end):
        if not bitmap_test(bitmap, cluster):
            added += 1
        bitmap_set(bitmap, cluster)
    return added


def range_count(ranges: list[tuple[int, int]]) -> int:
    return sum(end - start + 1 for start, end in ranges)


def analyze_partition(
    image: Path,
    parsed: dict[str, object],
    partition: dict[str, object],
) -> tuple[AllocationPartitionSummary, list[AllocationExtent], list[AllocationMismatchRange]]:
    sector_size = int_mapping_value(parsed, "sector_size_bytes", 512)
    partition_index = int_mapping_value(partition, "index")
    partition_name = str(partition.get("name", ""))
    start_sector = int_mapping_value(partition, "start_sector")
    sectors_per_cluster = partition_field(partition, "sectors_per_cluster") or 2
    total_clusters = partition_field(partition, "number_of_clusters")
    derived = partition.get("derived", {})
    if not isinstance(derived, dict):
        raise ValueError(f"partition {partition_index} lacks derived geometry")
    bitmap_offset = int_mapping_value(derived, "bitmap_absolute_offset")
    index_offset = int_mapping_value(derived, "directory_index_absolute_offset")
    fallback_end = index_offset + 1024 * 1024
    scan_end = first_object_offset(image, partition_index, fallback_end)
    scan_size = max(0, scan_end - index_offset)

    reconstructed = bytearray(bitmap_byte_count(total_clusters))
    extents_report: list[AllocationExtent] = []
    warnings: list[str] = []
    valid_records = 0
    invalid_extent_records = 0
    direct_records = 0
    continuation_records = 0
    data_extent_count = 0
    list_cluster_count = 0
    extent_total_mismatch_count = 0

    with dumper.ImageReader(image) as reader:
        stored_bitmap = reader.read_at(bitmap_offset, bitmap_byte_count(total_clusters))
        index_data = reader.read_at(index_offset, scan_size)
        for rel in inventory.iter_index_record_offsets(len(index_data)):
            sfs_id = inventory.index_record_offset_to_sfs_id(rel)
            if sfs_id is None:
                continue
            record = index_data[rel : rel + inventory.INDEX_RECORD_SIZE]
            parsed_record = inventory.parse_index_record(
                record,
                partition_index=partition_index,
                record_offset=index_offset + rel,
                record_offset_in_index=rel,
                partition_start_sector=start_sector,
                sector_size=sector_size,
                sectors_per_cluster=sectors_per_cluster,
            )
            if parsed_record is None:
                continue
            valid_records += 1

            record_extents: list[tuple[int, int, int]]
            list_clusters: list[int] = []
            if parsed_record.extent_count <= DIRECT_EXTENT_LIMIT:
                direct_records += 1
                record_extents = direct_extents(record, parsed_record.extent_count)
                if len(record_extents) != parsed_record.extent_count:
                    invalid_extent_records += 1
                    warnings.append(f"partition {partition_index} sfs_id {sfs_id}: invalid direct extents")
                    continue
            else:
                continuation_records += 1
                record_extents, list_clusters, extent_warnings = continuation_extents(
                    reader,
                    partition_start_sector=start_sector,
                    sector_size=sector_size,
                    sectors_per_cluster=sectors_per_cluster,
                    cluster_count_limit=total_clusters,
                    list_cluster=parsed_record.cluster_offset,
                    expected_extent_count=parsed_record.extent_count,
                )
                warnings.extend(f"partition {partition_index} sfs_id {sfs_id}: {item}" for item in extent_warnings)
                if len(record_extents) != parsed_record.extent_count:
                    invalid_extent_records += 1

            data_cluster_sum = sum(extent[1] for extent in record_extents)
            if data_cluster_sum != parsed_record.cluster_count:
                extent_total_mismatch_count += 1
                warnings.append(
                    f"partition {partition_index} sfs_id {sfs_id}: extent cluster sum {data_cluster_sum} != record total {parsed_record.cluster_count}"
                )

            for index, list_cluster in enumerate(list_clusters):
                add_extent_to_bitmap(
                    reconstructed,
                    cluster_offset=list_cluster,
                    cluster_count=1,
                    cluster_count_limit=total_clusters,
                )
                list_cluster_count += 1
                extents_report.append(
                    AllocationExtent(
                        source_image=str(image),
                        partition_index=partition_index,
                        sfs_id=sfs_id,
                        record_offset=index_offset + rel,
                        extent_kind="continuation-list",
                        extent_index=index,
                        cluster_offset=list_cluster,
                        cluster_count=1,
                        byte_count=sector_size * sectors_per_cluster,
                        continuation_cluster=list_cluster,
                    )
                )

            for index, (cluster_offset, extent_cluster_count, byte_count) in enumerate(record_extents):
                if cluster_offset + extent_cluster_count > total_clusters:
                    warnings.append(
                        f"partition {partition_index} sfs_id {sfs_id}: extent out of range at cluster {cluster_offset}"
                    )
                add_extent_to_bitmap(
                    reconstructed,
                    cluster_offset=cluster_offset,
                    cluster_count=extent_cluster_count,
                    cluster_count_limit=total_clusters,
                )
                data_extent_count += 1
                extents_report.append(
                    AllocationExtent(
                        source_image=str(image),
                        partition_index=partition_index,
                        sfs_id=sfs_id,
                        record_offset=index_offset + rel,
                        extent_kind="data",
                        extent_index=index,
                        cluster_offset=cluster_offset,
                        cluster_count=extent_cluster_count,
                        byte_count=byte_count,
                        continuation_cluster=list_clusters[-1] if list_clusters else None,
                    )
                )

    stored_not_reconstructed = mismatch_ranges(stored_bitmap, bytes(reconstructed), total_clusters)
    reconstructed_not_stored = mismatch_ranges(bytes(reconstructed), stored_bitmap, total_clusters)
    mismatch_report: list[AllocationMismatchRange] = []
    for direction, ranges in (
        ("stored_used_not_reconstructed", stored_not_reconstructed),
        ("reconstructed_used_not_stored", reconstructed_not_stored),
    ):
        for start, end in ranges[:MAX_MISMATCH_RANGES_PER_PARTITION]:
            mismatch_report.append(
                AllocationMismatchRange(
                    source_image=str(image),
                    partition_index=partition_index,
                    direction=direction,
                    start_cluster=start,
                    end_cluster=end,
                    cluster_count=end - start + 1,
                )
            )
        if len(ranges) > MAX_MISMATCH_RANGES_PER_PARTITION:
            warnings.append(
                f"partition {partition_index}: truncated {direction} ranges from {len(ranges)} to "
                f"{MAX_MISMATCH_RANGES_PER_PARTITION}"
            )

    summary = AllocationPartitionSummary(
        source_image=str(image),
        partition_index=partition_index,
        partition_name=partition_name,
        start_sector=start_sector,
        sectors_per_cluster=sectors_per_cluster,
        cluster_count=total_clusters,
        bitmap_offset=bitmap_offset,
        index_offset=index_offset,
        scanned_index_bytes=scan_size,
        valid_index_record_count=valid_records,
        invalid_extent_record_count=invalid_extent_records,
        direct_extent_record_count=direct_records,
        continuation_extent_record_count=continuation_records,
        data_extent_count=data_extent_count,
        continuation_list_cluster_count=list_cluster_count,
        stored_used_cluster_count=count_bitmap_bits(stored_bitmap, total_clusters),
        reconstructed_used_cluster_count=count_bitmap_bits(bytes(reconstructed), total_clusters),
        stored_used_not_reconstructed_count=range_count(stored_not_reconstructed),
        reconstructed_used_not_stored_count=range_count(reconstructed_not_stored),
        extent_total_mismatch_count=extent_total_mismatch_count,
        warning_count=len(warnings),
        warnings=" | ".join(warnings[:32]),
    )
    return summary, extents_report, mismatch_report




@dataclass(frozen=True)
class AllocationReport:
    summaries: tuple[AllocationPartitionSummary, ...]
    extents: tuple[AllocationExtent, ...]
    mismatches: tuple[AllocationMismatchRange, ...]


def analyze_image(image: Path) -> AllocationReport:
    parsed = dumper.parse_image(image, dumper.ReadOptions(max_nodes=4, include_node_payloads=False))
    classification = parsed.get("classification", {})
    if not isinstance(classification, dict) or classification.get("kind") != "yamaha_sfs":
        raise ValueError(f"{image} is not a Yamaha SFS image")

    partitions = parsed.get("partitions", [])
    if not isinstance(partitions, list):
        partitions = []

    summaries: list[AllocationPartitionSummary] = []
    extents: list[AllocationExtent] = []
    mismatches: list[AllocationMismatchRange] = []
    for partition in partitions:
        if not isinstance(partition, dict):
            continue
        summary, partition_extents, partition_mismatches = analyze_partition(image, parsed, partition)
        summaries.append(summary)
        extents.extend(partition_extents)
        mismatches.extend(partition_mismatches)
    return AllocationReport(
        summaries=tuple(summaries),
        extents=tuple(extents),
        mismatches=tuple(mismatches),
    )
