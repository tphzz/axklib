"""Unified object loading for axklib container formats."""

from __future__ import annotations

import glob
import warnings
from collections import Counter
from collections.abc import Iterable
from dataclasses import dataclass
from io import BytesIO
from pathlib import Path
from typing import Any

from axklib.containers import sfs_dump as dumper
from axklib.containers import sfs_extents, sfs_scan
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
class OpenOptions:
    """Options that control container loading.
    
    Use these when opening images to choose partition limits, whether payload bytes should be loaded, and whether unsupported or malformed inputs should fail strictly."""
    max_partitions: int | None = 8
    include_payloads: bool = True
    strict: bool = False


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
    return [partition for partition in partitions if isinstance(partition, dict)] if isinstance(partitions, list) else []

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
    payload: bytes,
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
        header=object_header(payload),
        quality=AxklibQuality(
            quality=DataQuality.KNOWN,
            source=basis,
        ),
        metadata=metadata,
    )


def _clean_volume_identifier(value: object, fallback: str) -> str:
    if isinstance(value, bytes):
        text = value.decode("ascii", errors="replace").strip(" \x00")
        return text or fallback
    return fallback


def _strip_iso_version(name: str) -> str:
    base = name.split(";", 1)[0] if ";" in name else name
    return base.rstrip(".")


def _expand_iso_inputs(paths: Iterable[Path]) -> list[Path]:
    out: list[Path] = []
    for path in paths:
        if path.is_dir():
            out.extend(
                sorted(child for child in path.rglob("*") if child.is_file() and child.suffix.lower() == ".iso")
            )
        else:
            out.append(path)
    return sorted(dict.fromkeys(out), key=lambda item: str(item).lower())


def _pycdlib_open(path: Path) -> Any:
    try:
        import pycdlib
    except Exception as exc:  # pragma: no cover - dependency is part of runtime env
        raise AxklibContainerUnsupportedError(
            "CONTAINER_LIBRARY_UNAVAILABLE",
            f"pycdlib is required for ISO/CD-ROM images: {exc}",
        ) from exc
    iso = pycdlib.PyCdlib()
    try:
        iso.open(str(path))
    except Exception as exc:
        try:
            iso.close()
        except Exception:
            pass
        raise AxklibContainerUnsupportedError(
            "CONTAINER_UNSUPPORTED_ISO9660",
            f"unsupported ISO9660 image {path}: {exc}",
        ) from exc
    return iso


def _iter_pycdlib_objects(path: Path) -> Iterable[tuple[str, str, bytes]]:
    iso = _pycdlib_open(path)
    try:
        pvd = getattr(iso, "pvd", None)
        volume_id = _clean_volume_identifier(getattr(pvd, "volume_identifier", None), path.stem)
        walk = getattr(iso, "walk", None)
        if walk is None:
            raise AxklibContainerUnsupportedError(
                "CONTAINER_UNSUPPORTED_ISO9660",
                f"pycdlib does not expose directory walking for {path}",
            )
        for dirname, _dirs, files in walk(iso_path="/"):
            base = str(dirname).strip("/")
            for file_name in files:
                raw_name = str(file_name)
                name = _strip_iso_version(raw_name)
                iso_path = f"/{base}/{raw_name}" if base else f"/{raw_name}"
                logical_path = (f"{base}/{name}" if base else name).strip("/")
                with BytesIO() as handle:
                    iso.get_file_from_iso_fp(handle, iso_path=iso_path)
                    payload = handle.getvalue()
                if payload.startswith(objects.OBJECT_MAGIC):
                    yield volume_id, logical_path, payload
    finally:
        try:
            iso.close()
        except Exception:
            pass


def _pyfatfs_class() -> Any:
    try:
        with warnings.catch_warnings():
            warnings.filterwarnings("ignore", category=UserWarning, module=r"fs(\.|$)")
            from pyfatfs.PyFatFS import PyFatFS
    except Exception as exc:  # pragma: no cover - dependency is part of runtime env
        raise AxklibContainerUnsupportedError(
            "CONTAINER_LIBRARY_UNAVAILABLE",
            f"pyfatfs is required for FAT/floppy images: {exc}",
        ) from exc
    return PyFatFS


def _iter_pyfatfs_objects(path: Path) -> Iterable[tuple[str, bytes]]:
    PyFatFS = _pyfatfs_class()
    fs: Any | None = None
    try:
        fs = PyFatFS(str(path), read_only=True)
        names = sorted(str(name) for name in fs.listdir("/") if name not in {".", ".."})
        for name in names:
            file_path = f"/{name}"
            if fs.isdir(file_path):
                continue
            with fs.openbin(file_path, "r") as handle:
                payload = handle.read()
            if payload.startswith(objects.OBJECT_MAGIC):
                yield name, payload
    except AxklibContainerUnsupportedError:
        raise
    except Exception as exc:
        raise AxklibContainerUnsupportedError(
            "CONTAINER_UNSUPPORTED_FAT",
            f"unsupported FAT/floppy image {path}: {exc}",
        ) from exc
    finally:
        if fs is not None:
            try:
                fs.close()
            except Exception:
                pass

