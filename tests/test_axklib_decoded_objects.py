from __future__ import annotations

from pathlib import Path

from axklib.model import AxklibObject, DataQuality
from axklib.objects.decoded import FieldValue, decode_object
from axklib.reports import write_json


def _put_be16(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 2] = value.to_bytes(2, "big")


def _put_be32(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 4] = value.to_bytes(4, "big")


def _object(payload: bytes, object_type: str = "SMPL", name: str = "S01") -> AxklibObject:
    return AxklibObject(
        image="fixture.hds",
        container_kind="sfs",
        scope_key="fixture.hds:partition:0",
        object_key="p0:sfs1",
        partition_index=0,
        sfs_id=1,
        fat_file="",
        payload_offset=0x1000,
        payload_size=len(payload),
        type=object_type,
        name=name,
        payload=payload,
    )


def _current_smpl_payload() -> bytes:
    payload = bytearray(0x200)
    payload[0:12] = b"FSFSDEV3SPLX"
    payload[0x0C:0x10] = b"SMPL"
    _put_be32(payload, 0x10, 0x200)
    _put_be32(payload, 0x18, 0x7C)
    _put_be32(payload, 0x1C, 8)
    _put_be32(payload, 0x20, 8)
    _put_be16(payload, 0x28, 44100)
    _put_be16(payload, 0x2A, 2)
    payload[0x32 : 0x32 + 3] = b"S01"
    _put_be16(payload, 0x7C, 44100)
    payload[0x7E] = 64
    payload[0x7F] = 0
    payload[0x85] = 4
    _put_be32(payload, 0x92, 1000)
    _put_be32(payload, 0x96, 100)
    _put_be32(payload, 0x9A, 400)
    return bytes(payload)


def test_field_value_serializes_with_quality_and_raw_offset(tmp_path: Path) -> None:
    field = FieldValue(
        name="root_key",
        value=64,
        raw_value=64,
        raw_offset=0x7E,
        raw_size=1,
        quality=DataQuality.LIKELY,
        basis="unit test",
    )

    write_json(tmp_path / "field.json", {"field": field})

    text = (tmp_path / "field.json").read_text(encoding="utf-8")
    assert '"quality": "Likely"' in text
    assert '"raw_offset": 126' in text
    assert '"basis": "unit test"' in text


def test_decode_current_smpl_returns_field_values() -> None:
    result = decode_object(_object(_current_smpl_payload()))

    assert result.issues == ()
    assert result.decoded.decoded_kind == "DecodedSample"
    assert result.decoded.fields["sample_rate"].value == 44100
    assert result.decoded.fields["root_key"].raw_offset == 0x7E
    assert result.decoded.fields["loop_mode"].display_value == "One->"


def test_decode_short_smpl_emits_bad_header_issue() -> None:
    payload = b"FSFSDEV3SPLXSMPL" + b"\x00" * 16

    result = decode_object(_object(payload))

    assert [issue.code for issue in result.issues] == ["OBJECT_BAD_HEADER_SIZE"]
    assert result.issues[0].byte_start == 0
    assert result.issues[0].severity == "error"


def test_decode_payload_omitted_emits_structured_info_issue() -> None:
    result = decode_object(_object(b""))

    assert [issue.code for issue in result.issues] == ["OBJECT_PAYLOAD_OMITTED"]
    assert result.decoded.fields["payload_size"].value == 0
