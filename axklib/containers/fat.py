#!/usr/bin/env python3
"""Report and optionally extract Yamaha A-Series objects from FAT12 floppies."""

from __future__ import annotations

import argparse
import csv
import glob
import json
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

from axklib.objects import current as objects


@dataclass(frozen=True)
class FatGeometry:
    bytes_per_sector: int
    sectors_per_cluster: int
    reserved_sectors: int
    fat_count: int
    root_entries: int
    total_sectors: int
    media_descriptor: int
    sectors_per_fat: int
    sectors_per_track: int
    heads: int
    fat_offset: int
    root_offset: int
    data_offset: int
    root_dir_sectors: int

    @property
    def cluster_size(self) -> int:
        return self.bytes_per_sector * self.sectors_per_cluster


@dataclass(frozen=True)
class FatFile:
    name: str
    directory_offset: int
    first_cluster: int
    size: int


def le16(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 2], "little")


def le32(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], "little")


def expand_inputs(inputs: Iterable[Path]) -> list[Path]:
    paths: list[Path] = []
    for item in inputs:
        text = str(item)
        if any(char in text for char in "*?["):
            paths.extend(Path(match) for match in glob.glob(text))
        else:
            paths.append(item)
    return sorted(paths)


def parse_geometry(image: bytes) -> FatGeometry:
    if len(image) < 512:
        raise ValueError("image is too small to contain a FAT boot sector")

    bytes_per_sector = le16(image, 0x0B)
    sectors_per_cluster = image[0x0D]
    reserved_sectors = le16(image, 0x0E)
    fat_count = image[0x10]
    root_entries = le16(image, 0x11)
    total_sectors = le16(image, 0x13) or le32(image, 0x20)
    media_descriptor = image[0x15]
    sectors_per_fat = le16(image, 0x16)
    sectors_per_track = le16(image, 0x18)
    heads = le16(image, 0x1A)

    if bytes_per_sector <= 0 or sectors_per_cluster <= 0 or sectors_per_fat <= 0:
        raise ValueError("invalid or unsupported FAT geometry")

    root_dir_sectors = (root_entries * 32 + bytes_per_sector - 1) // bytes_per_sector
    fat_offset = reserved_sectors * bytes_per_sector
    root_offset = (reserved_sectors + fat_count * sectors_per_fat) * bytes_per_sector
    data_offset = root_offset + root_dir_sectors * bytes_per_sector

    return FatGeometry(
        bytes_per_sector=bytes_per_sector,
        sectors_per_cluster=sectors_per_cluster,
        reserved_sectors=reserved_sectors,
        fat_count=fat_count,
        root_entries=root_entries,
        total_sectors=total_sectors,
        media_descriptor=media_descriptor,
        sectors_per_fat=sectors_per_fat,
        sectors_per_track=sectors_per_track,
        heads=heads,
        fat_offset=fat_offset,
        root_offset=root_offset,
        data_offset=data_offset,
        root_dir_sectors=root_dir_sectors,
    )


def fat12_entry(image: bytes, geometry: FatGeometry, cluster: int) -> int:
    byte_index = geometry.fat_offset + cluster + cluster // 2
    pair = image[byte_index] | (image[byte_index + 1] << 8)
    if cluster & 1:
        return pair >> 4
    return pair & 0x0FFF


def decode_83_name(entry: bytes) -> str:
    stem = entry[0:8].decode("ascii", errors="replace").rstrip()
    ext = entry[8:11].decode("ascii", errors="replace").rstrip()
    if ext:
        return f"{stem}.{ext}"
    return stem


def iter_root_files(image: bytes, geometry: FatGeometry) -> Iterable[FatFile]:
    for index in range(geometry.root_entries):
        offset = geometry.root_offset + index * 32
        entry = image[offset : offset + 32]
        if len(entry) < 32:
            break
        first = entry[0]
        if first == 0x00:
            break
        if first == 0xE5:
            continue

        attributes = entry[0x0B]
        if attributes == 0x0F or attributes & 0x18:
            continue

        size = le32(entry, 0x1C)
        first_cluster = le16(entry, 0x1A)
        if size <= 0:
            continue

        yield FatFile(
            name=decode_83_name(entry),
            directory_offset=offset,
            first_cluster=first_cluster,
            size=size,
        )


def cluster_offset(geometry: FatGeometry, cluster: int) -> int:
    return geometry.data_offset + (cluster - 2) * geometry.cluster_size


def file_clusters(image: bytes, geometry: FatGeometry, item: FatFile) -> list[int]:
    clusters: list[int] = []
    seen: set[int] = set()
    cluster = item.first_cluster
    remaining = item.size
    while 2 <= cluster < 0xFF8 and remaining > 0:
        if cluster in seen:
            raise ValueError(f"FAT chain loop in {item.name} at cluster {cluster}")
        seen.add(cluster)
        clusters.append(cluster)
        remaining -= geometry.cluster_size
        cluster = fat12_entry(image, geometry, cluster)
    return clusters


