from pathlib import Path

from axklib.containers import detect_container_kind, expand_inputs
from axklib.model import (
    AxklibContainerKind,
    AxklibContainerRef,
    AxklibObject,
    AxklibObjectFormat,
    AxklibObjectRef,
    AxklibObjectType,
    AxklibQuality,
    DataQuality,
    DecodeIssue,
    FieldQuality,
    FieldValue,
    SourceMatchMetadata,
)
from axklib.reports import write_json, write_rows_json


def test_axklib_object_accepts_flat_objectitem_shape() -> None:
    item = AxklibObject(
        image="image.hds",
        container_kind="sfs",
        scope_key="scope",
        object_key="p0:sfs9",
        partition_index=0,
        sfs_id=9,
        fat_file="",
        payload_offset=0x5EA00,
        payload_size=4,
        type="PROG",
        name="001",
        payload=b"test",
        metadata={"loader_quality": "fixture"},
    )

    assert item.image == "image.hds"
    assert item.container_kind == "sfs"
    assert item.scope_key == "scope"
    assert item.object_key == "p0:sfs9"
    assert item.type == "PROG"
    assert item.object_type == AxklibObjectType.PROG
    assert item.object_format == AxklibObjectFormat.NORMAL
    assert item.metadata["loader_quality"] == "fixture"


def test_axklib_object_supports_canonical_nested_refs() -> None:
    item = AxklibObject(
        ref=AxklibObjectRef(
            object_key="fixture.iso:iso9660:P001",
            partition_index=None,
            sfs_id=None,
            fat_file="P001",
            payload_offset=0xA800,
            payload_size=8,
        ),
        container=AxklibContainerRef(
            source_image="fixture.iso",
            kind=AxklibContainerKind.ISO,
            scope_key="fixture.iso:iso:TESTVOL",
        ),
        volume=None,
        object_type=AxklibObjectType.SBNK,
        object_format=AxklibObjectFormat.ALTERNATING_BYTE_ARTIFACT,
        name="Bank",
        payload=b"12345678",
        quality=AxklibQuality(
            quality=DataQuality.TENTATIVE,
            source="test",
            notes="artifact classification is explicit",
        ),
    )

    assert item.image == "fixture.iso"
    assert item.container_kind == "iso"
    assert item.object_key == "fixture.iso:iso9660:P001"
    assert item.type == "SBNK"
    assert item.format == "alternating-byte-artifact"
    assert item.quality.quality == DataQuality.TENTATIVE


def test_axklib_object_payload_loader_materializes_once() -> None:
    calls = 0

    def load_payload() -> bytes:
        nonlocal calls
        calls += 1
        return b"lazy-payload"

    item = AxklibObject(
        image="image.hds",
        container_kind="sfs",
        scope_key="scope",
        object_key="p0:sfs9",
        partition_index=0,
        sfs_id=9,
        fat_file="",
        payload_offset=0,
        payload_size=12,
        type="SMPL",
        name="S01",
        payload_loader=load_payload,
    )

    assert calls == 0
    assert item.payload == b"lazy-payload"
    assert item.payload == b"lazy-payload"
    assert calls == 1


def test_axklib_object_supports_typed_temporary_extensions() -> None:
    quality = AxklibQuality(quality=DataQuality.LIKELY, source="unit test")
    extension = SourceMatchMetadata(
        namespace="source-match",
        producer="tests",
        quality=quality,
        source_path="source.iso:/S01",
        match_quality="exact-payload",
    )
    item = AxklibObject(
        image="image.hds",
        container_kind="sfs",
        scope_key="scope",
        object_key="p0:sfs9",
        partition_index=0,
        sfs_id=9,
        fat_file="",
        payload_offset=0,
        payload_size=0,
        type="SMPL",
        name="S01",
        payload=b"",
        temporary_extensions={"source-match": extension},
    )

    source_extension = item.temporary_extensions["source-match"]
    assert isinstance(source_extension, SourceMatchMetadata)
    assert source_extension.source_path == "source.iso:/S01"
    assert source_extension.quality == quality


def test_report_json_serializes_enums_and_dataclasses(tmp_path: Path) -> None:
    item = AxklibObject(
        image="image.hds",
        container_kind="sfs",
        scope_key="scope",
        object_key="p0:sfs9",
        partition_index=0,
        sfs_id=9,
        fat_file="",
        payload_offset=0,
        payload_size=0,
        type="SMPL",
        name="S01",
        payload=b"",
    )

    write_rows_json(tmp_path / "rows.json", [item])
    write_json(tmp_path / "single.json", {"item": item, "quality": DataQuality.KNOWN})

    rows_text = (tmp_path / "rows.json").read_text(encoding="utf-8")
    single_text = (tmp_path / "single.json").read_text(encoding="utf-8")
    assert '"object_type": "SMPL"' in rows_text
    assert '"kind": "sfs"' in rows_text
    assert '"quality": "Known"' in single_text
    assert '"metadata": {}' in rows_text


def test_expand_inputs_expands_globs(tmp_path: Path) -> None:
    first = tmp_path / "a.hds"
    second = tmp_path / "b.hds"
    ignored = tmp_path / "c.txt"
    first.write_bytes(b"")
    second.write_bytes(b"")
    ignored.write_bytes(b"")

    assert expand_inputs([tmp_path / "*.hds"]) == [first, second]


def test_detect_container_kind_prefers_iso_extension_over_fat_like_bytes(tmp_path: Path) -> None:
    image = bytearray(512)
    image[11:13] = (512).to_bytes(2, "little")
    image[13] = 1
    image[14:16] = (1).to_bytes(2, "little")
    image[16] = 2
    image[17:19] = (224).to_bytes(2, "little")
    image[19:21] = (2880).to_bytes(2, "little")
    image[21] = 0xF0
    image[22:24] = (9).to_bytes(2, "little")
    image[24:26] = (18).to_bytes(2, "little")
    image[26:28] = (2).to_bytes(2, "little")
    path = tmp_path / "fat_like.iso"
    path.write_bytes(image)

    assert detect_container_kind(path) == AxklibContainerKind.ISO.value


def test_field_value_exposes_typed_quality() -> None:
    field = FieldValue(
        name="loop_mode",
        value=4,
        raw_value=4,
        raw_offset=0x85,
        raw_size=1,
        quality=DataQuality.KNOWN,
        basis="validated UI check",
        notes="One->",
        display_value="One->",
    )

    quality = field.field_quality

    assert isinstance(quality, FieldQuality)
    assert quality.quality == DataQuality.KNOWN
    assert quality.raw_offset == 0x85
    assert quality.raw_size == 1
    assert field.display_value == "One->"


def test_decode_issue_can_carry_stable_field_and_object_refs() -> None:
    ref = AxklibObjectRef(
        object_key="p0:sfs9",
        partition_index=0,
        sfs_id=9,
        fat_file="",
        payload_offset=0x5EA00,
        payload_size=16,
    )
    issue = DecodeIssue(
        code="OBJECT_BAD_HEADER_SIZE",
        severity="error",
        object_key=ref.object_key,
        message="too short",
        field_name="payload",
        object_ref=ref,
    )

    assert issue.code == "OBJECT_BAD_HEADER_SIZE"
    assert issue.field_name == "payload"
    assert issue.object_ref == ref

