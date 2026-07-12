#!/usr/bin/env python3
"""Find SFS object headers in Yamaha A-Series disk images."""

from __future__ import annotations

import argparse
import glob
import wave
from collections.abc import Iterable
from pathlib import Path

from axklib.containers import sfs_dump as dumper
from axklib.objects import current as objects

OBJECT_MAGIC = objects.OBJECT_MAGIC
OBJECT_TYPES = objects.OBJECT_TYPES
DEFAULT_CHUNK_SIZE = 1024 * 1024


def be16(data: bytes, offset: int) -> int:
    return objects.be16(data, offset)


def be32(data: bytes, offset: int) -> int:
    return objects.be32(data, offset)


def clean_ascii(data: bytes) -> str:
    return objects.clean_ascii(data)


def expand_inputs(inputs: Iterable[Path]) -> list[Path]:
    paths: list[Path] = []
    for item in inputs:
        text = str(item)
        if any(char in text for char in "*?["):
            paths.extend(Path(match) for match in glob.glob(text))
        else:
            paths.append(item)
    return sorted(paths)


def iter_magic_offsets(path: Path, chunk_size: int = DEFAULT_CHUNK_SIZE) -> Iterable[int]:
    overlap = len(OBJECT_MAGIC) - 1
    base = 0
    previous = b""
    with dumper.ImageReader(path) as reader:
        while True:
            chunk = reader.read_at(base, chunk_size)
            if not chunk:
                break
            data = previous + chunk
            scan_base = base - len(previous)
            start = 0
            while True:
                index = data.find(OBJECT_MAGIC, start)
                if index < 0:
                    break
                yield scan_base + index
                start = index + 1
            if len(chunk) < chunk_size:
                break
            previous = data[-overlap:]
            base += chunk_size


def field_value(fields: dict[str, object], name: str) -> int | None:
    field = fields.get(name, {})
    if isinstance(field, dict):
        value = field.get("value")
        if isinstance(value, int):
            return value
    return None


def partition_for_offset(parsed: dict[str, object], offset: int) -> dict[str, object] | None:
    sector_size = parsed.get("sector_size_bytes")
    if not isinstance(sector_size, int):
        return None
    partitions = parsed.get("partitions", [])
    if not isinstance(partitions, list):
        return None
    for partition in partitions:
        if not isinstance(partition, dict):
            continue
        start_sector = partition.get("start_sector")
        sector_count = partition.get("sector_count")
        if not isinstance(start_sector, int) or not isinstance(sector_count, int):
            continue
        start = start_sector * sector_size
        end = (start_sector + sector_count) * sector_size
        if start <= offset < end:
            return partition
    return None


def node_for_offset(partition: dict[str, object], offset: int) -> dict[str, object] | None:
    nodes = partition.get("nodes", [])
    if not isinstance(nodes, list):
        return None
    for node in nodes:
        if not isinstance(node, dict):
            continue
        start = node.get("payload_absolute_offset")
        size = node.get("data_size") or node.get("node_size")
        if not isinstance(start, int) or not isinstance(size, int) or size <= 0:
            continue
        if start <= offset < start + size:
            return node
    return None


def summarize_hit(path: Path, parsed: dict[str, object], offset: int) -> dict[str, object]:
    sector_size = parsed.get("sector_size_bytes")
    if not isinstance(sector_size, int):
        sector_size = dumper.DEFAULT_SECTOR_SIZE

    with dumper.ImageReader(path) as reader:
        header = reader.read_at(offset, 128)

    partition = partition_for_offset(parsed, offset)
    partition_index: object = ""
    cluster_offset: object = ""
    byte_in_cluster: object = ""
    node_index: object = ""
    byte_in_node: object = ""
    if partition is not None:
        partition_index = partition.get("index", "")
        fields = partition.get("fields", {})
        if isinstance(fields, dict):
            sectors_per_cluster = field_value(fields, "sectors_per_cluster")
        else:
            sectors_per_cluster = None
        start_sector = partition.get("start_sector")
        if (
            isinstance(start_sector, int)
            and isinstance(sectors_per_cluster, int)
            and sectors_per_cluster
        ):
            relative_sector = offset // sector_size - start_sector
            cluster_offset = relative_sector // sectors_per_cluster
            byte_in_cluster = (
                relative_sector % sectors_per_cluster
            ) * sector_size + offset % sector_size

        node = node_for_offset(partition, offset)
        if node is not None:
            node_index = node.get("index", "")
            node_start = node.get("payload_absolute_offset")
            if isinstance(node_start, int):
                byte_in_node = offset - node_start

    row = {
        "image": str(path),
        "offset": offset,
        "sector": offset // sector_size,
        "sector_offset": offset % sector_size,
        "partition": partition_index,
        "cluster_offset": cluster_offset,
        "byte_in_cluster": byte_in_cluster,
        "node": node_index,
        "byte_in_node": byte_in_node,
    }
    row.update(objects.summarize_object_header(header))
    return row


