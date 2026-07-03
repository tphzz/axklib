#!/usr/bin/env python3
"""Yamaha A-Series SFS disk image dumper.

This is intentionally conservative.  It decodes fields that are stable in the
current corpus and preserves unknown areas as raw quality instead of naming
them prematurely.
"""

from __future__ import annotations

import argparse
import gzip
import json
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, cast

MAGIC = b"YAMAHA_dev3"
DEFAULT_SECTOR_SIZE = 512
SUPERBLOCK_SIZE = 512
PARTITION_HEADER_SIZE = 1024
PARTITION_ENTRY_COUNT = 8
PARTITION_ENTRY_SIZE = 8
NODE_SIZE = 72


def be16(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 2], "big")


def be32(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], "big")


def clean_ascii(data: bytes) -> str:
    return data.rstrip(b"\x00 ").decode("ascii", errors="replace")


def hex_sample(data: bytes, limit: int = 128) -> dict[str, object]:
    sample = data[:limit]
    return {
        "size": len(data),
        "sample_size": len(sample),
        "hex": sample.hex(),
        "truncated": len(data) > len(sample),
    }


@dataclass
class ReadOptions:
    max_nodes: int = 128
    max_partitions: int = 8
    include_node_payloads: bool = True


class ImageReader:
    def __init__(self, path: Path):
        self.path = path
        self.is_gzip = path.suffix.lower() == ".gz"
        self._fh: BinaryIO | gzip.GzipFile
        self._fh = gzip.open(path, "rb") if self.is_gzip else path.open("rb")
        self.size: int | None = None if self.is_gzip else path.stat().st_size

    def close(self) -> None:
        self._fh.close()

    def __enter__(self) -> ImageReader:
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def read_at(self, offset: int, size: int) -> bytes:
        if offset < 0:
            raise ValueError(f"negative read offset: {offset}")
        if size < 0:
            raise ValueError(f"negative read size: {size}")
        self._fh.seek(offset)
        return self._fh.read(size)


def field_u32(block: bytes, rel: int, absolute_base: int, name: str) -> dict[str, object]:
    return {
        "name": name,
        "relative_offset": rel,
        "absolute_offset": absolute_base + rel,
        "size": 4,
        "value": be32(block, rel),
        "raw_hex": block[rel : rel + 4].hex(),
    }


def classify_image(first_sector: bytes, path: Path) -> dict[str, object]:
    if first_sector.startswith(MAGIC):
        return {"kind": "yamaha_sfs", "quality": "known"}

    warnings: list[str] = []
    if len(first_sector) >= 512 and first_sector[510:512] == b"\x55\xaa":
        warnings.append("sector has DOS boot signature 55aa")
    if first_sector[:1] in (b"\xeb", b"\xe9"):
        warnings.append("sector starts with x86 DOS boot jump")
    if b"FAT" in first_sector[:96]:
        warnings.append("sector contains FAT marker")
    if path.suffix.lower() == ".ima":
        warnings.append(
            ".IMA suffix is treated as floppy/DOS-like unless Yamaha SFS magic is present"
        )

    return {
        "kind": "non_sfs",
        "quality": "likely" if warnings else "unknown",
        "warnings": warnings or ["missing Yamaha SFS magic"],
        "first_16_hex": first_sector[:16].hex(),
    }


def parse_superblock(block: bytes, sector_index: int, sector_size: int) -> dict[str, object]:
    absolute_base = sector_index * sector_size
    entries = []
    for index in range(PARTITION_ENTRY_COUNT):
        rel = 0x0A8 + index * PARTITION_ENTRY_SIZE
        start_sector = be32(block, rel)
        sector_count = be32(block, rel + 4)
        entries.append(
            {
                "index": index,
                "relative_offset": rel,
                "absolute_offset": absolute_base + rel,
                "start_sector": start_sector,
                "sector_count": sector_count,
                "active": start_sector != 0 or sector_count != 0,
            }
        )

    return {
        "sector_index": sector_index,
        "absolute_offset": absolute_base,
        "tag": clean_ascii(block[:11]),
        "tag_hex": block[:11].hex(),
        "valid_magic": block.startswith(MAGIC),
        "fields": {
            "sector_size_bytes": field_u32(block, 0x09C, absolute_base, "sector_size_bytes"),
            "total_number_of_sectors": field_u32(
                block, 0x0A0, absolute_base, "total_number_of_sectors"
            ),
        },
        "partition_entries": entries,
        "raw_partition_mode_metadata": {
            "relative_offset": 0x080,
            "absolute_offset": absolute_base + 0x080,
            **hex_sample(block[0x080:0x09C]),
        },
    }