def load_fat_objects(path: Path) -> list[AxklibObject]:
    items: list[AxklibObject] = []
    for fat_name, payload in _iter_pyfatfs_objects(path):
        object_type = payload[0x0C:0x10].decode("ascii", errors="replace")
        if object_type not in SUPPORTED_NORMAL_OBJECT_TYPES:
            continue
        items.append(
            make_object(
                image=path,
                container_kind=AxklibContainerKind.FAT12_FLOPPY,
                scope_key=f"{path}:fat-root",
                object_key=f"{path.name}:{fat_name}",
                partition_index=None,
                sfs_id=None,
                fat_file=fat_name,
                payload_offset=None,
                payload_size=len(payload),
                object_type=object_type,
                name=objects.clean_ascii(payload[0x32:0x42]),
                payload=payload,
                basis="FAT/floppy file read through pyfatfs containing plain FSFSDEV3SPLX object",
            )
        )
    return items

def load_sfs_objects(path: Path) -> list[AxklibObject]:
    parsed = dumper.parse_image(path, dumper.ReadOptions(max_nodes=4, include_node_payloads=False))
    sector_size = _int_value(parsed.get("sector_size_bytes"), dumper.DEFAULT_SECTOR_SIZE)
    object_rows = sfs_scan.scan_image(path, max_nodes=4)
    items: list[AxklibObject] = []
    with dumper.ImageReader(path) as reader:
        for partition in _parsed_partitions(parsed):
            partition_index = _int_value(partition.get("index"))
            start_sector = _int_value(partition.get("start_sector"))
            sectors_per_cluster = inventory.field_value(partition, "sectors_per_cluster") or 2
            cluster_count_limit = inventory.field_value(partition, "number_of_clusters")
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
                        name=objects.clean_ascii(payload[0x32:0x42]),
                        payload=payload,
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
    for fat_name, payload in _iter_pyfatfs_objects(path):
        object_type = payload[0x0C:0x10].decode("ascii", errors="replace")
        if object_type not in SUPPORTED_NORMAL_OBJECT_TYPES:
            continue
        items.append(
            make_summary_object(
                image=path,
                container_kind=AxklibContainerKind.FAT12_FLOPPY,
                scope_key=f"{path}:fat-root",
                object_key=f"{path.name}:{fat_name}",
                partition_index=None,
                sfs_id=None,
                fat_file=fat_name,
                payload_offset=None,
                payload_size=len(payload),
                object_type=object_type,
                name=objects.clean_ascii(payload[0x32:0x42]),
                basis="FAT/floppy root file metadata from pyfatfs",
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


def load_iso_object_summaries(path: Path) -> list[AxklibObject]:
    items: list[AxklibObject] = []
    for image in _expand_iso_inputs([path]):
        for volume_id, object_path, payload in _iter_pycdlib_objects(image):
            object_type = payload[0x0C:0x10].decode("ascii", errors="replace")
            if object_type not in SUPPORTED_NORMAL_OBJECT_TYPES:
                continue
            header = object_header(payload)
            metadata: dict[str, object] = {
                "iso_inventory_method": "iso9660",
                "iso_inventory_status": "iso9660",
                "iso_recovery_quality": "clean-iso9660-object",
                "iso_recovery_notes": "object came from a pycdlib ISO9660 directory entry",
                "iso_header_size": header.header_size if header else None,
                "iso_stored_payload_size": header.stored_payload_size if header else None,
            }
            items.append(
                make_summary_object(
                    image=image,
                    container_kind=AxklibContainerKind.ISO,
                    scope_key=f"{image}:iso:{volume_id}:iso9660",
                    object_key=f"{image.name}:iso9660:{object_path}",
                    partition_index=None,
                    sfs_id=None,
                    fat_file=object_path,
                    payload_offset=None,
                    payload_size=len(payload),
                    object_type=object_type,
                    name=objects.clean_ascii(payload[0x32:0x42]),
                    basis="ISO/CD-ROM pycdlib directory object metadata summary",
                    metadata=metadata,
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


def load_iso_objects(path: Path) -> list[AxklibObject]:
    items: list[AxklibObject] = []
    for image in _expand_iso_inputs([path]):
        for volume_id, object_path, payload in _iter_pycdlib_objects(image):
            object_type = payload[0x0C:0x10].decode("ascii", errors="replace")
            if object_type not in SUPPORTED_NORMAL_OBJECT_TYPES:
                continue
            header = object_header(payload)
            metadata: dict[str, object] = {
                "iso_inventory_method": "iso9660",
                "iso_inventory_status": "iso9660",
                "iso_recovery_quality": "clean-iso9660-object",
                "iso_recovery_notes": "object came from a pycdlib ISO9660 directory entry",
                "iso_header_size": header.header_size if header else None,
                "iso_stored_payload_size": header.stored_payload_size if header else None,
            }
            items.append(
                make_object(
                    image=image,
                    container_kind=AxklibContainerKind.ISO,
                    scope_key=f"{image}:iso:{volume_id}:iso9660",
                    object_key=f"{image.name}:iso9660:{object_path}",
                    partition_index=None,
                    sfs_id=None,
                    fat_file=object_path,
                    payload_offset=None,
                    payload_size=len(payload),
                    object_type=object_type,
                    name=objects.clean_ascii(payload[0x32:0x42]),
                    payload=payload,
                    basis="ISO/CD-ROM pycdlib file containing plain FSFSDEV3SPLX object",
                    metadata=metadata,
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


def load_objects(path: Path, container: str) -> list[AxklibObject]:
    kind = detect_container_kind(path) if container == "auto" else container
    if kind == AxklibContainerKind.SFS.value:
        return load_sfs_objects(path)
    if kind in {"fat", AxklibContainerKind.FAT12_FLOPPY.value, "floppy"}:
        return load_fat_objects(path)
    if kind in {AxklibContainerKind.ISO.value, "cdrom"}:
        return load_iso_objects(path)
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
        load_objects(source, kind) if opts.include_payloads else load_object_summaries(source, kind)
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