def read_file_bytes(image: bytes, geometry: FatGeometry, item: FatFile) -> bytes:
    output = bytearray()
    remaining = item.size
    for cluster in file_clusters(image, geometry, item):
        offset = cluster_offset(geometry, cluster)
        chunk = image[offset : offset + geometry.cluster_size]
        output.extend(chunk[:remaining])
        remaining -= len(chunk)
        if remaining <= 0:
            break
    return bytes(output[: item.size])


def scan_floppy(path: Path) -> list[dict[str, object]]:
    image = path.read_bytes()
    geometry = parse_geometry(image)
    rows: list[dict[str, object]] = []

    for item in iter_root_files(image, geometry):
        payload = read_file_bytes(image, geometry, item)
        if not payload.startswith(objects.OBJECT_MAGIC):
            continue

        first_object_offset = cluster_offset(geometry, item.first_cluster)
        row = {
            "image": str(path),
            "container_kind": "fat12_floppy",
            "fat_file": item.name,
            "directory_offset": item.directory_offset,
            "first_cluster": item.first_cluster,
            "cluster_count": len(file_clusters(image, geometry, item)),
            "file_size": item.size,
            "object_offset": first_object_offset,
            "object_payload_offset_in_file": "",
            "stored_payload_offset": "",
            "bytes_per_sector": geometry.bytes_per_sector,
            "sectors_per_cluster": geometry.sectors_per_cluster,
            "data_offset": geometry.data_offset,
        }
        row.update(objects.summarize_object_header(payload[:0x200]))
        header_size = row.get("header_size")
        if isinstance(header_size, int):
            row["object_payload_offset_in_file"] = header_size
            row["stored_payload_offset"] = first_object_offset + header_size
        rows.append(row)

    return rows


def write_reports(rows: list[dict[str, object]], output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    json_path = output_dir / "floppy_objects.json"
    csv_path = output_dir / "floppy_objects.csv"
    json_path.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")

    if not rows:
        csv_path.write_text("", encoding="utf-8")
        return

    fieldnames = sorted({key for row in rows for key in row})
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)



def _int_row_value(row: dict[str, object], key: str, default: int = 0) -> int:
    value = row.get(key, default)
    return value if isinstance(value, int) else default

def extract_smpl_rows(rows: list[dict[str, object]], output_dir: Path) -> list[dict[str, object]]:
    output_dir.mkdir(parents=True, exist_ok=True)
    by_image: dict[Path, bytes] = {}
    extracted: list[dict[str, object]] = []

    for row in rows:
        if row.get("type") != "SMPL":
            continue

        image_path = Path(str(row["image"]))
        image = by_image.setdefault(image_path, image_path.read_bytes())
        geometry = parse_geometry(image)
        item = FatFile(
            name=str(row["fat_file"]),
            directory_offset=_int_row_value(row, "directory_offset"),
            first_cluster=_int_row_value(row, "first_cluster"),
            size=_int_row_value(row, "file_size"),
        )
        payload = read_file_bytes(image, geometry, item)
        stem_prefix = (
            f"{image_path.stem}_{objects.safe_name(str(row['fat_file']).replace('.', '_'))}"
        )
        def read_stored_payload(rel_offset: int, size: int, payload: bytes = payload) -> bytes:
            return payload[rel_offset : rel_offset + size]

        extracted.append(
            objects.write_current_smpl_wav(
                row=row,
                output_dir=output_dir,
                stem_prefix=stem_prefix,
                read_stored_payload=read_stored_payload,
                source_container=str(image_path),
                container_kind="fat12_floppy",
                extra_metadata={
                    "source_image": str(image_path),
                    "fat_file": row.get("fat_file"),
                    "directory_offset": row.get("directory_offset"),
                    "first_cluster": row.get("first_cluster"),
                    "file_size": row.get("file_size"),
                    "object_offset": row.get("object_offset"),
                },
            )
        )

    return extracted


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("images", nargs="+", type=Path)
    parser.add_argument("--output-dir", "-o", type=Path, required=True)
    parser.add_argument(
        "--extract-wav-dir",
        type=Path,
        help="also extract current-format SMPL payloads through the shared decoder",
    )
    parser.add_argument("--quiet", action="store_true")
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    rows: list[dict[str, object]] = []
    for path in expand_inputs(args.images):
        rows.extend(scan_floppy(path))

    write_reports(rows, args.output_dir)
    extracted: list[dict[str, object]] = []
    if args.extract_wav_dir:
        extracted = extract_smpl_rows(rows, args.extract_wav_dir)

    print(f"reported {len(rows)} floppy object files to {args.output_dir}")
    if args.extract_wav_dir:
        print(f"extracted {len(extracted)} SMPL waveforms to {args.extract_wav_dir}")
    if not args.quiet:
        for row in rows:
            print(
                f"{row['image']} {row['fat_file']} type={row['type']} "
                f"size={row['file_size']} name={row['name_guess']}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