def parse_partition_header(
    reader: ImageReader,
    *,
    partition_index: int,
    start_sector: int,
    sector_count: int,
    sector_size: int,
    options: ReadOptions,
) -> dict[str, object]:
    start_offset = start_sector * sector_size
    block = reader.read_at(start_offset, PARTITION_HEADER_SIZE)
    warnings: list[str] = []
    if len(block) < PARTITION_HEADER_SIZE:
        warnings.append("partition header is truncated")
        block = block.ljust(PARTITION_HEADER_SIZE, b"\x00")

    backup_offset = start_offset + PARTITION_HEADER_SIZE
    backup = reader.read_at(backup_offset, PARTITION_HEADER_SIZE)
    backup_matches = len(backup) == PARTITION_HEADER_SIZE and backup == block
    if len(backup) < PARTITION_HEADER_SIZE:
        warnings.append("backup partition header is truncated or unavailable")
    elif not backup_matches:
        warnings.append("backup partition header differs from primary")

    sectors_per_cluster = be32(block, 0x094)
    bitmap_cluster = be32(block, 0x09C)
    directory_cluster = be32(block, 0x0A4)
    cluster_multiplier = sectors_per_cluster if sectors_per_cluster else 2

    bitmap_sector = start_sector + bitmap_cluster * cluster_multiplier
    directory_sector = start_sector + directory_cluster * cluster_multiplier

    partition = {
        "index": partition_index,
        "start_sector": start_sector,
        "sector_count": sector_count,
        "absolute_offset": start_offset,
        "tag": clean_ascii(block[:11]),
        "valid_magic": block.startswith(MAGIC),
        "name": clean_ascii(block[0x040:0x050]),
        "backup_header": {
            "absolute_offset": backup_offset,
            "matches_primary": backup_matches,
        },
        "fields": {
            "content_1": field_u32(block, 0x080, start_offset, "content_1"),
            "content_2": field_u32(block, 0x084, start_offset, "content_2"),
            "number_of_clusters": field_u32(block, 0x090, start_offset, "number_of_clusters"),
            "sectors_per_cluster": field_u32(block, 0x094, start_offset, "sectors_per_cluster"),
            "header_cluster_count_or_related": field_u32(
                block, 0x098, start_offset, "header_cluster_count_or_related"
            ),
            "cluster_offset_to_cluster_bitmap": field_u32(
                block, 0x09C, start_offset, "cluster_offset_to_cluster_bitmap"
            ),
            "unknown_static_0x0a0": field_u32(block, 0x0A0, start_offset, "unknown_static_0x0a0"),
            "cluster_offset_to_directory_index": field_u32(
                block, 0x0A4, start_offset, "cluster_offset_to_directory_index"
            ),
            "unknown_static_0x0a8": field_u32(block, 0x0A8, start_offset, "unknown_static_0x0a8"),
        },
        "derived": {
            "cluster_multiplier_sectors": cluster_multiplier,
            "bitmap_absolute_sector": bitmap_sector,
            "bitmap_absolute_offset": bitmap_sector * sector_size,
            "directory_index_absolute_sector": directory_sector,
            "directory_index_absolute_offset": directory_sector * sector_size,
        },
        "raw_dynamic_header_region": {
            "relative_offset": 0x0AC,
            "absolute_offset": start_offset + 0x0AC,
            **hex_sample(block[0x0AC:0x200], limit=256),
        },
        "warnings": warnings,
    }

    partition["allocation_bitmap"] = parse_bitmap_summary(
        reader,
        start_sector=start_sector,
        sector_size=sector_size,
        bitmap_sector=bitmap_sector,
        directory_sector=directory_sector,
    )
    partition["nodes"] = parse_nodes(
        reader,
        start_sector=start_sector,
        sector_size=sector_size,
        sectors_per_cluster=cluster_multiplier,
        directory_sector=directory_sector,
        options=options,
    )
    return partition