def scan_image(path: Path, max_nodes: int) -> list[dict[str, object]]:
    parsed = dumper.parse_image(
        path,
        dumper.ReadOptions(max_nodes=max_nodes, include_node_payloads=True),
    )
    classification = parsed.get("classification", {})
    if not isinstance(classification, dict) or classification.get("kind") != "yamaha_sfs":
        return []
    return [summarize_hit(path, parsed, offset) for offset in iter_magic_offsets(path)]


def swap_16bit_words(data: bytes) -> bytes:
    return objects.swap_16bit_words(data)


def load_source_wavs(source_dir: Path) -> dict[str, bytes]:
    sources: dict[str, bytes] = {}
    for path in sorted(source_dir.glob("*.wav")):
        with wave.open(str(path), "rb") as wav:
            sources[path.name] = wav.readframes(wav.getnframes())
    return sources


def add_wav_matches(rows: list[dict[str, object]], source_dir: Path) -> None:
    sources = load_source_wavs(source_dir)
    for row in rows:
        if row.get("type") != "SMPL":
            continue
        size = row.get("payload_bytes_0x1c")
        header_size = row.get("header_size")
        image_path = row.get("image")
        object_offset = row.get("offset")
        if (
            not isinstance(size, int)
            or not isinstance(header_size, int)
            or not isinstance(image_path, str)
            or not isinstance(object_offset, int)
        ):
            continue

        with dumper.ImageReader(Path(image_path)) as reader:
            stored = reader.read_at(object_offset + header_size, size)
        candidates = {
            "raw": stored,
            "byteswap16": swap_16bit_words(stored),
        }
        matches = [
            f"{name}:{transform}"
            for transform, candidate in candidates.items()
            for name, source_pcm in sources.items()
            if candidate == source_pcm
        ]
        row["wav_match"] = ",".join(matches)


def print_table(rows: list[dict[str, object]]) -> None:
    columns = [
        ("image", "image"),
        ("offset", "offset"),
        ("sector", "sector"),
        ("sector_offset", "sec_off"),
        ("partition", "part"),
        ("cluster_offset", "cluster"),
        ("byte_in_cluster", "cl_off"),
        ("node", "node"),
        ("byte_in_node", "node_off"),
        ("type", "type"),
        ("known_type", "known"),
        ("header_size", "hsize"),
        ("payload_bytes_0x1c", "bytes1"),
        ("payload_bytes_0x20", "bytes2"),
        ("sample_rate_guess", "rate?"),
        ("bytes_per_sample_guess", "bps?"),
        ("name_guess", "name?"),
        ("wav_match", "wav_match"),
    ]
    widths = [
        max(len(title), *(len(str(row.get(key, ""))) for row in rows)) for key, title in columns
    ]
    print(
        "  ".join(title.ljust(width) for width, (_key, title) in zip(widths, columns, strict=True))
    )
    print("  ".join("-" * width for width in widths))
    for row in rows:
        print(
            "  ".join(
                str(row.get(key, "")).ljust(width)
                for width, (key, _title) in zip(widths, columns, strict=True)
            )
        )


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("images", nargs="+", type=Path)
    parser.add_argument("--max-nodes", type=int, default=256)
    parser.add_argument("--show-headers", action="store_true")
    parser.add_argument(
        "--match-wavs",
        type=Path,
        help="compare SMPL payloads against source WAV PCM from this directory",
    )
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    rows: list[dict[str, object]] = []
    for path in expand_inputs(args.images):
        rows.extend(scan_image(path, args.max_nodes))
    if args.match_wavs:
        add_wav_matches(rows, args.match_wavs)

    if rows:
        print_table(rows)
    else:
        print("No SFS object headers found.")

    if args.show_headers:
        print()
        print("Header samples:")
        for row in rows:
            offset = row.get("offset", 0)
            offset_int = offset if isinstance(offset, int) else 0
            print(f"{row['image']} @ 0x{offset_int:x}: {row['header_hex']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
