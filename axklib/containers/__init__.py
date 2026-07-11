"""Unified object loading for axklib container formats."""

from __future__ import annotations

import glob
from collections import Counter
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from functools import partial
from pathlib import Path

from axklib.containers import fat as fat_container
from axklib.containers import iso_lowlevel, sfs_extents, sfs_scan
from axklib.containers import sfs_dump as dumper
from axklib.containers import sfs_inventory as inventory
from axklib.model import (
    AxklibContainerKind,
    AxklibContainerRef,
    AxklibObject,
    AxklibObjectFormat,
    AxklibObjectHeader,
    AxklibObjectRef,
    AxklibQuality,
    DataQuality,
)
from axklib.objects import current as objects

SUPPORTED_NORMAL_OBJECT_TYPES = {"SMPL", "SBNK", "SBAC", "PROG", "SEQU", "PRF3"}
STANDALONE_OBJECT_KIND = "standalone_object"
ISO_SECTOR_SIZE = 2048
ISO_PVD_SECTOR = 16
ISO_ID = b"CD001"


@dataclass(frozen=True)
class IsoObjectEntry:
    volume_id: str
    logical_path: str
    payload: bytes
    payload_loader: Callable[[], bytes] | None
    extent_sector: int
    data_offset: int
    file_size: int
    inventory_status: str
    recovery_quality: str
    recovery_notes: str
    raw_group: str = ""
    raw_volume: str = ""
    group_label: str = ""
    volume_label: str = ""


@dataclass(frozen=True)
class FatObjectEntry:
    fat_file: str
    payload: bytes
    payload_loader: Callable[[], bytes] | None
    directory_offset: int
    first_cluster: int
    cluster_count: int
    file_size: int
    object_offset: int
    stored_payload_offset: int | None


@dataclass(frozen=True)
class OpenOptions:
    """Options that control container loading.

    Use these when opening images to choose partition limits, whether payload bytes should be loaded, and whether unsupported or malformed inputs should fail strictly."""

    max_partitions: int | None = 8
    include_payloads: bool = True
    strict: bool = False
    lazy_payloads: bool = False


@dataclass(frozen=True)
class AxklibContainer:
    """Loaded Yamaha-capable container with decoded object payloads.

    Use this as the primary result of opening one SFS/HDA/HDS image, ISO9660 disc image, FAT floppy image, standalone object, or directory expansion."""

    source_path: Path
    kind: str
    detected_format: str
    objects: tuple[AxklibObject, ...]
    recovery_quality_summary: dict[str, int]
    issues: tuple[str, ...] = ()


@dataclass(frozen=True)
class AxklibContainerLoadError:
    """Structured error for an input path that could not be opened as a supported container.

    Use this in multi-input workflows so one bad file can be reported without discarding successfully loaded containers."""

    path: Path
    error_code: str
    message: str
    recoverable: bool
    original_exception: str = ""


@dataclass(frozen=True)
class AxklibContainerLoadResult:
    """Per-path load result containing either a container or a structured load error.

    Use this for corpus scans and CLI commands that need deterministic failure reporting across many inputs."""

    path: Path
    container: AxklibContainer | None = None
    error: AxklibContainerLoadError | None = None