def parse_bitmap_summary(
    reader: ImageReader,
    *,
    start_sector: int,
    sector_size: int,
    bitmap_sector: int,
    directory_sector: int,
) -> dict[str, object]:
    sector_count = max(0, directory_sector - bitmap_sector)
    byte_count = sector_count * sector_size
    summary: dict[str, object] = {
        "absolute_sector": bitmap_sector,
        "absolute_offset": bitmap_sector * sector_size,
        "sector_count_until_directory_index": sector_count,
    }
    if byte_count <= 0:
        summary["warnings"] = ["bitmap length cannot be inferred from directory index offset"]
        return summary

    data = reader.read_at(bitmap_sector * sector_size, min(byte_count, 65536))
    nonzero_offsets = [index for index, value in enumerate(data) if value][:32]
    summary.update(
        {
            "sampled_bytes": len(data),
            "nonzero_byte_count_in_sample": sum(1 for value in data if value),
            "first_nonzero_offsets_in_sample": nonzero_offsets,
            "sample": hex_sample(data, limit=128),
        }
    )
    return summary


def parse_nodes(
    reader: ImageReader,
    *,
    start_sector: int,
    sector_size: int,
    sectors_per_cluster: int,
    directory_sector: int,
    options: ReadOptions,
) -> list[dict[str, object]]:
    index_offset = directory_sector * sector_size
    read_size = options.max_nodes * NODE_SIZE + sector_size
    data = reader.read_at(index_offset, read_size)
    nodes: list[dict[str, object]] = []

    for index in range(options.max_nodes):
        rel = index * NODE_SIZE
        record = data[rel : rel + NODE_SIZE]
        if len(record) < NODE_SIZE:
            break
        if record[:4] == b"\x00\x00\x00\x00":
            break

        cluster_offset = be32(record, 0x0A)
        node_size = be16(record, 0x08)
        data_size = be32(record, 0x12)
        payload_sector = (
            start_sector + cluster_offset * sectors_per_cluster if cluster_offset else 0
        )
        node = {
            "index": index,
            "absolute_offset": index_offset + rel,
            "raw_hex": record.hex(),
            "unknown_1a": be16(record, 0x00),
            "unknown_1b": be16(record, 0x02),
            "unknown_2a": be16(record, 0x04),
            "unknown_2b": be16(record, 0x06),
            "node_size": node_size,
            "cluster_offset": cluster_offset,
            "payload_absolute_sector": payload_sector,
            "payload_absolute_offset": payload_sector * sector_size if payload_sector else 0,
            "unknown_2": be32(record, 0x0E),
            "data_size": data_size,
            "cluster_offset_sampledata_maybe": be32(record, 0x16),
            "unknown_3b": be32(record, 0x1A),
            "unknown_3d_this": be16(record, 0x20),
        }

        if options.include_node_payloads and payload_sector and 0 < node_size <= 65536:
            payload = reader.read_at(payload_sector * sector_size, node_size)
            node["payload_sample"] = hex_sample(payload, limit=128)
            entries = parse_directory_entries_guess(payload)
            if entries:
                node["directory_entries_guess"] = entries

        nodes.append(node)

    return nodes


def parse_directory_entries_guess(payload: bytes) -> list[dict[str, object]]:
    entries: list[dict[str, object]] = []
    for rel in range(0, min(len(payload), 4096), 32):
        entry = payload[rel : rel + 32]
        if len(entry) < 10 or entry[:8] == b"\x00" * 8:
            break
        name_len = be16(entry, 0x02)
        if name_len == 0 or name_len > 24:
            break
        name_raw = entry[0x08 : 0x08 + name_len]
        if not name_raw or any(byte and (byte < 0x20 or byte >= 0x7F) for byte in name_raw):
            break
        entries.append(
            {
                "relative_offset": rel,
                "entry_class_or_flags": be16(entry, 0x00),
                "name_length_including_nul_guess": name_len,
                "link_or_index_guess": be32(entry, 0x04),
                "name": name_raw.rstrip(b"\x00").decode("ascii", errors="replace"),
                "raw_hex": entry.hex(),
            }
        )
    return entries



def _dict_int(row: dict[str, object], key: str, default: int = 0) -> int:
    value = row.get(key, default)
    return value if isinstance(value, int) else default

