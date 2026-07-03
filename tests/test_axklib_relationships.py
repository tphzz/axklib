from __future__ import annotations

from axklib.model import AxklibObject
from axklib.relationships import build_relationship_graph


def _object(object_key: str, object_type: str, name: str, payload: bytes, offset: int) -> AxklibObject:
    return AxklibObject(
        image="fixture.hds",
        container_kind="sfs",
        scope_key="fixture:partition:0",
        object_key=object_key,
        partition_index=0,
        sfs_id=offset,
        fat_file="",
        payload_offset=offset,
        payload_size=len(payload),
        type=object_type,
        name=name,
        payload=payload,
    )


def _sbnk_payload(name: str) -> bytes:
    payload = bytearray(0x120)
    payload[0:12] = b"FSFSDEV3SPLX"
    payload[0x0C:0x10] = b"SBNK"
    payload[0x32 : 0x32 + len(name)] = name.encode("ascii")
    return bytes(payload)


def _sbac_payload(slot_name: str) -> bytes:
    payload = bytearray(0x180)
    payload[0:12] = b"FSFSDEV3SPLX"
    payload[0x0C:0x10] = b"SBAC"
    payload[0x32 : 0x36] = b"AC01"
    payload[0x144] = 1
    payload[0x14C : 0x14C + len(slot_name)] = slot_name.encode("ascii")
    return bytes(payload)


def test_relationship_graph_preserves_and_sorts_ambiguous_candidates() -> None:
    # Deliberately provide candidates in reverse object-key order. The graph output
    # must still be deterministic and preserve both candidates.
    items = [
        _object("sbnk-b", "SBNK", "BANK", _sbnk_payload("BANK"), 20),
        _object("sbnk-a", "SBNK", "BANK", _sbnk_payload("BANK"), 10),
        _object("sbac", "SBAC", "AC01", _sbac_payload("BANK"), 30),
    ]

    graph = build_relationship_graph(items)

    sbac_rows = [row for row in graph.sbac_sbnk_rows if row.sbac_object_key == "sbac"]
    assert len(sbac_rows) == 1
    assert sbac_rows[0].match_quality == "Tentative"
    assert sbac_rows[0].candidate_object_keys == "sbnk-a|sbnk-b"
    relationships = [row for row in graph.relationships if row.relationship_type == "SBAC_SLOT_TO_SBNK"]
    assert len(relationships) == 1
    assert relationships[0].target_key == "sbnk-a|sbnk-b"
    assert graph.ambiguous() == tuple(relationships)
