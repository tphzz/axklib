"""Canonical axklib domain model."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from dataclasses import dataclass
from enum import StrEnum
from typing import Any


class DataQuality(StrEnum):
    """Quality quality level used across decoded fields, relationships, and reports.

    Use this enum to distinguish proven behavior from likely correlations,
    hypotheses, and unknown raw data without collapsing ambiguity.
    """

    KNOWN = "Known"
    LIKELY = "Likely"
    TENTATIVE = "Tentative"
    UNKNOWN = "Unknown"


class AxklibObjectFormat(StrEnum):
    """Physical/object-format classification for a loaded Yamaha object.

    Use this to keep normal current objects separate from alternating-byte or
    artifact patterns that are readable but not promoted as ordinary
    supported storage.
    """

    NORMAL = "normal-fsfsdev3splx"
    ALTERNATING_BYTE_ARTIFACT = "alternating-byte-artifact"
    UNKNOWN = "unknown"


class AxklibObjectType(StrEnum):
    """Known Yamaha object type codes exposed by the model.

    Use this instead of comparing raw strings when branching on SMPL, SBNK,
    SBAC, PROG, SEQU, or currently unknown object types.
    """

    SMPL = "SMPL"
    SBNK = "SBNK"
    SBAC = "SBAC"
    PROG = "PROG"
    SEQU = "SEQU"
    PRF3 = "PRF3"
    UNKNOWN = "UNKNOWN"


class AxklibContainerKind(StrEnum):
    """Supported container families that can hold Yamaha objects.

    The value identifies the outer image/container layer, not the Yamaha object
    type inside it.
    """

    SFS = "sfs"
    FAT12_FLOPPY = "fat12_floppy"
    ISO = "iso"
    UNKNOWN = "unknown"

@dataclass(frozen=True)
class AxklibQuality:
    """Compact quality label attached to decoded data.
    
    Use it when a value needs a quality level, source description, and optional notes but does not require byte-level field origin."""
    quality: DataQuality
    source: str
    notes: str = ""


@dataclass(frozen=True)
class AxklibContainerRef:
    """Stable identity for the source container that produced an object.
    
    Use `scope_key` together with `source_image` when combining multiple images so object keys from different sources cannot collide."""
    source_image: str
    kind: AxklibContainerKind
    scope_key: str


@dataclass(frozen=True)
class AxklibVolumeRef:
    """Sampler-facing placement metadata for an object inside a partition, volume, and category.
    
    Use this for navigation and export layout; do not treat missing fields as basis that an object is unowned or unused."""
    partition_index: int | None = None
    partition_name: str = ""
    volume_name: str = ""
    volume_path: str = ""
    category_code: str = ""
    category_name: str = ""


@dataclass(frozen=True)
class AxklibObjectRef:
    """Stable identity and physical location for one loaded Yamaha object.
    
    The reference preserves both logical keys and container-specific location hints such as SFS ID, FAT filename, payload offset, and payload size."""
    object_key: str
    partition_index: int | None
    sfs_id: int | None
    fat_file: str
    payload_offset: int | None
    payload_size: int


@dataclass(frozen=True)
class AxklibObjectHeader:
    """Small normalized summary of an object header.
    
    Use this for generic object diagnostics before handing payload bytes to a type-specific decoder."""
    header_size: int | None
    stored_payload_size: int | None
    raw_prefix_hex: str


@dataclass(frozen=True)
class FieldQuality:
    """Byte-level quality for one decoded field.
    
    Use this when a report or sidecar must explain which raw offset and bytes support a displayed value."""
    quality: DataQuality
    basis: str
    notes: str = ""
    raw_offset: int | None = None
    raw_size: int | None = None
    raw_bytes_hex: str = ""


@dataclass(frozen=True)
class FieldValue[T]:
    """Decoded field value with raw storage and quality metadata.
    
    Use this for parameter decoders that need to keep display value, raw value, offset, size, quality, and basis notes together."""
    name: str
    value: T
    raw_value: object
    raw_offset: int | None
    raw_size: int | None
    quality: DataQuality
    basis: str
    notes: str = ""
    display_value: str = ""

    @property
    def field_quality(self) -> FieldQuality:
        return FieldQuality(
            quality=self.quality,
            basis=self.basis,
            notes=self.notes,
            raw_offset=self.raw_offset,
            raw_size=self.raw_size,
        )


@dataclass(frozen=True)
class DecodeIssue:
    """Structured issue emitted while decoding one object or field.
    
    Use this instead of free-form warnings when a malformed field, unsupported layout, or boundary problem should be testable and reportable."""
    code: str
    severity: str
    object_key: str
    message: str
    quality: DataQuality = DataQuality.KNOWN
    basis: str = "decoder"
    byte_start: int | None = None
    byte_end: int | None = None
    field_name: str = ""
    object_ref: AxklibObjectRef | None = None


@dataclass(frozen=True)
class AxklibExtensionRecord:
    """Base class for typed, namespaced extension metadata.

    This replaces anonymous catch-all metadata over time. Temporary mappings are
    still accepted during migration, but each producer should move stable values
    into a narrow extension record.
    """

    namespace: str
    producer: str
    quality: AxklibQuality | None = None


@dataclass(frozen=True)
class SfsLocationMetadata(AxklibExtensionRecord):
    """Typed extension metadata for Yamaha SFS hard-disk object placement.
    
    Use it when an object came from SFS and its partition, SFS ID, object offset, or Y-node offset is known."""
    partition_index: int | None = None
    sfs_id: int | None = None
    object_offset: int | None = None
    y_node_offset: int | None = None


@dataclass(frozen=True)
class FatDirectoryMetadata(AxklibExtensionRecord):
    """Typed extension metadata for Yamaha objects loaded from a FAT floppy image.
    
    Use it to preserve the FAT filename and directory index without pretending those values are SFS locations."""
    fat_file: str = ""
    directory_index: int | None = None


@dataclass(frozen=True)
class IsoRecoveryMetadata(AxklibExtensionRecord):
    """Typed extension metadata for Yamaha objects loaded from ISO9660 media.
    
    Use it to record ISO inventory quality and recovery status so downstream reports can distinguish clean ISO traversal from weaker source quality."""
    recovery_quality: str = ""
    inventory_status: str = ""
    notes: str = ""


@dataclass(frozen=True)
class ArtifactClassificationMetadata(AxklibExtensionRecord):
    """Typed extension metadata for suspected conversion-artifact or alternating-byte compatibility classifications.
    
    Use it to keep compatibility/export labels explicit without promoting those objects to normal supported-format quality."""
    label: str = ""
    classification_quality: DataQuality = DataQuality.UNKNOWN
    notes: str = ""


@dataclass(frozen=True)
class SourceMatchMetadata(AxklibExtensionRecord):
    """Typed extension metadata for payload matches to external source objects.
    
    Use it to carry source-recovered metadata or conflict notes separately from decoded on-disk fields."""
    source_path: str = ""
    match_quality: str = ""
    conflict_notes: str = ""


@dataclass(frozen=True, init=False)
class AxklibObject:
    """A loaded Yamaha object plus stable origin.

    The canonical representation is nested refs. The constructor also accepts
    the old flat ObjectItem keyword shape while reports are being migrated.
    """

    ref: AxklibObjectRef
    container: AxklibContainerRef
    volume: AxklibVolumeRef | None
    object_type: AxklibObjectType
    object_format: AxklibObjectFormat
    name: str
    _payload: bytes | None
    _payload_loader: Callable[[], bytes] | None
    header: AxklibObjectHeader | None
    quality: AxklibQuality
    metadata: dict[str, Any]
    temporary_extensions: dict[str, AxklibExtensionRecord]

    def __init__(
        self,
        *,
        ref: AxklibObjectRef | None = None,
        container: AxklibContainerRef | None = None,
        volume: AxklibVolumeRef | None = None,
        object_type: AxklibObjectType | str | None = None,
        object_format: AxklibObjectFormat | str = AxklibObjectFormat.NORMAL,
        name: str,
        payload: bytes | None = None,
        payload_loader: Callable[[], bytes] | None = None,
        header: AxklibObjectHeader | None = None,
        quality: AxklibQuality | None = None,
        metadata: dict[str, Any] | None = None,
        temporary_extensions: Mapping[str, AxklibExtensionRecord] | None = None,
        image: str | None = None,
        container_kind: AxklibContainerKind | str | None = None,
        scope_key: str | None = None,
        object_key: str | None = None,
        partition_index: int | None = None,
        sfs_id: int | None = None,
        fat_file: str = "",
        payload_offset: int | None = None,
        payload_size: int | None = None,
        type: str | AxklibObjectType | None = None,
    ) -> None:
        if object_type is None:
            object_type = type
        if object_type is None:
            object_type = AxklibObjectType.UNKNOWN
        normalized_type = coerce_object_type(object_type)
        normalized_format = coerce_object_format(object_format)

        if container is None:
            if image is None or container_kind is None or scope_key is None:
                raise TypeError("container or image/container_kind/scope_key is required")
            container = AxklibContainerRef(
                source_image=image,
                kind=coerce_container_kind(container_kind),
                scope_key=scope_key,
            )
        if ref is None:
            if object_key is None:
                raise TypeError("ref or object_key is required")
            ref = AxklibObjectRef(
                object_key=object_key,
                partition_index=partition_index,
                sfs_id=sfs_id,
                fat_file=fat_file,
                payload_offset=payload_offset,
                payload_size=len(payload or b"") if payload_size is None else payload_size,
            )
        if quality is None:
            quality = AxklibQuality(
                quality=DataQuality.KNOWN,
                source="direct container object load",
            )

        object.__setattr__(self, "ref", ref)
        object.__setattr__(self, "container", container)
        object.__setattr__(self, "volume", volume)
        object.__setattr__(self, "object_type", normalized_type)
        object.__setattr__(self, "object_format", normalized_format)
        object.__setattr__(self, "name", name)
        object.__setattr__(self, "_payload", payload)
        object.__setattr__(self, "_payload_loader", payload_loader)
        object.__setattr__(self, "header", header)
        object.__setattr__(self, "quality", quality)
        object.__setattr__(self, "metadata", {} if metadata is None else dict(metadata))
        object.__setattr__(
            self,
            "temporary_extensions",
            {} if temporary_extensions is None else dict(temporary_extensions),
        )

    @property
    def payload(self) -> bytes:
        payload = self._payload
        if payload is None:
            loader = self._payload_loader
            payload = b"" if loader is None else loader()
            object.__setattr__(self, "_payload", payload)
            object.__setattr__(self, "_payload_loader", None)
        return payload

    @property
    def image(self) -> str:
        return self.container.source_image

    @property
    def container_kind(self) -> str:
        return self.container.kind.value

    @property
    def scope_key(self) -> str:
        return self.container.scope_key

    @property
    def object_key(self) -> str:
        return self.ref.object_key

    @property
    def partition_index(self) -> int | None:
        return self.ref.partition_index

    @property
    def sfs_id(self) -> int | None:
        return self.ref.sfs_id

    @property
    def fat_file(self) -> str:
        return self.ref.fat_file

    @property
    def payload_offset(self) -> int | None:
        return self.ref.payload_offset

    @property
    def payload_size(self) -> int:
        return self.ref.payload_size

    @property
    def type(self) -> str:
        return self.object_type.value

    @property
    def format(self) -> str:
        return self.object_format.value


def coerce_object_type(value: AxklibObjectType | str) -> AxklibObjectType:
    if isinstance(value, AxklibObjectType):
        return value
    try:
        return AxklibObjectType(value)
    except ValueError:
        return AxklibObjectType.UNKNOWN


def coerce_object_format(value: AxklibObjectFormat | str) -> AxklibObjectFormat:
    if isinstance(value, AxklibObjectFormat):
        return value
    try:
        return AxklibObjectFormat(value)
    except ValueError:
        return AxklibObjectFormat.UNKNOWN


def coerce_container_kind(value: AxklibContainerKind | str) -> AxklibContainerKind:
    if isinstance(value, AxklibContainerKind):
        return value
    try:
        return AxklibContainerKind(value)
    except ValueError:
        return AxklibContainerKind.UNKNOWN