class AxklibContainerUnsupportedError(ValueError):
    """Exception raised when a path is readable but not a supported axklib container.

    The `code` attribute is intended for stable diagnostics rather than user-facing traceback text."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


def expand_inputs(inputs: Iterable[Path]) -> list[Path]:
    paths: list[Path] = []
    for item in inputs:
        text = str(item)
        if any(char in text for char in "*?["):
            paths.extend(Path(match) for match in glob.glob(text))
        else:
            paths.append(item)
    return sorted(dict.fromkeys(paths), key=lambda item: str(item).lower())


def detect_container_kind(path: Path) -> str:
    if path.is_dir():
        if any(child.is_file() and child.suffix.lower() == ".iso" for child in path.rglob("*")):
            return AxklibContainerKind.ISO.value
        return AxklibContainerKind.UNKNOWN.value
    if path.suffix.lower() == ".iso":
        return AxklibContainerKind.ISO.value
    with path.open("rb") as handle:
        first = handle.read(0x9000)
    if first.startswith(dumper.MAGIC):
        return AxklibContainerKind.SFS.value
    if len(first) >= ISO_PVD_SECTOR * ISO_SECTOR_SIZE + 6:
        pvd_offset = ISO_PVD_SECTOR * ISO_SECTOR_SIZE
        if first[pvd_offset] == 1 and first[pvd_offset + 1 : pvd_offset + 6] == ISO_ID:
            return AxklibContainerKind.ISO.value
    if first.startswith(objects.OBJECT_MAGIC):
        return STANDALONE_OBJECT_KIND
    if path.suffix.lower() in {".ima", ".img"}:
        return AxklibContainerKind.FAT12_FLOPPY.value
    return AxklibContainerKind.UNKNOWN.value


def _int_value(value: object, default: int = 0) -> int:
    return value if isinstance(value, int) else default


def _parsed_partitions(parsed: dict[str, object]) -> list[dict[str, object]]:
    partitions = parsed.get("partitions", [])
    return (
        [partition for partition in partitions if isinstance(partition, dict)]
        if isinstance(partitions, list)
        else []
    )


def object_header(payload: bytes) -> AxklibObjectHeader | None:
    if len(payload) < 0x24:
        return None
    try:
        summary = objects.summarize_object_header(payload[:0x200])
    except ValueError:
        return None
    return AxklibObjectHeader(
        header_size=_int_value(summary.get("header_size")),
        stored_payload_size=_int_value(summary.get("payload_bytes_0x1c")),
        raw_prefix_hex=payload[:64].hex(),
    )


def make_object(
    *,
    image: Path,
    container_kind: AxklibContainerKind,
    scope_key: str,
    object_key: str,
    partition_index: int | None,
    sfs_id: int | None,
    fat_file: str,
    payload_offset: int | None,
    payload_size: int,
    object_type: str,
    name: str,
    payload: bytes | None = None,
    payload_loader: Callable[[], bytes] | None = None,
    header_payload: bytes | None = None,
    basis: str,
    metadata: dict[str, object] | None = None,
) -> AxklibObject:
    return AxklibObject(
        ref=AxklibObjectRef(
            object_key=object_key,
            partition_index=partition_index,
            sfs_id=sfs_id,
            fat_file=fat_file,
            payload_offset=payload_offset,
            payload_size=payload_size,
        ),
        container=AxklibContainerRef(
            source_image=str(image),
            kind=container_kind,
            scope_key=scope_key,
        ),
        volume=None,
        object_type=object_type,
        object_format=AxklibObjectFormat.NORMAL,
        name=name,
        payload=payload,
        payload_loader=payload_loader,
        header=object_header(header_payload if header_payload is not None else payload or b""),
        quality=AxklibQuality(
            quality=DataQuality.KNOWN,
            source=basis,
        ),
        metadata=metadata,
    )


def _iso_path_group_volume(logical_path: str) -> tuple[str, str]:
    parts = [part for part in logical_path.replace("\\", "/").split("/") if part]
    group = parts[0] if len(parts) >= 1 else ""
    volume = parts[1] if len(parts) >= 2 else ""
    return group, volume


def _expand_iso_inputs(paths: Iterable[Path]) -> list[Path]:
    out: list[Path] = []
    for path in paths:
        if path.is_dir():
            out.extend(
                sorted(
                    child
                    for child in path.rglob("*")
                    if child.is_file() and child.suffix.lower() == ".iso"
                )
            )
        else:
            out.append(path)
    return sorted(dict.fromkeys(out), key=lambda item: str(item).lower())


def _read_iso_payload(path: Path, offset: int, size: int) -> bytes:
    with path.open("rb") as handle:
        return iso_lowlevel.read_at(handle, offset, size)


def _iter_iso_objects(path: Path, *, lazy_payloads: bool = False) -> Iterable[IsoObjectEntry]:
    try:
        volume_id, rows = iso_lowlevel.walk_iso_files(path)
    except iso_lowlevel.Iso9660Error as exc:
        raise AxklibContainerUnsupportedError(
            "CONTAINER_UNSUPPORTED_ISO9660",
            f"unsupported ISO9660 image {path}: {exc}",
        ) from exc
    menu_labels = iso_lowlevel.decode_yamaha_menu_labels(path, rows)
    with path.open("rb") as handle:
        for row in rows:
            if row.is_directory or row.object_type not in SUPPORTED_NORMAL_OBJECT_TYPES:
                continue
            prefix_size = min(row.size, max(0x200, iso_lowlevel.SBAC_SLOT_COUNT_OFFSET + 1))
            prefix = iso_lowlevel.read_at(handle, row.data_offset, prefix_size)
            payload = (
                prefix if lazy_payloads else iso_lowlevel.read_at(handle, row.data_offset, row.size)
            )
            quality, notes = iso_lowlevel.classify_iso_recovery(row, payload, row.inventory_status)
            raw_group, raw_volume = _iso_path_group_volume(row.path)
            yield IsoObjectEntry(
                volume_id=volume_id,
                logical_path=row.path,
                payload=payload,
                payload_loader=partial(_read_iso_payload, path, row.data_offset, row.size)
                if lazy_payloads
                else None,
                extent_sector=row.extent_sector,
                data_offset=row.data_offset,
                file_size=row.size,
                inventory_status=row.inventory_status,
                recovery_quality=quality,
                recovery_notes=notes,
                raw_group=raw_group,
                raw_volume=raw_volume,
                group_label=menu_labels.group_labels.get(raw_group, ""),
                volume_label=menu_labels.volume_labels.get((raw_group, raw_volume), ""),
            )


def _read_fat_file_prefix(
    image: bytes, geometry: fat_container.FatGeometry, item: fat_container.FatFile, size: int
) -> bytes:
    output = bytearray()
    remaining = min(size, item.size)
    for cluster in fat_container.file_clusters(image, geometry, item):
        offset = fat_container.cluster_offset(geometry, cluster)
        chunk = image[offset : offset + geometry.cluster_size]
        take = min(remaining, len(chunk))
        output.extend(chunk[:take])
        remaining -= take
        if remaining <= 0:
            break
    return bytes(output)


def _read_fat_payload(path: Path, directory_offset: int) -> bytes:
    image = path.read_bytes()
    geometry = fat_container.parse_geometry(image)
    for item in fat_container.iter_root_files(image, geometry):
        if item.directory_offset == directory_offset:
            return fat_container.read_file_bytes(image, geometry, item)
    raise FileNotFoundError(
        f"FAT object at directory offset {directory_offset} no longer exists in {path}"
    )


def _iter_fat_objects(path: Path, *, lazy_payloads: bool = False) -> Iterable[FatObjectEntry]:
    image = path.read_bytes()
    try:
        geometry = fat_container.parse_geometry(image)
    except ValueError as exc:
        raise AxklibContainerUnsupportedError(
            "CONTAINER_UNSUPPORTED_FAT",
            f"unsupported FAT/floppy image {path}: {exc}",
        ) from exc
    for item in fat_container.iter_root_files(image, geometry):
        prefix = _read_fat_file_prefix(image, geometry, item, 0x200)
        if not prefix.startswith(objects.OBJECT_MAGIC):
            continue
        payload = prefix if lazy_payloads else fat_container.read_file_bytes(image, geometry, item)
        object_offset = fat_container.cluster_offset(geometry, item.first_cluster)
        header = object_header(payload)
        stored_payload_offset = (
            object_offset + header.header_size
            if header is not None and header.header_size is not None
            else None
        )
        yield FatObjectEntry(
            fat_file=item.name,
            payload=payload,
            payload_loader=partial(_read_fat_payload, path, item.directory_offset)
            if lazy_payloads
            else None,
            directory_offset=item.directory_offset,
            first_cluster=item.first_cluster,
            cluster_count=len(fat_container.file_clusters(image, geometry, item)),
            file_size=item.size,
            object_offset=object_offset,
            stored_payload_offset=stored_payload_offset,
        )


def _fat_metadata(entry: FatObjectEntry) -> dict[str, object]:
    return {
        "fat_directory_offset": entry.directory_offset,
        "fat_first_cluster": entry.first_cluster,
        "fat_cluster_count": entry.cluster_count,
        "fat_file_size": entry.file_size,
        "fat_object_offset": entry.object_offset,
        "fat_stored_payload_offset": entry.stored_payload_offset,
    }


def load_fat_objects(path: Path, *, lazy_payloads: bool = False) -> list[AxklibObject]:
    items: list[AxklibObject] = []
    for entry in _iter_fat_objects(path, lazy_payloads=lazy_payloads):
        payload = entry.payload
        object_type = payload[0x0C:0x10].decode("ascii", errors="replace")
        if object_type not in SUPPORTED_NORMAL_OBJECT_TYPES:
            continue
        items.append(
            make_object(
                image=path,
                container_kind=AxklibContainerKind.FAT12_FLOPPY,
                scope_key=f"{path}:fat-root",
                object_key=f"{path.name}:{entry.fat_file}",
                partition_index=None,
                sfs_id=None,
                fat_file=entry.fat_file,
                payload_offset=entry.object_offset,
                payload_size=entry.file_size,
                object_type=object_type,
                name=objects.clean_ascii(payload[0x32:0x42]),
                payload=None if lazy_payloads else payload,
                payload_loader=entry.payload_loader,
                header_payload=payload,
                basis="FAT/floppy root file containing plain FSFSDEV3SPLX object",
                metadata=_fat_metadata(entry),
            )
        )
    return items


def _read_sfs_payload(
    path: Path,
    record_offset: int,
    *,
    partition_start_sector: int,
    sector_size: int,
    sectors_per_cluster: int,
    cluster_count_limit: int,
) -> bytes:
    with dumper.ImageReader(path) as reader:
        index_record = reader.read_at(record_offset, inventory.INDEX_RECORD_SIZE)
        return sfs_extents.read_index_record_data(
            reader,
            index_record,
            partition_start_sector=partition_start_sector,
            sector_size=sector_size,
            sectors_per_cluster=sectors_per_cluster,
            cluster_count_limit=cluster_count_limit,
        ).data


def load_sfs_objects(path: Path, *, lazy_payloads: bool = False) -> list[AxklibObject]:
    parsed = dumper.parse_image(path, dumper.ReadOptions(max_nodes=4, include_node_payloads=False))
    sector_size = _int_value(parsed.get("sector_size_bytes"), dumper.DEFAULT_SECTOR_SIZE)
    object_rows = sfs_scan.scan_image(path, max_nodes=4)
    items: list[AxklibObject] = []
    with dumper.ImageReader(path) as reader:
        for partition in _parsed_partitions(parsed):
            partition_index = _int_value(partition.get("index"))
            start_sector = _int_value(partition.get("start_sector"))
            sectors_per_cluster = inventory.field_value(partition, "sectors_per_cluster") or 2
            cluster_count_limit = inventory.field_value(partition, "number_of_clusters") or 0
            ynodes = inventory.scan_ynode_records(
                path,
                partition,
                object_rows,
                sector_size=sector_size,
            )
            for record in ynodes:
                if (
                    record.payload_kind != "object"
                    or record.object_type not in SUPPORTED_NORMAL_OBJECT_TYPES
                ):
                    continue
                payload = b""
                payload_loader = None
                if lazy_payloads:
                    payload_loader = partial(
                        _read_sfs_payload,
                        path,
                        record.record_offset,
                        partition_start_sector=start_sector,
                        sector_size=sector_size,
                        sectors_per_cluster=sectors_per_cluster,
                        cluster_count_limit=cluster_count_limit,
                    )
                else:
                    index_record = reader.read_at(record.record_offset, inventory.INDEX_RECORD_SIZE)
                    extent_read = sfs_extents.read_index_record_data(
                        reader,
                        index_record,
                        partition_start_sector=start_sector,
                        sector_size=sector_size,
                        sectors_per_cluster=sectors_per_cluster,
                        cluster_count_limit=cluster_count_limit,
                    )
                    payload = extent_read.data
                    if not payload.startswith(objects.OBJECT_MAGIC):
                        continue
                items.append(
                    make_object(
                        image=path,
                        container_kind=AxklibContainerKind.SFS,
                        scope_key=f"{path}:partition:{partition_index}",
                        object_key=f"p{partition_index}:sfs{record.sfs_id}",
                        partition_index=partition_index,
                        sfs_id=record.sfs_id,
                        fat_file="",
                        payload_offset=record.payload_offset,
                        payload_size=record.data_size,
                        object_type=record.object_type,
                        name=record.object_name
                        if lazy_payloads
                        else objects.clean_ascii(payload[0x32:0x42]),
                        payload=None if lazy_payloads else payload,
                        payload_loader=payload_loader,
                        basis="SFS allocated Y-node containing plain FSFSDEV3SPLX object",
                    )
                )
    return items


def make_summary_object(
    *,
    image: Path,
    container_kind: AxklibContainerKind,
    scope_key: str,
    object_key: str,
    partition_index: int | None,
    sfs_id: int | None,
    fat_file: str,
    payload_offset: int | None,
    payload_size: int,
    object_type: str,
    name: str,
    basis: str,
    metadata: dict[str, object] | None = None,
) -> AxklibObject:
    return AxklibObject(
        ref=AxklibObjectRef(
            object_key=object_key,
            partition_index=partition_index,
            sfs_id=sfs_id,
            fat_file=fat_file,
            payload_offset=payload_offset,
            payload_size=payload_size,
        ),
        container=AxklibContainerRef(
            source_image=str(image),
            kind=container_kind,
            scope_key=scope_key,
        ),
        volume=None,
        object_type=object_type,
        object_format=AxklibObjectFormat.NORMAL,
        name=name,
        payload=b"",
        header=None,
        quality=AxklibQuality(quality=DataQuality.LIKELY, source=basis),
        metadata=metadata,
    )


def load_fat_object_summaries(path: Path) -> list[AxklibObject]:
    items: list[AxklibObject] = []
    for entry in _iter_fat_objects(path):
        payload = entry.payload
        object_type = payload[0x0C:0x10].decode("ascii", errors="replace")
        if object_type not in SUPPORTED_NORMAL_OBJECT_TYPES:
            continue
        items.append(
            make_summary_object(
                image=path,
                container_kind=AxklibContainerKind.FAT12_FLOPPY,
                scope_key=f"{path}:fat-root",
                object_key=f"{path.name}:{entry.fat_file}",
                partition_index=None,
                sfs_id=None,
                fat_file=entry.fat_file,
                payload_offset=entry.object_offset,
                payload_size=len(payload),
                object_type=object_type,
                name=objects.clean_ascii(payload[0x32:0x42]),
                basis="FAT/floppy root file metadata",
                metadata=_fat_metadata(entry),
            )
        )
    return items


def load_sfs_object_summaries(path: Path) -> list[AxklibObject]:
    parsed = dumper.parse_image(path, dumper.ReadOptions(max_nodes=4, include_node_payloads=False))
    sector_size = _int_value(parsed.get("sector_size_bytes"), dumper.DEFAULT_SECTOR_SIZE)
    object_rows = sfs_scan.scan_image(path, max_nodes=4)
    items: list[AxklibObject] = []
    for partition in _parsed_partitions(parsed):
        partition_index = _int_value(partition.get("index"))
        ynodes = inventory.scan_ynode_records(
            path,
            partition,
            object_rows,
            sector_size=sector_size,
        )
        for record in ynodes:
            if (
                record.payload_kind != "object"
                or record.object_type not in SUPPORTED_NORMAL_OBJECT_TYPES
            ):
                continue
            items.append(
                make_summary_object(
                    image=path,
                    container_kind=AxklibContainerKind.SFS,
                    scope_key=f"{path}:partition:{partition_index}",
                    object_key=f"p{partition_index}:sfs{record.sfs_id}",
                    partition_index=partition_index,
                    sfs_id=record.sfs_id,
                    fat_file="",
                    payload_offset=record.payload_offset,
                    payload_size=record.data_size,
                    object_type=record.object_type,
                    name=record.object_name,
                    basis="SFS allocated Y-node metadata summary",
                )
            )
    return items


def _iso_metadata(entry: IsoObjectEntry, header: AxklibObjectHeader | None) -> dict[str, object]:
    return {
        "iso_inventory_method": "iso9660",
        "iso_inventory_status": entry.inventory_status,
        "iso_recovery_quality": entry.recovery_quality,
        "iso_recovery_notes": entry.recovery_notes,
        "iso_extent_sector": entry.extent_sector,
        "iso_data_offset": entry.data_offset,
        "iso_file_size": entry.file_size,
        "iso_raw_group": entry.raw_group,
        "iso_raw_volume": entry.raw_volume,
        "iso_group_label": entry.group_label,
        "iso_volume_label": entry.volume_label,
        "iso_group_label_source": "yamaha-cdrom-menu-label" if entry.group_label else "",
        "iso_volume_label_source": "yamaha-cdrom-menu-label" if entry.volume_label else "",
        "iso_header_size": header.header_size if header else None,
        "iso_stored_payload_size": header.stored_payload_size if header else None,
    }


def load_iso_object_summaries(path: Path) -> list[AxklibObject]:
    items: list[AxklibObject] = []
    for image in _expand_iso_inputs([path]):
        for entry in _iter_iso_objects(image):
            payload = entry.payload
            object_type = payload[0x0C:0x10].decode("ascii", errors="replace")
            if object_type not in SUPPORTED_NORMAL_OBJECT_TYPES:
                continue
            header = object_header(payload)
            items.append(
                make_summary_object(
                    image=image,
                    container_kind=AxklibContainerKind.ISO,
                    scope_key=f"{image}:iso:{entry.volume_id}:iso9660",
                    object_key=f"{image.name}:iso9660:{entry.logical_path}",
                    partition_index=None,
                    sfs_id=None,
                    fat_file=entry.logical_path,
                    payload_offset=entry.data_offset,
                    payload_size=len(payload),
                    object_type=object_type,
                    name=objects.clean_ascii(payload[0x32:0x42]),
                    basis="ISO/CD-ROM directory object metadata summary",
                    metadata=_iso_metadata(entry, header),
                )
            )
    return items


def load_object_summaries(path: Path, container: str) -> list[AxklibObject]:
    kind = detect_container_kind(path) if container == "auto" else container
    if kind == AxklibContainerKind.SFS.value:
        return load_sfs_object_summaries(path)
    if kind in {"fat", AxklibContainerKind.FAT12_FLOPPY.value, "floppy"}:
        return load_fat_object_summaries(path)
    if kind in {AxklibContainerKind.ISO.value, "cdrom"}:
        return load_iso_object_summaries(path)
    if kind == STANDALONE_OBJECT_KIND:
        return load_standalone_object(path)
    raise ValueError(f"unsupported or unknown container kind for {path}: {kind}")


def load_iso_objects(path: Path, *, lazy_payloads: bool = False) -> list[AxklibObject]:
    items: list[AxklibObject] = []
    for image in _expand_iso_inputs([path]):
        for entry in _iter_iso_objects(image, lazy_payloads=lazy_payloads):
            payload = entry.payload
            object_type = payload[0x0C:0x10].decode("ascii", errors="replace")
            if object_type not in SUPPORTED_NORMAL_OBJECT_TYPES:
                continue
            header = object_header(payload)
            items.append(
                make_object(
                    image=image,
                    container_kind=AxklibContainerKind.ISO,
                    scope_key=f"{image}:iso:{entry.volume_id}:iso9660",
                    object_key=f"{image.name}:iso9660:{entry.logical_path}",
                    partition_index=None,
                    sfs_id=None,
                    fat_file=entry.logical_path,
                    payload_offset=entry.data_offset,
                    payload_size=entry.file_size,
                    object_type=object_type,
                    name=objects.clean_ascii(payload[0x32:0x42]),
                    payload=None if lazy_payloads else payload,
                    payload_loader=entry.payload_loader,
                    header_payload=payload,
                    basis="ISO/CD-ROM file containing plain FSFSDEV3SPLX object",
                    metadata=_iso_metadata(entry, header),
                )
            )
    return items


def load_standalone_object(path: Path) -> list[AxklibObject]:
    payload = path.read_bytes()
    if not payload.startswith(objects.OBJECT_MAGIC):
        raise ValueError(f"not a standalone FSFSDEV3SPLX object: {path}")
    object_type = payload[0x0C:0x10].decode("ascii", errors="replace")
    if object_type not in SUPPORTED_NORMAL_OBJECT_TYPES:
        raise ValueError(f"unsupported standalone object type for {path}: {object_type}")
    return [
        make_object(
            image=path,
            container_kind=AxklibContainerKind.UNKNOWN,
            scope_key=f"{path}:standalone-object",
            object_key=f"{path.name}:standalone-object",
            partition_index=None,
            sfs_id=None,
            fat_file=path.name,
            payload_offset=0,
            payload_size=len(payload),
            object_type=object_type,
            name=objects.clean_ascii(payload[0x32:0x42]),
            payload=payload,
            basis="standalone file beginning with FSFSDEV3SPLX object magic",
        )
    ]


def load_objects(path: Path, container: str, *, lazy_payloads: bool = False) -> list[AxklibObject]:
    kind = detect_container_kind(path) if container == "auto" else container
    if kind == AxklibContainerKind.SFS.value:
        return load_sfs_objects(path, lazy_payloads=lazy_payloads)
    if kind in {"fat", AxklibContainerKind.FAT12_FLOPPY.value, "floppy"}:
        return load_fat_objects(path, lazy_payloads=lazy_payloads)
    if kind in {AxklibContainerKind.ISO.value, "cdrom"}:
        return load_iso_objects(path, lazy_payloads=lazy_payloads)
    if kind == STANDALONE_OBJECT_KIND:
        return load_standalone_object(path)
    raise ValueError(f"unsupported or unknown container kind for {path}: {kind}")


def _recovery_quality_summary(items: Iterable[AxklibObject]) -> dict[str, int]:
    counts: Counter[str] = Counter()
    for item in items:
        quality = item.metadata.get("iso_recovery_quality")
        if isinstance(quality, str) and quality:
            counts[quality] += 1
    return dict(sorted(counts.items()))


def open(path: str | Path, *, options: OpenOptions | None = None) -> AxklibContainer:
    opts = options or OpenOptions()
    source = Path(path)
    kind = detect_container_kind(source)
    if kind == AxklibContainerKind.UNKNOWN.value and opts.strict:
        raise ValueError(f"unsupported or unknown container kind for {source}")
    items = (
        load_objects(source, kind, lazy_payloads=opts.lazy_payloads)
        if opts.include_payloads
        else load_object_summaries(source, kind)
    )
    return AxklibContainer(
        source_path=source,
        kind=kind,
        detected_format=kind,
        objects=tuple(items),
        recovery_quality_summary=_recovery_quality_summary(items),
    )


def open_many(
    paths: Iterable[str | Path], *, options: OpenOptions | None = None
) -> list[AxklibContainerLoadResult]:
    results: list[AxklibContainerLoadResult] = []
    for source in expand_inputs(Path(item) for item in paths):
        try:
            results.append(
                AxklibContainerLoadResult(path=source, container=open(source, options=options))
            )
        except AxklibContainerUnsupportedError as exc:
            results.append(
                AxklibContainerLoadResult(
                    path=source,
                    error=AxklibContainerLoadError(
                        path=source,
                        error_code=exc.code,
                        message=str(exc),
                        recoverable=not (options is not None and options.strict),
                        original_exception=type(exc).__name__,
                    ),
                )
            )
        except Exception as exc:
            results.append(
                AxklibContainerLoadResult(
                    path=source,
                    error=AxklibContainerLoadError(
                        path=source,
                        error_code="AXKLIB_CONTAINER_OPEN_FAILED",
                        message=str(exc),
                        recoverable=not (options is not None and options.strict),
                        original_exception=type(exc).__name__,
                    ),
                )
            )
    return results