def parse_image(path: Path, options: ReadOptions | None = None) -> dict[str, object]:
    options = options or ReadOptions()
    with ImageReader(path) as reader:
        first_sector = reader.read_at(0, SUPERBLOCK_SIZE)
        classification = classify_image(first_sector, path)
        warnings: list[str] = []
        result: dict[str, object] = {
            "path": str(path),
            "image_size_bytes": reader.size,
            "container": "gzip" if reader.is_gzip else "raw",
            "classification": classification,
            "warnings": warnings,
        }
        if classification["kind"] != "yamaha_sfs":
            result["warnings"] = cast(list[str], classification.get("warnings", []))
            return result

        superblock0 = parse_superblock(first_sector, 0, DEFAULT_SECTOR_SIZE)
        fields = cast(dict[str, dict[str, object]], superblock0["fields"])
        sector_size_value = fields["sector_size_bytes"].get("value", DEFAULT_SECTOR_SIZE)
        sector_size = sector_size_value if isinstance(sector_size_value, int) else DEFAULT_SECTOR_SIZE
        if sector_size <= 0 or sector_size > 65536:
            warnings.append(f"invalid sector size {sector_size}; using 512 for recovery")
            sector_size = DEFAULT_SECTOR_SIZE

        superblock1_data = reader.read_at(sector_size, SUPERBLOCK_SIZE)
        superblock1 = parse_superblock(
            superblock1_data.ljust(SUPERBLOCK_SIZE, b"\x00"), 1, sector_size
        )

        result.update(
            {
                "sector_size_bytes": sector_size,
                "superblocks": [superblock0, superblock1],
                "superblock_backup_matches_primary": first_sector == superblock1_data,
            }
        )
        if first_sector != superblock1_data:
            warnings.append("sector 1 superblock differs from sector 0")

        entries = cast(list[dict[str, object]], superblock0["partition_entries"])
        partitions: list[dict[str, object]] = []
        for entry in entries[: options.max_partitions]:
            if not entry.get("active"):
                continue
            start_sector = _dict_int(entry, "start_sector")
            sector_count = _dict_int(entry, "sector_count")
            if reader.size is not None:
                partition_end = (start_sector + sector_count) * sector_size
                if partition_end > reader.size:
                    warnings.append(f"partition {entry.get('index', 0)} extends beyond raw image size")
            partitions.append(
                parse_partition_header(
                    reader,
                    partition_index=_dict_int(entry, "index"),
                    start_sector=start_sector,
                    sector_count=sector_count,
                    sector_size=sector_size,
                    options=options,
                )
            )
        result["partitions"] = partitions
        return result


def parse_many(paths: Iterable[Path], options: ReadOptions) -> dict[str, object]:
    images: list[dict[str, object]] = []
    for path in paths:
        try:
            images.append(parse_image(path, options))
        except Exception as exc:  # keep batch scans useful
            images.append(
                {"path": str(path), "error": str(exc), "classification": {"kind": "error"}}
            )
    return {"images": images}


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("images", nargs="+", type=Path, help="SFS disk images to inspect")
    parser.add_argument(
        "--output", "-o", type=Path, help="write JSON to this path instead of stdout"
    )
    parser.add_argument("--pretty", action="store_true", help="pretty-print JSON")
    parser.add_argument(
        "--max-nodes", type=int, default=128, help="maximum index nodes per partition"
    )
    parser.add_argument(
        "--max-partitions", type=int, default=8, help="maximum partition entries to parse"
    )
    parser.add_argument(
        "--no-node-payloads",
        action="store_true",
        help="skip payload samples and directory-entry guesses",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="exit with status 2 when any input is not Yamaha SFS",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)
    options = ReadOptions(
        max_nodes=args.max_nodes,
        max_partitions=args.max_partitions,
        include_node_payloads=not args.no_node_payloads,
    )
    result = parse_many(args.images, options)
    text = json.dumps(result, indent=2 if args.pretty else None, sort_keys=True)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    else:
        print(text)

    if args.strict:
        images = result.get("images", [])
        for image in images if isinstance(images, list) else []:
            if not isinstance(image, dict):
                return 2
            classification = image.get("classification", {})
            if not isinstance(classification, dict) or classification.get("kind") != "yamaha_sfs":
                return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
