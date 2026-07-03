"""Shared Yamaha SFS extent helpers."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Protocol

DIRECT_EXTENT_LIMIT = 4
EXTENT_SIZE = 12
EXTENT_RECORD_START = 0x0A
CONTINUATION_HEADER_SIZE = 12


class Reader(Protocol):
    def read_at(self, offset: int, size: int) -> bytes: ...


@dataclass(frozen=True)
class SfsExtent:
    cluster_offset: int
    cluster_count: int
    byte_count: int


@dataclass(frozen=True)
class SfsExtentRead:
    data: bytes
    extents: list[SfsExtent]
    continuation_clusters: list[int]
    warnings: list[str]
    total_data_size: int
    allocated_bytes: int

    @property
    def storage_padding_bytes(self) -> int:
        return max(0, self.allocated_bytes - self.total_data_size)


def be16(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 2], "big")


def be32(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], "big")


def cluster_absolute_offset(
    *,
    partition_start_sector: int,
    sector_size: int,
    sectors_per_cluster: int,
    cluster_offset: int,
) -> int:
    sector = partition_start_sector + cluster_offset * sectors_per_cluster
    return sector * sector_size


def direct_extents(record: bytes, extent_count: int) -> list[SfsExtent]:
    if extent_count <= 0 or extent_count > DIRECT_EXTENT_LIMIT:
        return []
    extents: list[SfsExtent] = []
    for index in range(extent_count):
        offset = EXTENT_RECORD_START + index * EXTENT_SIZE
        if offset + EXTENT_SIZE > len(record):
            return []
        cluster_offset = be32(record, offset)
        cluster_count = be32(record, offset + 4)
        byte_count = be32(record, offset + 8)
        if cluster_offset == 0 or cluster_count == 0 or byte_count == 0:
            return []
        extents.append(SfsExtent(cluster_offset, cluster_count, byte_count))
    return extents


def read_cluster(
    reader: Reader,
    *,
    partition_start_sector: int,
    sector_size: int,
    sectors_per_cluster: int,
    cluster_offset: int,
) -> bytes:
    return reader.read_at(
        cluster_absolute_offset(
            partition_start_sector=partition_start_sector,
            sector_size=sector_size,
            sectors_per_cluster=sectors_per_cluster,
            cluster_offset=cluster_offset,
        ),
        sector_size * sectors_per_cluster,
    )


def continuation_extents(
    reader: Reader,
    *,
    partition_start_sector: int,
    sector_size: int,
    sectors_per_cluster: int,
    cluster_count_limit: int,
    list_cluster: int,
    expected_extent_count: int,
) -> tuple[list[SfsExtent], list[int], list[str]]:
    extents: list[SfsExtent] = []
    list_clusters: list[int] = []
    warnings: list[str] = []
    seen: set[int] = set()
    remaining = expected_extent_count
    max_triplets = (sector_size * sectors_per_cluster - CONTINUATION_HEADER_SIZE) // EXTENT_SIZE

    while list_cluster and remaining > 0:
        if list_cluster in seen:
            warnings.append(f"continuation list loop at cluster {list_cluster}")
            break
        if list_cluster < 0 or list_cluster >= cluster_count_limit:
            warnings.append(f"continuation list cluster out of range: {list_cluster}")
            break
        seen.add(list_cluster)
        list_clusters.append(list_cluster)

        payload = read_cluster(
            reader,
            partition_start_sector=partition_start_sector,
            sector_size=sector_size,
            sectors_per_cluster=sectors_per_cluster,
            cluster_offset=list_cluster,
        )
        block_count = be32(payload, 0)
        next_cluster = be32(payload, 8)
        if block_count <= 0 or block_count > max_triplets:
            warnings.append(
                f"invalid continuation extent count {block_count} at cluster {list_cluster}"
            )
            break
        if block_count > remaining:
            warnings.append(
                f"continuation count {block_count} exceeds remaining {remaining} at cluster {list_cluster}"
            )
            block_count = remaining

        offset = CONTINUATION_HEADER_SIZE
        for _index in range(block_count):
            cluster_offset = be32(payload, offset)
            extent_cluster_count = be32(payload, offset + 4)
            byte_count = be32(payload, offset + 8)
            if cluster_offset == 0 or extent_cluster_count == 0 or byte_count == 0:
                warnings.append(
                    f"invalid continuation triplet at cluster {list_cluster}+0x{offset:x}"
                )
                break
            extents.append(SfsExtent(cluster_offset, extent_cluster_count, byte_count))
            offset += EXTENT_SIZE

        remaining = expected_extent_count - len(extents)
        list_cluster = next_cluster

    if remaining:
        warnings.append(f"missing {remaining} continuation extents")
    return extents, list_clusters, warnings


def extents_for_index_record(
    reader: Reader,
    record: bytes,
    *,
    partition_start_sector: int,
    sector_size: int,
    sectors_per_cluster: int,
    cluster_count_limit: int,
) -> tuple[list[SfsExtent], list[int], list[str]]:
    extent_count = be16(record, 0x00)
    first_cluster = be32(record, 0x0A)
    if extent_count <= DIRECT_EXTENT_LIMIT:
        extents = direct_extents(record, extent_count)
        return extents, [], [] if extents else ["invalid direct extents"]
    return continuation_extents(
        reader,
        partition_start_sector=partition_start_sector,
        sector_size=sector_size,
        sectors_per_cluster=sectors_per_cluster,
        cluster_count_limit=cluster_count_limit,
        list_cluster=first_cluster,
        expected_extent_count=extent_count,
    )


def read_index_record_data(
    reader: Reader,
    record: bytes,
    *,
    partition_start_sector: int,
    sector_size: int,
    sectors_per_cluster: int,
    cluster_count_limit: int,
) -> SfsExtentRead:
    total_data_size = be32(record, 0x06)
    extents, continuation_clusters, warnings = extents_for_index_record(
        reader,
        record,
        partition_start_sector=partition_start_sector,
        sector_size=sector_size,
        sectors_per_cluster=sectors_per_cluster,
        cluster_count_limit=cluster_count_limit,
    )
    cluster_size = sector_size * sectors_per_cluster
    allocated_bytes = sum(extent.cluster_count * cluster_size for extent in extents)

    chunks: list[bytes] = []
    remaining = total_data_size
    for extent in extents:
        if remaining <= 0:
            break
        capacity = extent.cluster_count * cluster_size
        if extent.byte_count > capacity:
            warnings.append(
                f"extent byte count {extent.byte_count} exceeds allocated capacity {capacity} at cluster {extent.cluster_offset}"
            )
        read_size = min(extent.byte_count, capacity, remaining)
        offset = cluster_absolute_offset(
            partition_start_sector=partition_start_sector,
            sector_size=sector_size,
            sectors_per_cluster=sectors_per_cluster,
            cluster_offset=extent.cluster_offset,
        )
        chunks.append(reader.read_at(offset, read_size))
        remaining -= read_size

    data = b"".join(chunks)
    if len(data) != total_data_size:
        warnings.append(f"logical data read {len(data)} bytes, expected {total_data_size}")

    return SfsExtentRead(
        data=data,
        extents=extents,
        continuation_clusters=continuation_clusters,
        warnings=warnings,
        total_data_size=total_data_size,
        allocated_bytes=allocated_bytes,
    )
