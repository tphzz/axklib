"""ISO/CD-ROM discovery for embedded Yamaha A-Series objects."""

from __future__ import annotations

import hashlib
import mmap
import re
from collections import Counter
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, cast

from axklib.objects import current as a_series_objects

SECTOR_SIZE = 2048
PVD_SECTOR = 16
ISO_ID = b"CD001"
OBJECT_MAGIC = a_series_objects.OBJECT_MAGIC
SBAC_SLOT_COUNT_OFFSET = 0x144
SBAC_SLOT_START = 0x14C
SBAC_SLOT_SIZE = 0x14
IMPOSSIBLE_INTERNAL_CAPACITY_QUALITIES = {
    "impossible-internal-capacity",
    "raw-scan-impossible-internal-capacity",
}


def _int_value(value: object, default: int = 0) -> int:
    return value if isinstance(value, int) else default


def base_iso_recovery_quality(inventory_method: str, inventory_status: str) -> tuple[str, str]:
    if inventory_method == "iso9660" and inventory_status == "iso9660":
        return "clean-iso9660-object", "object came from an ISO9660 directory entry"
    if inventory_method == "raw-magic-scan":
        return (
            "raw-scan-recovered-object",
            "object came from raw FSFSDEV3SPLX magic scanning after ISO directory parsing was incomplete or unusable",
        )
    return (
        "nonstandard-iso-object",
        "object came from an ISO reader path outside the clean ISO9660 directory case",
    )


def classify_iso_recovery(
    row: IsoFileRow, payload: bytes, inventory_status: str
) -> tuple[str, str]:
    quality, notes = base_iso_recovery_quality(row.inventory_method, inventory_status)
    if row.object_type == "SBAC" and len(payload) > SBAC_SLOT_COUNT_OFFSET:
        slot_count = payload[SBAC_SLOT_COUNT_OFFSET]
        max_slots = max(0, (len(payload) - SBAC_SLOT_START) // SBAC_SLOT_SIZE)
        if slot_count > max_slots:
            quality = (
                "raw-scan-impossible-internal-capacity"
                if row.inventory_method == "raw-magic-scan"
                else "impossible-internal-capacity"
            )
            notes = (
                f"SBAC+0x144 slot count {slot_count} exceeds recovered payload capacity {max_slots}; "
                "treat this object as a recovery artifact or nonstandard/truncated embedded span until proven otherwise"
            )
    return quality, notes


def annotate_rows_with_inventory_status(rows: list[IsoFileRow], inventory_status: str) -> None:
    for row in rows:
        row.inventory_status = inventory_status
        row.iso_recovery_quality, row.iso_recovery_notes = base_iso_recovery_quality(
            row.inventory_method,
            inventory_status,
        )


def is_authoritative_source_quality(quality: str) -> bool:
    return quality not in IMPOSSIBLE_INTERNAL_CAPACITY_QUALITIES


@dataclass
class IsoImageSummary:
    image: str
    volume_id: str
    directory_count: int
    file_count: int
    fsfsdev3splx_object_count: int
    object_type_counts: str
    smpl_object_count: int
    smpl_payload_bytes: int
    inventory_status: str


@dataclass
class IsoFileRow:
    image: str
    volume_id: str
    path: str
    extent_sector: int
    data_offset: int
    size: int
    is_directory: bool
    known_type: bool
    object_type: str
    name_guess: str
    inventory_method: str
    inventory_status: str = ""
    iso_recovery_quality: str = ""
    iso_recovery_notes: str = ""


@dataclass
class IsoObjectHashRow:
    image: str
    volume_id: str
    path: str
    extent_sector: int
    data_offset: int
    size: int
    object_type: str
    name_guess: str
    inventory_method: str
    object_sha256: str
    header_size: int | str
    stored_payload_size: int | str
    stored_payload_sha256: str
    high_byte_lane_sha256: str
    high_byte_lane_size: int | str
    low_byte_lane_sha256: str
    low_byte_lane_size: int | str
    decoded_pcm_sha256: str
    decoded_pcm_size: int | str
    stored_payload_transform: str
    marker_lane_payload_detected: bool | str
    inventory_status: str = ""
    iso_recovery_quality: str = ""
    iso_recovery_notes: str = ""


@dataclass
class IsoStringRow:
    image: str
    volume_id: str
    path: str
    object_type: str
    name_guess: str
    inventory_method: str
    strings: str
    inventory_status: str = ""
    iso_recovery_quality: str = ""
    iso_recovery_notes: str = ""


@dataclass(frozen=True)
class IsoMenuLabels:
    group_labels: dict[str, str]
    volume_labels: dict[tuple[str, str], str]


class Iso9660Error(ValueError):
    pass


def le32(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], "little")


def clean_text(data: bytes) -> str:
    return data.decode("ascii", errors="replace").rstrip(" \x00")


def strip_version(name: str) -> str:
    base = name.split(";", 1)[0] if ";" in name else name
    return base.rstrip(".")


def read_at(handle: BinaryIO, offset: int, size: int) -> bytes:
    handle.seek(offset)
    data = handle.read(size)
    if len(data) != size:
        raise Iso9660Error(f"short read at {offset}: wanted {size}, got {len(data)}")
    return data


def parse_directory_record(record: bytes) -> dict[str, object] | None:
    if not record or record[0] == 0:
        return None
    length = record[0]
    if length < 34 or len(record) < length:
        return None
    name_len = record[32]
    name_bytes = record[33 : 33 + name_len]
    if name_bytes == b"\x00":
        name = "."
    elif name_bytes == b"\x01":
        name = ".."
    else:
        name = strip_version(clean_text(name_bytes))
    return {
        "length": length,
        "extent_sector": le32(record, 2),
        "size": le32(record, 10),
        "flags": record[25],
        "name": name,
    }


def iter_directory_records(data: bytes) -> Iterable[dict[str, object]]:
    offset = 0
    while offset < len(data):
        length = data[offset]
        if length == 0:
            offset = ((offset // SECTOR_SIZE) + 1) * SECTOR_SIZE
            continue
        record = parse_directory_record(data[offset : offset + length])
        if record is not None:
            yield record
        offset += length


def pvd_info(handle: BinaryIO) -> tuple[str, dict[str, object]]:
    pvd = read_at(handle, PVD_SECTOR * SECTOR_SIZE, SECTOR_SIZE)
    if pvd[0] != 1 or pvd[1:6] != ISO_ID:
        raise Iso9660Error("primary volume descriptor not found at sector 16")
    volume_id = clean_text(pvd[40:72])
    root = parse_directory_record(pvd[156 : 156 + pvd[156]])
    if root is None:
        raise Iso9660Error("root directory record is missing or malformed")
    return volume_id, root


def read_extent(handle: BinaryIO, sector: int, size: int) -> bytes:
    return read_at(handle, sector * SECTOR_SIZE, size)


def walk_iso_files(path: Path) -> tuple[str, list[IsoFileRow]]:
    rows: list[IsoFileRow] = []
    with path.open("rb") as handle:
        volume_id, root = pvd_info(handle)
        queue: list[tuple[str, int, int]] = [
            ("", _int_value(root.get("extent_sector")), _int_value(root.get("size")))
        ]
        while queue:
            parent, sector, size = queue.pop(0)
            directory_data = read_extent(handle, sector, size)
            for record in iter_directory_records(directory_data):
                name = str(record["name"])
                if name in {".", ".."}:
                    continue
                child_path = f"{parent}/{name}" if parent else name
                child_sector = _int_value(record.get("extent_sector"))
                child_size = _int_value(record.get("size"))
                is_dir = bool(_int_value(record.get("flags")) & 0x02)
                row = IsoFileRow(
                    image=str(path),
                    volume_id=volume_id,
                    path=child_path,
                    extent_sector=child_sector,
                    data_offset=child_sector * SECTOR_SIZE,
                    size=child_size,
                    is_directory=is_dir,
                    known_type=False,
                    object_type="",
                    name_guess="",
                    inventory_method="iso9660",
                )
                if is_dir:
                    rows.append(row)
                    queue.append((child_path, child_sector, child_size))
                    continue
                header = (
                    read_extent(handle, child_sector, min(child_size, 0x200)) if child_size else b""
                )
                if header.startswith(OBJECT_MAGIC) and len(header) >= 0x42:
                    summary = a_series_objects.summarize_object_header(header)
                    row.known_type = bool(summary.get("known_type"))
                    row.object_type = str(summary.get("type", ""))
                    row.name_guess = str(summary.get("name_guess", ""))
                rows.append(row)
    annotate_rows_with_inventory_status(rows, "iso9660")
    return volume_id, rows


def _iso_path_parts(path: str) -> list[str]:
    return [part for part in path.replace("\\", "/").split("/") if part]


def _read_text_file(handle: BinaryIO, row: IsoFileRow) -> str:
    return clean_text(read_at(handle, row.data_offset, row.size)).strip()


def _decode_menu_volume_record(record: bytes) -> tuple[str, str] | None:
    if len(record) < 22:
        return None
    match = re.search(rb"F\d{3}", record[:24])
    if match is None:
        return None
    volume = match.group(0).decode("ascii")
    label = clean_text(record[1:14]).strip()
    if not label:
        return None
    return volume, label


def decode_yamaha_menu_labels(path: Path, rows: Iterable[IsoFileRow]) -> IsoMenuLabels:
    """Decode sampler-facing Yamaha CD-ROM menu labels from ISO files."""

    row_by_path = {row.path: row for row in rows}
    volume_dirs = {
        (parts[0], parts[1])
        for row in row_by_path.values()
        if row.is_directory
        for parts in [_iso_path_parts(row.path)]
        if len(parts) == 2 and re.fullmatch(r"F\d{3}", parts[1])
    }
    groups = sorted({group for group, _volume in volume_dirs})
    group_labels: dict[str, str] = {}
    volume_labels: dict[tuple[str, str], str] = {}

    with path.open("rb") as handle:
        for group in groups:
            label_rows = [
                row
                for row in row_by_path.values()
                for parts in [_iso_path_parts(row.path)]
                if (
                    len(parts) == 2
                    and parts[0] == group
                    and not row.is_directory
                    and re.fullmatch(r"F\d{3}", parts[1])
                    and row.size <= 64
                )
            ]
            for row in sorted(label_rows, key=lambda item: item.path):
                label = _read_text_file(handle, row)
                if label:
                    group_labels[group] = label
                    break

            table_row = row_by_path.get(f"{group}/0000")
            if table_row is None or table_row.is_directory:
                continue
            table = read_at(handle, table_row.data_offset, table_row.size)
            for offset in range(0, len(table) - (len(table) % 32), 32):
                decoded = _decode_menu_volume_record(table[offset : offset + 32])
                if decoded is None:
                    continue
                volume, label = decoded
                if (group, volume) in volume_dirs:
                    volume_labels[(group, volume)] = label

    return IsoMenuLabels(group_labels=group_labels, volume_labels=volume_labels)


def object_span_from_header(data: bytes | mmap.mmap, offset: int, next_offset: int | None) -> int:
    header = data[offset : offset + 0x200]
    if len(header) >= 0x24:
        try:
            summary = a_series_objects.summarize_object_header(header)
            header_size = _int_value(summary.get("header_size"))
            payload_size = _int_value(summary.get("payload_bytes_0x1c"))
            candidate = header_size + payload_size
            if candidate >= 0x10 and offset + candidate <= len(data):
                return candidate
        except (ValueError, TypeError):
            pass
    if next_offset is not None and next_offset > offset:
        return next_offset - offset
    return min(len(data) - offset, 0x200)


def raw_magic_scan(path: Path) -> tuple[str, list[IsoFileRow]]:
    rows: list[IsoFileRow] = []
    with path.open("rb") as handle:
        with mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as data:
            offsets: list[int] = []
            offset = data.find(OBJECT_MAGIC)
            while offset != -1:
                offsets.append(offset)
                offset = data.find(OBJECT_MAGIC, offset + 1)
            for index, offset in enumerate(offsets):
                next_offset = offsets[index + 1] if index + 1 < len(offsets) else None
                header = bytes(data[offset : offset + 0x200])
                if len(header) < 0x42:
                    continue
                try:
                    summary = a_series_objects.summarize_object_header(header)
                except ValueError:
                    continue
                size = object_span_from_header(data, offset, next_offset)
                rows.append(
                    IsoFileRow(
                        image=str(path),
                        volume_id="raw-magic-scan",
                        path=f"raw/0x{offset:08x}",
                        extent_sector=offset // SECTOR_SIZE,
                        data_offset=offset,
                        size=size,
                        is_directory=False,
                        known_type=bool(summary.get("known_type")),
                        object_type=str(summary.get("type", "")),
                        name_guess=str(summary.get("name_guess", "")),
                        inventory_method="raw-magic-scan",
                    )
                )
    return "raw-magic-scan", rows


def scan_file_rows(image: Path) -> tuple[str, list[IsoFileRow], str]:
    status = "iso9660"
    try:
        volume_id, rows = walk_iso_files(image)
        if not any(row.object_type for row in rows):
            raw_volume_id, raw_rows = raw_magic_scan(image)
            if raw_rows:
                volume_id = raw_volume_id
                rows = raw_rows
                status = "raw-magic-scan-after-empty-iso"
    except Iso9660Error as exc:
        volume_id, rows = raw_magic_scan(image)
        status = f"raw-magic-scan-after-iso-error:{exc}"
    annotate_rows_with_inventory_status(rows, status)
    return volume_id, rows, status


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def strings_from_bytes(data: bytes, *, min_len: int = 4) -> list[str]:
    out: list[str] = []
    current = bytearray()
    for byte in data:
        if 0x20 <= byte <= 0x7E:
            current.append(byte)
        else:
            if len(current) >= min_len:
                out.append(current.decode("ascii", errors="replace"))
            current.clear()
    if len(current) >= min_len:
        out.append(current.decode("ascii", errors="replace"))
    return out


def build_hash_rows(
    path: Path, volume_id: str, file_rows: list[IsoFileRow]
) -> tuple[list[IsoObjectHashRow], list[IsoStringRow]]:
    object_rows = [row for row in file_rows if row.object_type]
    hashes: list[IsoObjectHashRow] = []
    strings: list[IsoStringRow] = []
    with path.open("rb") as handle:
        for row in object_rows:
            payload = read_at(handle, row.data_offset, row.size)
            quality, quality_notes = classify_iso_recovery(row, payload, row.inventory_status)
            row.iso_recovery_quality = quality
            row.iso_recovery_notes = quality_notes
            header = payload[:0x200]
            string_sample = payload[: min(len(payload), 8192)]
            strings.append(
                IsoStringRow(
                    image=str(path),
                    volume_id=volume_id,
                    path=row.path,
                    object_type=row.object_type,
                    name_guess=row.name_guess,
                    inventory_method=row.inventory_method,
                    strings=" | ".join(strings_from_bytes(string_sample)[:80]),
                    inventory_status=row.inventory_status,
                    iso_recovery_quality=row.iso_recovery_quality,
                    iso_recovery_notes=row.iso_recovery_notes,
                )
            )
            header_size: int | str = ""
            stored_payload_size: int | str = ""
            stored_payload_sha = ""
            high_sha = ""
            high_size: int | str = ""
            low_sha = ""
            low_size: int | str = ""
            decoded_sha = ""
            decoded_size: int | str = ""
            transform = ""
            marker_detected: bool | str = ""
            if row.object_type == "SMPL" and len(header) >= 0x2C:
                summary = a_series_objects.summarize_object_header(header)
                header_size_int = _int_value(summary.get("header_size"))
                stored_payload_size_int = _int_value(summary.get("payload_bytes_0x1c"))
                width = _int_value(summary.get("bytes_per_sample_guess"))
                header_size = header_size_int
                stored_payload_size = stored_payload_size_int
                if (
                    header_size_int >= 0
                    and stored_payload_size_int >= 0
                    and header_size_int + stored_payload_size_int <= len(payload)
                ):
                    stored = payload[header_size_int : header_size_int + stored_payload_size_int]
                    stored_payload_sha = sha256(stored)
                    high_lane = stored[0::2]
                    low_lane = stored[1::2]
                    high_sha = sha256(high_lane)
                    high_size = len(high_lane)
                    low_sha = sha256(low_lane)
                    low_size = len(low_lane)
                    try:
                        decoded = a_series_objects.decode_current_smpl_payload_info(stored, width)
                        decoded_pcm = cast(bytes, decoded["pcm"])
                        decoded_sha = sha256(decoded_pcm)
                        decoded_size = len(decoded_pcm)
                        transform = str(decoded["stored_payload_transform"])
                        marker_detected = bool(decoded["marker_lane_payload_detected"])
                    except ValueError:
                        transform = "unsupported-width"
            hashes.append(
                IsoObjectHashRow(
                    image=str(path),
                    volume_id=volume_id,
                    path=row.path,
                    extent_sector=row.extent_sector,
                    data_offset=row.data_offset,
                    size=row.size,
                    object_type=row.object_type,
                    name_guess=row.name_guess,
                    inventory_method=row.inventory_method,
                    object_sha256=sha256(payload),
                    header_size=header_size,
                    stored_payload_size=stored_payload_size,
                    stored_payload_sha256=stored_payload_sha,
                    high_byte_lane_sha256=high_sha,
                    high_byte_lane_size=high_size,
                    low_byte_lane_sha256=low_sha,
                    low_byte_lane_size=low_size,
                    decoded_pcm_sha256=decoded_sha,
                    decoded_pcm_size=decoded_size,
                    stored_payload_transform=transform,
                    marker_lane_payload_detected=marker_detected,
                    inventory_status=row.inventory_status,
                    iso_recovery_quality=row.iso_recovery_quality,
                    iso_recovery_notes=row.iso_recovery_notes,
                )
            )
    return hashes, strings


def expand_inputs(paths: Iterable[Path]) -> list[Path]:
    out: list[Path] = []
    for path in paths:
        if path.is_dir():
            out.extend(
                sorted(p for p in path.rglob("*") if p.is_file() and p.suffix.lower() == ".iso")
            )
        else:
            out.append(path)
    return sorted(dict.fromkeys(out), key=lambda item: str(item).lower())


def scan_one_image(
    image: Path,
) -> tuple[IsoImageSummary, list[IsoFileRow], list[IsoObjectHashRow], list[IsoStringRow]]:
    volume_id, rows, status = scan_file_rows(image)
    hashes, strings = build_hash_rows(image, volume_id, rows)
    object_counts = Counter(row.object_type for row in rows if row.object_type)
    summary = IsoImageSummary(
        image=str(image),
        volume_id=volume_id,
        directory_count=sum(1 for row in rows if row.is_directory),
        file_count=sum(1 for row in rows if not row.is_directory),
        fsfsdev3splx_object_count=sum(object_counts.values()),
        object_type_counts=";".join(
            f"{key}:{value}" for key, value in sorted(object_counts.items())
        ),
        smpl_object_count=object_counts.get("SMPL", 0),
        smpl_payload_bytes=sum(
            int(row.stored_payload_size)
            for row in hashes
            if row.object_type == "SMPL" and isinstance(row.stored_payload_size, int)
        ),
        inventory_status=status,
    )
    return summary, rows, hashes, strings


def scan_images(
    images: list[Path],
) -> tuple[list[IsoImageSummary], list[IsoFileRow], list[IsoObjectHashRow], list[IsoStringRow]]:
    summaries: list[IsoImageSummary] = []
    all_files: list[IsoFileRow] = []
    all_hashes: list[IsoObjectHashRow] = []
    all_strings: list[IsoStringRow] = []
    for image in images:
        summary, rows, hashes, strings = scan_one_image(image)
        summaries.append(summary)
        all_files.extend(rows)
        all_hashes.extend(hashes)
        all_strings.extend(strings)
        print(
            f"{image}: {summary.smpl_object_count} SMPL, "
            f"{summary.fsfsdev3splx_object_count} FSFSDEV3SPLX objects ({summary.inventory_status})"
        )
    return summaries, all_files, all_hashes, all_strings
