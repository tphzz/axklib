from __future__ import annotations

from typing import Any

from axklib.model import AxklibObject
from axklib.relationships import (
    ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE,
    ACTIVE_ASSIGNMENT_STATE_CONFIRMED_DUPLICATE_NOT_ACTIVE,
    ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF,
    ACTIVE_ASSIGNMENT_STATE_SOURCE_LOAD_ASSIGNMENT,
    ACTIVE_ASSIGNMENT_STATE_UNKNOWN,
    ASSIGNMENT_ROW_STATE_DECODED,
    build_relationship_graph,
    classify_active_assignment_state,
    classify_rch_assign_display,
)


def _object(
    object_key: str,
    object_type: str,
    name: str,
    payload: bytes,
    offset: int,
    *,
    fat_file: str = "",
    metadata: dict[str, Any] | None = None,
    container_kind: str = "sfs",
) -> AxklibObject:
    return AxklibObject(
        image="fixture.hds",
        container_kind=container_kind,
        scope_key="fixture:partition:0",
        object_key=object_key,
        partition_index=0,
        sfs_id=offset,
        fat_file=fat_file,
        payload_offset=offset,
        payload_size=len(payload),
        type=object_type,
        metadata=metadata,
        name=name,
        payload=payload,
    )


def _base_payload(object_type: str, name: str, size: int) -> bytearray:
    payload = bytearray(size)
    payload[0:12] = b"FSFSDEV3SPLX"
    payload[0x0C:0x10] = object_type.encode("ascii")
    payload[0x32 : 0x32 + len(name)] = name.encode("ascii")
    return payload


def _sbnk_payload(name: str, *, member_name: str = "", member_link_id: int = 0) -> bytes:
    payload = _base_payload("SBNK", name, 0x200)
    if member_name:
        payload[0x078 : 0x078 + len(member_name)] = member_name.encode("ascii")
        payload[0x0A0:0x0A4] = member_link_id.to_bytes(4, "big")
    return bytes(payload)


def _sbac_payload(slot_name: str, *, raw_handle: int = 0) -> bytes:
    payload = _base_payload("SBAC", "AC01", 0x180)
    payload[0x144] = 1
    payload[0x14C : 0x14C + len(slot_name)] = slot_name.encode("ascii")
    payload[0x15C:0x160] = raw_handle.to_bytes(4, "big")
    return bytes(payload)


def _prog_payload(
    assignment_name: str,
    *,
    kind_byte: int,
    raw_handle: int = 0,
    rch_assign_gate: int = 0x7E,
    rch_assign_selector: int = 0xFF,
) -> bytes:
    payload = _base_payload("PROG", "001", 0x200)
    payload[0x120 : 0x120 + len(assignment_name)] = assignment_name.encode("ascii")
    payload[0x130:0x134] = raw_handle.to_bytes(4, "big")
    payload[0x134] = kind_byte
    payload[0x135] = rch_assign_selector
    payload[0x148] = rch_assign_gate
    return bytes(payload)


type ProgPayloadRow = (
    tuple[str, int, int] | tuple[str, int, int, int] | tuple[str, int, int, int, int]
)


def _prog_payload_rows(rows: list[ProgPayloadRow]) -> bytes:
    payload = _base_payload("PROG", "001", 0x400)
    for index, row in enumerate(rows):
        assignment_name = row[0]
        kind_byte = row[1]
        raw_handle = row[2]
        rch_assign_gate = row[3] if len(row) > 3 else 0x7E
        rch_assign_selector = row[4] if len(row) > 4 else 0xFF
        offset = 0x120 + index * 0x38
        payload[offset : offset + len(assignment_name)] = assignment_name.encode("ascii")
        payload[offset + 0x10 : offset + 0x14] = raw_handle.to_bytes(4, "big")
        payload[offset + 0x14] = kind_byte
        payload[offset + 0x15] = rch_assign_selector
        payload[offset + 0x28] = rch_assign_gate
    return bytes(payload)


def _sfs_placement(volume_name: str) -> dict[str, Any]:
    return {
        "sfs_placement_partition_index": 0,
        "sfs_placement_volume_name": volume_name,
        "sfs_placement_category_name": "Sample Banks",
        "sfs_placement_entry_name": "entry",
        "sfs_placement_quality": "Known",
    }


def _smpl_payload(name: str, *, link_id: int = 0) -> bytes:
    payload = _base_payload("SMPL", name, 0x120)
    payload[0x078:0x07C] = link_id.to_bytes(4, "big")
    return bytes(payload)


def test_rch_assign_display_mapper_uses_gate_and_selector_bytes() -> None:
    assert (
        classify_rch_assign_display(
            assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
            midi_receive_channel_assign_byte_0x15=0xFF,
            rch_assign_gate_byte_0x28=0x00,
        )
        == "off"
    )
    assert (
        classify_rch_assign_display(
            assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
            midi_receive_channel_assign_byte_0x15=0xFF,
            rch_assign_gate_byte_0x28=0xFF,
        )
        == "=SMP"
    )
    assert (
        classify_rch_assign_display(
            assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
            midi_receive_channel_assign_byte_0x15=0,
            rch_assign_gate_byte_0x28=0xFF,
        )
        == "01"
    )
    assert (
        classify_rch_assign_display(
            assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
            midi_receive_channel_assign_byte_0x15=15,
            rch_assign_gate_byte_0x28=0xFF,
        )
        == "16"
    )
    assert (
        classify_rch_assign_display(
            assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
            midi_receive_channel_assign_byte_0x15=16,
            rch_assign_gate_byte_0x28=0xFF,
        )
        == "BasicRch"
    )
    assert (
        classify_rch_assign_display(
            assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
            midi_receive_channel_assign_byte_0x15=17,
            rch_assign_gate_byte_0x28=0xFF,
        )
        == "B01"
    )
    assert (
        classify_rch_assign_display(
            assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
            midi_receive_channel_assign_byte_0x15=33,
            rch_assign_gate_byte_0x28=0xFF,
        )
        == "unknown"
    )


def test_active_assignment_classifier_uses_rch_assign_gate_byte() -> None:
    assert (
        classify_active_assignment_state(
            assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
            rch_assign_gate_byte_0x28=0xFF,
        )
        == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE
    )
    assert (
        classify_active_assignment_state(
            assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
            rch_assign_gate_byte_0x28=0x00,
        )
        == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF
    )
    assert (
        classify_active_assignment_state(
            assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
            rch_assign_gate_byte_0x28=0x7E,
        )
        == ACTIVE_ASSIGNMENT_STATE_UNKNOWN
    )
    assert (
        classify_active_assignment_state(assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED)
        == ACTIVE_ASSIGNMENT_STATE_UNKNOWN
    )


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
    relationships = [
        row for row in graph.relationships if row.relationship_type == "SBAC_SLOT_TO_SBNK"
    ]
    assert len(relationships) == 1
    assert relationships[0].target_key == "sbnk-a|sbnk-b"
    assert graph.ambiguous() == tuple(relationships)


def test_sbac_slot_prefers_same_folder_duplicate_candidate() -> None:
    items = [
        _object(
            "other-sbnk",
            "SBNK",
            "BANK",
            _sbnk_payload("BANK"),
            10,
            fat_file="VOL/F002/SBNK/F001",
        ),
        _object(
            "local-sbnk",
            "SBNK",
            "BANK",
            _sbnk_payload("BANK"),
            20,
            fat_file="VOL/F001/SBNK/F001",
        ),
        _object(
            "sbac",
            "SBAC",
            "AC01",
            _sbac_payload("BANK"),
            30,
            fat_file="VOL/F001/SBAC/F001",
        ),
    ]

    graph = build_relationship_graph(items)

    sbac_row = next(row for row in graph.sbac_sbnk_rows if row.sbac_object_key == "sbac")
    assert sbac_row.match_quality == "Likely"
    assert sbac_row.match_method == "active-sbac-slot-name+same-folder"
    assert sbac_row.matched_sbnk_object_key == "local-sbnk"
    assert sbac_row.candidate_object_keys == "local-sbnk|other-sbnk"
    relationship = next(
        row for row in graph.relationships if row.relationship_type == "SBAC_SLOT_TO_SBNK"
    )
    assert relationship.target_key == "local-sbnk"
    assert relationship.quality == "Likely"


def test_sbac_slot_prefers_same_sfs_volume_duplicate_candidate() -> None:
    items = [
        _object(
            "vol-b-sbnk",
            "SBNK",
            "BANK",
            _sbnk_payload("BANK"),
            10,
            metadata=_sfs_placement("Vol B"),
        ),
        _object(
            "vol-a-sbnk",
            "SBNK",
            "BANK",
            _sbnk_payload("BANK"),
            20,
            metadata=_sfs_placement("Vol A"),
        ),
        _object(
            "sbac",
            "SBAC",
            "AC01",
            _sbac_payload("BANK"),
            30,
            metadata=_sfs_placement("Vol A"),
        ),
    ]

    graph = build_relationship_graph(items)

    sbac_row = next(row for row in graph.sbac_sbnk_rows if row.sbac_object_key == "sbac")
    assert sbac_row.match_quality == "Likely"
    assert sbac_row.match_method == "active-sbac-slot-name+same-volume"
    assert sbac_row.matched_sbnk_object_key == "vol-a-sbnk"
    assert sbac_row.candidate_object_keys == "vol-a-sbnk|vol-b-sbnk"
    relationship = next(
        row for row in graph.relationships if row.relationship_type == "SBAC_SLOT_TO_SBNK"
    )
    assert relationship.target_key == "vol-a-sbnk"
    assert relationship.quality == "Likely"
    assert relationship.basis == "active-sbac-slot-name+same-volume"


def test_sbac_same_sfs_volume_rule_requires_unique_candidate() -> None:
    items = [
        _object(
            "vol-a-sbnk-1",
            "SBNK",
            "BANK",
            _sbnk_payload("BANK"),
            10,
            metadata=_sfs_placement("Vol A"),
        ),
        _object(
            "vol-a-sbnk-2",
            "SBNK",
            "BANK",
            _sbnk_payload("BANK"),
            20,
            metadata=_sfs_placement("Vol A"),
        ),
        _object(
            "vol-b-sbnk",
            "SBNK",
            "BANK",
            _sbnk_payload("BANK"),
            30,
            metadata=_sfs_placement("Vol B"),
        ),
        _object(
            "sbac",
            "SBAC",
            "AC01",
            _sbac_payload("BANK"),
            40,
            metadata=_sfs_placement("Vol A"),
        ),
    ]

    graph = build_relationship_graph(items)

    sbac_row = next(row for row in graph.sbac_sbnk_rows if row.sbac_object_key == "sbac")
    assert sbac_row.match_quality == "Tentative"
    assert sbac_row.match_method == "active-sbac-slot-name-ambiguous"
    assert sbac_row.matched_sbnk_object_key == ""
    assert sbac_row.candidate_object_keys == "vol-a-sbnk-1|vol-a-sbnk-2|vol-b-sbnk"


def test_prog_assignment_prefers_same_folder_duplicate_candidate() -> None:
    items = [
        _object(
            "other-sbac",
            "SBAC",
            "GROUP",
            _sbac_payload("BANK"),
            10,
            fat_file="VOL/F002/SBAC/F001",
        ),
        _object(
            "local-sbac",
            "SBAC",
            "GROUP",
            _sbac_payload("BANK"),
            20,
            fat_file="VOL/F001/SBAC/F001",
        ),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload("GROUP", kind_byte=0x11),
            30,
            fat_file="VOL/F001/PROG/F001",
        ),
    ]

    graph = build_relationship_graph(items)

    prog_row = next(row for row in graph.prog_bank_rows if row.prog_object_key == "prog")
    assert prog_row.match_quality == "Likely"
    assert prog_row.match_method == "assignment-kind-0x11+name+same-folder"
    assert prog_row.assignment_row_state == ASSIGNMENT_ROW_STATE_DECODED
    assert prog_row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_UNKNOWN
    assert prog_row.assignment_rch_assign_display == "unknown"
    assert prog_row.matched_target_object_key == "local-sbac"
    assert prog_row.candidate_object_keys == "local-sbac|other-sbac"
    relationship = next(
        row for row in graph.relationships if row.relationship_type == "PROG_ASSIGNMENT_TO_SBAC"
    )
    assert relationship.target_key == "local-sbac"
    assert relationship.quality == "Likely"


def test_iso_source_load_prog_assignment_promotes_same_folder_sbnk_candidate() -> None:
    items = [
        _object(
            "other-sbnk",
            "SBNK",
            "sine wave",
            _sbnk_payload("sine wave"),
            10,
            fat_file="VOL/F002/SBNK/F002",
            container_kind="iso",
        ),
        _object(
            "local-sbnk",
            "SBNK",
            "sine wave",
            _sbnk_payload("sine wave"),
            20,
            fat_file="VOL/F001/SBNK/F002",
            container_kind="iso",
        ),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload("sine wave", kind_byte=0x10, rch_assign_gate=0x00),
            30,
            fat_file="VOL/F001/PROG/F003",
            container_kind="iso",
        ),
    ]

    graph = build_relationship_graph(items)

    prog_row = next(row for row in graph.prog_bank_rows if row.prog_object_key == "prog")
    assert prog_row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_SOURCE_LOAD_ASSIGNMENT
    assert prog_row.match_quality == "Known"
    assert prog_row.match_method == "assignment-kind-0x10+name+same-folder"
    assert prog_row.matched_target_object_key == "local-sbnk"
    assert prog_row.candidate_object_keys == "local-sbnk|other-sbnk"
    relationship = next(
        row for row in graph.relationships if row.relationship_type == "PROG_ASSIGNMENT_TO_SBNK"
    )
    assert relationship.target_key == "local-sbnk"
    assert relationship.quality == "Known"


def test_iso_source_load_prog_assignment_promotes_same_folder_sbac_candidate() -> None:
    items = [
        _object(
            "other-sbac",
            "SBAC",
            "GROUP",
            _sbac_payload("BANK"),
            10,
            fat_file="VOL/F002/SBAC/F001",
            container_kind="iso",
        ),
        _object(
            "local-sbac",
            "SBAC",
            "GROUP",
            _sbac_payload("BANK"),
            20,
            fat_file="VOL/F001/SBAC/F001",
            container_kind="iso",
        ),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload("GROUP", kind_byte=0x11, rch_assign_gate=0x00),
            30,
            fat_file="VOL/F001/PROG/F001",
            container_kind="iso",
        ),
    ]

    graph = build_relationship_graph(items)

    prog_row = next(row for row in graph.prog_bank_rows if row.prog_object_key == "prog")
    assert prog_row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_SOURCE_LOAD_ASSIGNMENT
    assert prog_row.match_quality == "Known"
    assert prog_row.match_method == "assignment-kind-0x11+name+same-folder"
    assert prog_row.matched_target_object_key == "local-sbac"
    relationship = next(
        row for row in graph.relationships if row.relationship_type == "PROG_ASSIGNMENT_TO_SBAC"
    )
    assert relationship.target_key == "local-sbac"
    assert relationship.quality == "Known"


def test_prog_assignment_kind_selects_unique_sbac_when_sbnk_shares_name() -> None:
    items = [
        _object("same-name-sbnk", "SBNK", "GROUP", _sbnk_payload("GROUP"), 10),
        _object("same-name-sbac", "SBAC", "GROUP", _sbac_payload("GROUP"), 20),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload("GROUP", kind_byte=0x11, rch_assign_gate=0xFF),
            30,
        ),
    ]

    graph = build_relationship_graph(items)

    row = next(row for row in graph.prog_bank_rows if row.prog_object_key == "prog")
    assert row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE
    assert row.match_quality == "Known"
    assert row.match_method == "assignment-kind-0x11+name"
    assert row.matched_target_type == "SBAC"
    assert row.matched_target_object_key == "same-name-sbac"
    relationship = next(
        item for item in graph.relationships if item.relationship_type == "PROG_ASSIGNMENT_TO_SBAC"
    )
    assert relationship.quality == "Known"
    assert relationship.target_key == "same-name-sbac"


def test_prog_assignment_kind_selects_unique_sbnk_when_sbac_shares_name() -> None:
    items = [
        _object("same-name-sbac", "SBAC", "GROUP", _sbac_payload("GROUP"), 10),
        _object("same-name-sbnk", "SBNK", "GROUP", _sbnk_payload("GROUP"), 20),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload("GROUP", kind_byte=0x10, rch_assign_gate=0xFF),
            30,
        ),
    ]

    graph = build_relationship_graph(items)

    row = next(row for row in graph.prog_bank_rows if row.prog_object_key == "prog")
    assert row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE
    assert row.match_quality == "Known"
    assert row.match_method == "assignment-kind-0x10+name"
    assert row.matched_target_type == "SBNK"
    assert row.matched_target_object_key == "same-name-sbnk"
    relationship = next(
        item for item in graph.relationships if item.relationship_type == "PROG_ASSIGNMENT_TO_SBNK"
    )
    assert relationship.quality == "Known"
    assert relationship.target_key == "same-name-sbnk"


def test_active_prog_assignment_prefers_same_sfs_volume_sbac_candidate() -> None:
    items = [
        _object(
            "vol-b-sbac",
            "SBAC",
            "GROUP",
            _sbac_payload("BANK"),
            10,
            metadata=_sfs_placement("Vol B"),
        ),
        _object(
            "vol-a-sbac",
            "SBAC",
            "GROUP",
            _sbac_payload("BANK"),
            20,
            metadata=_sfs_placement("Vol A"),
        ),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload("GROUP", kind_byte=0x11, rch_assign_gate=0xFF),
            30,
            metadata=_sfs_placement("Vol A"),
        ),
    ]

    graph = build_relationship_graph(items)

    row = next(row for row in graph.prog_bank_rows if row.prog_object_key == "prog")
    assert row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE
    assert row.match_quality == "Likely"
    assert row.match_method == "assignment-kind-0x11+name+same-volume"
    assert row.matched_target_object_key == "vol-a-sbac"
    assert row.candidate_object_keys == "vol-a-sbac|vol-b-sbac"


def test_active_prog_assignment_prefers_same_sfs_volume_sbnk_candidate() -> None:
    items = [
        _object(
            "vol-b-sbnk",
            "SBNK",
            "BD",
            _sbnk_payload("BD"),
            10,
            metadata=_sfs_placement("Vol B"),
        ),
        _object(
            "vol-a-sbnk",
            "SBNK",
            "BD",
            _sbnk_payload("BD"),
            20,
            metadata=_sfs_placement("Vol A"),
        ),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload("BD", kind_byte=0x10, rch_assign_gate=0xFF),
            30,
            metadata=_sfs_placement("Vol A"),
        ),
    ]

    graph = build_relationship_graph(items)

    row = next(row for row in graph.prog_bank_rows if row.prog_object_key == "prog")
    assert row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE
    assert row.match_quality == "Likely"
    assert row.match_method == "assignment-kind-0x10+name+same-volume"
    assert row.matched_target_object_key == "vol-a-sbnk"
    assert row.candidate_object_keys == "vol-a-sbnk|vol-b-sbnk"


def test_same_sfs_volume_prog_assignment_classifies_visible_off_as_diagnostic() -> None:
    items = [
        _object(
            "vol-b-sbac",
            "SBAC",
            "GROUP",
            _sbac_payload("BANK"),
            10,
            metadata=_sfs_placement("Vol B"),
        ),
        _object(
            "vol-a-sbac",
            "SBAC",
            "GROUP",
            _sbac_payload("BANK"),
            20,
            metadata=_sfs_placement("Vol A"),
        ),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload("GROUP", kind_byte=0x11, rch_assign_gate=0x00),
            30,
            metadata=_sfs_placement("Vol A"),
        ),
    ]

    graph = build_relationship_graph(items)

    row = next(row for row in graph.prog_bank_rows if row.prog_object_key == "prog")
    assert row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF
    assert row.match_quality == "Tentative"
    assert row.match_method == "assignment-visible-off-same-volume-sbac-diagnostic"
    assert row.matched_target_object_key == ""
    assert row.candidate_object_keys == "vol-a-sbac|vol-b-sbac"
    assert "assignment-visible-off-same-volume-sbac-diagnostic" in row.notes
    relationship = next(
        item for item in graph.relationships if item.relationship_type == "PROG_ASSIGNMENT_TO_SBAC"
    )
    assert relationship.quality == "Tentative"
    assert relationship.basis == "assignment-visible-off-same-volume-sbac-diagnostic"
    assert relationship.diagnostic_category == "visible-off-assignment"
    assert relationship.target_key == "vol-a-sbac|vol-b-sbac"


def test_same_sfs_volume_visible_off_sbnk_assignment_gets_sbnk_diagnostic() -> None:
    items = [
        _object(
            "vol-b-sbnk",
            "SBNK",
            "PAD",
            _sbnk_payload("PAD"),
            10,
            metadata=_sfs_placement("Vol B"),
        ),
        _object(
            "vol-a-sbnk",
            "SBNK",
            "PAD",
            _sbnk_payload("PAD"),
            20,
            metadata=_sfs_placement("Vol A"),
        ),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload("PAD", kind_byte=0x10, rch_assign_gate=0x00),
            30,
            metadata=_sfs_placement("Vol A"),
        ),
    ]

    graph = build_relationship_graph(items)

    row = next(row for row in graph.prog_bank_rows if row.prog_object_key == "prog")
    assert row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF
    assert row.match_quality == "Tentative"
    assert row.match_method == "assignment-visible-off-same-volume-sbnk-diagnostic"
    assert row.matched_target_object_key == ""
    assert row.candidate_object_keys == "vol-a-sbnk|vol-b-sbnk"
    assert "assignment-visible-off-same-volume-sbnk-diagnostic" in row.notes
    relationship = next(
        item for item in graph.relationships if item.relationship_type == "PROG_ASSIGNMENT_TO_SBNK"
    )
    assert relationship.quality == "Tentative"
    assert relationship.basis == "assignment-visible-off-same-volume-sbnk-diagnostic"
    assert relationship.diagnostic_category == "visible-off-assignment"
    assert relationship.target_key == "vol-a-sbnk|vol-b-sbnk"


def test_visible_off_same_volume_diagnostic_requires_unique_candidate() -> None:
    items = [
        _object(
            "vol-a-sbac-1",
            "SBAC",
            "GROUP",
            _sbac_payload("BANK"),
            10,
            metadata=_sfs_placement("Vol A"),
        ),
        _object(
            "vol-a-sbac-2",
            "SBAC",
            "GROUP",
            _sbac_payload("BANK"),
            20,
            metadata=_sfs_placement("Vol A"),
        ),
        _object(
            "vol-b-sbac",
            "SBAC",
            "GROUP",
            _sbac_payload("BANK"),
            30,
            metadata=_sfs_placement("Vol B"),
        ),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload("GROUP", kind_byte=0x11, rch_assign_gate=0x00),
            40,
            metadata=_sfs_placement("Vol A"),
        ),
    ]

    graph = build_relationship_graph(items)

    row = next(row for row in graph.prog_bank_rows if row.prog_object_key == "prog")
    assert row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF
    assert row.match_quality == "Tentative"
    assert row.match_method == "assignment-visible-off-name-ambiguous-sbac"
    assert row.matched_target_object_key == ""
    assert row.candidate_object_keys == "vol-a-sbac-1|vol-a-sbac-2|vol-b-sbac"

    relationship = next(
        item for item in graph.relationships if item.relationship_type == "PROG_ASSIGNMENT_TO_SBAC"
    )
    assert relationship.diagnostic_category == "visible-off-assignment"


def test_iso_zeroed_matched_prog_assignment_is_source_load_not_hda_off() -> None:
    items = [
        _object(
            "target-sbac",
            "SBAC",
            "CFIII DrkTmprd",
            _sbac_payload("CFIII 026-S"),
            10,
            container_kind="iso",
        ),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload("CFIII DrkTmprd", kind_byte=0x11, rch_assign_gate=0x00),
            20,
            container_kind="iso",
        ),
    ]

    graph = build_relationship_graph(items)

    row = next(row for row in graph.prog_bank_rows if row.prog_object_key == "prog")
    assert row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_SOURCE_LOAD_ASSIGNMENT
    assert row.assignment_output1_byte_0x1d == 0x00
    assert row.assignment_rch_assign_gate_byte_0x28 == 0x00
    assert row.assignment_rch_assign_display == "unknown"
    assert row.match_quality == "Known"
    assert row.match_method == "assignment-kind-0x11+name"
    assert row.matched_target_object_key == "target-sbac"
    assert "source-load-assignment" in row.notes
    relationship = next(
        item for item in graph.relationships if item.relationship_type == "PROG_ASSIGNMENT_TO_SBAC"
    )
    assert relationship.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_SOURCE_LOAD_ASSIGNMENT
    assert relationship.assignment_rch_assign_display == "unknown"


def test_duplicate_iso_source_load_prog_assignment_marks_later_row_not_active() -> None:
    items = [
        _object(
            "target-sbac",
            "SBAC",
            "CFIII DrkTmprd",
            _sbac_payload("CFIII 026-S"),
            10,
            container_kind="iso",
        ),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload_rows(
                [
                    ("CFIII DrkTmprd", 0x11, 0x09134910, 0x00),
                    ("CFIII DrkTmprd", 0x11, 0x09134910, 0x00),
                ]
            ),
            20,
            container_kind="iso",
        ),
    ]

    graph = build_relationship_graph(items)

    rows = sorted(graph.prog_bank_rows, key=lambda row: row.assignment_index)
    assert [row.active_assignment_state for row in rows] == [
        ACTIVE_ASSIGNMENT_STATE_SOURCE_LOAD_ASSIGNMENT,
        ACTIVE_ASSIGNMENT_STATE_CONFIRMED_DUPLICATE_NOT_ACTIVE,
    ]
    assert [row.assignment_rch_assign_display for row in rows] == ["unknown", "unknown"]


def test_visible_off_sbnk_ambiguous_prog_assignment_gets_diagnostic_label() -> None:
    items = [
        _object("sbnk-a", "SBNK", "PAD", _sbnk_payload("PAD"), 10),
        _object("sbnk-b", "SBNK", "PAD", _sbnk_payload("PAD"), 20),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload("PAD", kind_byte=0x10, rch_assign_gate=0x00),
            30,
        ),
    ]

    graph = build_relationship_graph(items)

    row = next(row for row in graph.prog_bank_rows if row.prog_object_key == "prog")
    assert row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF
    assert row.match_quality == "Tentative"
    assert row.match_method == "assignment-visible-off-name-ambiguous-sbnk"
    assert row.matched_target_object_key == ""
    assert row.candidate_categories == "SBNK|SBNK"
    assert row.candidate_object_keys == "sbnk-a|sbnk-b"
    assert "visible-off-name-ambiguous-sbnk" in row.notes

    relationship = next(
        item for item in graph.relationships if item.relationship_type == "PROG_ASSIGNMENT_TO_SBNK"
    )
    assert relationship.diagnostic_category == "visible-off-assignment"


def test_visible_off_non_target_category_ambiguous_prog_assignment_gets_diagnostic_label() -> None:
    items = [
        _object("smpl-a", "SMPL", "WAVE", _smpl_payload("WAVE"), 10),
        _object("smpl-b", "SMPL", "WAVE", _smpl_payload("WAVE"), 20),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload("WAVE", kind_byte=0x10, rch_assign_gate=0x00),
            30,
        ),
    ]

    graph = build_relationship_graph(items)

    row = next(row for row in graph.prog_bank_rows if row.prog_object_key == "prog")
    assert row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF
    assert row.match_quality == "Tentative"
    assert row.match_method == "assignment-visible-off-name-ambiguous-smpl-candidates"
    assert row.selector_expected_category == "SBNK"
    assert row.matched_target_object_key == ""
    assert row.candidate_categories == "SMPL|SMPL"
    assert row.candidate_object_keys == "smpl-a|smpl-b"
    assert "assignment-visible-off-name-ambiguous-smpl-candidates" in row.notes

    relationship = next(
        item for item in graph.relationships if item.relationship_type == "PROG_ASSIGNMENT_TO_SBNK"
    )
    assert relationship.diagnostic_category == "visible-off-assignment"


def test_active_unmatched_direct_prog_assignment_stays_unresolved() -> None:
    items = [
        _object(
            "local-sbnk",
            "SBNK",
            "INDIAN 1",
            _sbnk_payload("INDIAN 1"),
            10,
            metadata=_sfs_placement("Vol A"),
        ),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload(
                "INDIAN         7",
                kind_byte=0x10,
                raw_handle=0x12345678,
                rch_assign_gate=0xFF,
            ),
            20,
            metadata=_sfs_placement("Vol A"),
        ),
    ]

    graph = build_relationship_graph(items)

    row = next(row for row in graph.prog_bank_rows if row.prog_object_key == "prog")
    assert row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE
    assert row.match_quality == "Unknown"
    assert row.match_method == "assignment-active-missing-local-target"
    assert row.matched_target_object_key == ""
    assert row.candidate_object_keys == ""
    assert "missing local target" in row.match_notes
    assert "active-missing-local-target" in row.notes

    relationship = next(
        item for item in graph.relationships if item.relationship_type == "PROG_ASSIGNMENT_TO_SBNK"
    )
    assert relationship.diagnostic_category == "active-assignment-missing-target"


def test_duplicate_prog_assignment_rows_mark_later_rows_not_active() -> None:
    items = [
        _object("target-sbac", "SBAC", "GROUP", _sbac_payload("BANK"), 10),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload_rows(
                [
                    ("GROUP", 0x11, 0x01000000, 0xFF, 0x00),
                    ("GROUP", 0x11, 0x09000000, 0x00, 0xFF),
                ]
            ),
            20,
        ),
    ]

    graph = build_relationship_graph(items)

    rows = sorted(graph.prog_bank_rows, key=lambda row: row.assignment_index)
    assert rows[0].active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE
    assert rows[0].assignment_rch_assign_display == "01"
    assert rows[1].active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_DUPLICATE_NOT_ACTIVE
    assert rows[1].assignment_rch_assign_display == "off"


def test_duplicate_prog_assignment_keeps_later_active_gate_active() -> None:
    items = [
        _object("target-sbac", "SBAC", "GROUP", _sbac_payload("BANK"), 10),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload_rows(
                [
                    ("GROUP", 0x11, 0x01000000, 0xFF),
                    ("GROUP", 0x11, 0x01000000, 0xFF),
                ]
            ),
            20,
        ),
    ]

    graph = build_relationship_graph(items)

    rows = sorted(graph.prog_bank_rows, key=lambda row: row.assignment_index)
    assert [row.active_assignment_state for row in rows] == [
        ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE,
        ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE,
    ]


def test_duplicate_prog_assignment_rule_requires_same_target() -> None:
    items = [
        _object("target-a", "SBAC", "A", _sbac_payload("BANK"), 10),
        _object("target-b", "SBAC", "B", _sbac_payload("BANK"), 20),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload_rows(
                [
                    ("A", 0x11, 0x01000000),
                    ("B", 0x11, 0x09000000),
                ]
            ),
            30,
        ),
    ]

    graph = build_relationship_graph(items)

    rows = sorted(graph.prog_bank_rows, key=lambda row: row.assignment_index)
    assert [row.active_assignment_state for row in rows] == [
        ACTIVE_ASSIGNMENT_STATE_UNKNOWN,
        ACTIVE_ASSIGNMENT_STATE_UNKNOWN,
    ]


def test_prog_assignment_uses_program_local_resolved_target_context() -> None:
    items = [
        _object(
            "local-sbac",
            "SBAC",
            "Sa nv",
            _sbac_payload("KY_OSA_NV_S_036_"),
            10,
        ),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload_rows(
                [
                    ("Sa nv", 0x11, 0x014454C0),
                    ("06 Santana nv", 0x11, 0x09135930),
                ]
            ),
            20,
        ),
    ]

    graph = build_relationship_graph(items)

    context_row = next(
        row for row in graph.prog_bank_rows if row.assignment_name == "06 Santana nv"
    )
    assert context_row.match_quality == "Likely"
    assert context_row.match_method == "assignment-kind-0x11+program-local-target-context"
    assert context_row.assignment_raw_handle_0x10 == 0x09135930
    assert context_row.matched_target_object_key == "local-sbac"
    assert context_row.candidate_object_keys == "local-sbac"
    assert "raw handle" in context_row.match_notes


def test_prog_assignment_context_resolver_requires_one_local_target() -> None:
    items = [
        _object("sbac-a", "SBAC", "A", _sbac_payload("BANK"), 10),
        _object("sbac-b", "SBAC", "B", _sbac_payload("BANK"), 20),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload_rows(
                [
                    ("A", 0x11, 0x01000000),
                    ("B", 0x11, 0x02000000),
                    ("STALE NAME", 0x11, 0x09135930),
                ]
            ),
            30,
        ),
    ]

    graph = build_relationship_graph(items)

    stale_row = next(row for row in graph.prog_bank_rows if row.assignment_name == "STALE NAME")
    assert stale_row.match_quality == "Unknown"
    assert stale_row.match_method == "assignment-unmatched"
    assert stale_row.matched_target_object_key == ""


def test_sbnk_member_name_only_unique_match_is_likely() -> None:
    items = [
        _object("smpl", "SMPL", "SAMPLE", _smpl_payload("SAMPLE", link_id=0x2222), 10),
        _object(
            "sbnk",
            "SBNK",
            "BANK",
            _sbnk_payload("BANK", member_name="SAMPLE", member_link_id=0x1111),
            20,
        ),
    ]

    graph = build_relationship_graph(items)

    relationship = next(
        row for row in graph.relationships if row.relationship_type == "SBNK_LEFT_MEMBER_TO_SMPL"
    )
    assert relationship.target_key == "smpl"
    assert relationship.quality == "Likely"
    assert relationship.basis == "sbnk-member-name-only"


def test_sbnk_member_link_id_only_name_mismatch_gets_precise_basis() -> None:
    items = [
        _object("smpl", "SMPL", "OTHER", _smpl_payload("OTHER", link_id=0x1111), 10),
        _object(
            "sbnk",
            "SBNK",
            "BANK",
            _sbnk_payload("BANK", member_name="SAMPLE", member_link_id=0x1111),
            20,
        ),
    ]

    graph = build_relationship_graph(items)

    relationship = next(
        row for row in graph.relationships if row.relationship_type == "SBNK_LEFT_MEMBER_TO_SMPL"
    )
    assert relationship.target_key == "smpl"
    assert relationship.quality == "Tentative"
    assert relationship.basis == "sbnk-member-link-id-only-name-mismatch"
    assert relationship.diagnostic_category == "sbnk-member-link"


def test_sbnk_member_link_id_only_iso_cross_folder_name_mismatch_gets_precise_basis() -> None:
    items = [
        _object(
            "smpl",
            "SMPL",
            "OTHER",
            _smpl_payload("OTHER", link_id=0x1111),
            10,
            container_kind="iso",
            fat_file="VOL/F002/SMPL/F029",
        ),
        _object(
            "sbnk",
            "SBNK",
            "BANK",
            _sbnk_payload("BANK", member_name="SAMPLE", member_link_id=0x1111),
            20,
            container_kind="iso",
            fat_file="VOL/F003/SBNK/F062",
        ),
    ]

    graph = build_relationship_graph(items)

    relationship = next(
        row for row in graph.relationships if row.relationship_type == "SBNK_LEFT_MEMBER_TO_SMPL"
    )
    assert relationship.target_key == "smpl"
    assert relationship.quality == "Tentative"
    assert relationship.basis == "sbnk-member-link-id-only-iso-cross-folder-name-mismatch"
    assert relationship.diagnostic_category == "sbnk-member-link"


def test_sbnk_program_bitmap_mismatch_gets_diagnostic_basis() -> None:
    payload = bytearray(_sbnk_payload("BANK"))
    payload[0x0C0:0x0C4] = (1).to_bytes(4, "big")
    items = [_object("sbnk", "SBNK", "BANK", bytes(payload), 10)]

    graph = build_relationship_graph(items)

    relationship = next(
        row for row in graph.relationships if row.relationship_type == "SBNK_PROGRAM_BITMAP_TO_PROG"
    )
    assert relationship.target_key == "001"
    assert relationship.quality == "Tentative"
    assert (
        relationship.basis
        == "sbnk-program-link-bitmap-bitmap-without-decoded-direct-assignment-diagnostic"
    )
    assert relationship.diagnostic_category == "program-link-bitmap"


def test_sbnk_program_bitmap_disambiguates_ambiguous_direct_assignment_basis() -> None:
    payload = bytearray(_sbnk_payload("BANK"))
    payload[0x0C0:0x0C4] = (1).to_bytes(4, "big")
    items = [
        _object("sbnk-a", "SBNK", "BANK", bytes(payload), 10),
        _object("sbnk-b", "SBNK", "BANK", _sbnk_payload("BANK"), 20),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload("BANK", kind_byte=0x10, rch_assign_gate=0xFF),
            30,
        ),
    ]

    graph = build_relationship_graph(items)

    bitmap_row = next(row for row in graph.sbnk_bitmap_rows if row.sbnk_object_key == "sbnk-a")
    assert bitmap_row.mismatch_class == "bitmap_disambiguates_ambiguous_direct_assignment"
    relationship = next(
        row
        for row in graph.relationships
        if row.relationship_type == "SBNK_PROGRAM_BITMAP_TO_PROG" and row.source_key == "sbnk-a"
    )
    assert relationship.target_key == "001"
    assert relationship.quality == "Tentative"
    assert (
        relationship.basis
        == "sbnk-program-link-bitmap-bitmap-disambiguates-ambiguous-direct-assignment-diagnostic"
    )
    assert relationship.diagnostic_category == "program-link-bitmap"


def test_sbnk_program_bitmap_known_direct_assignment_missing_basis() -> None:
    payload = bytearray(_sbnk_payload("BANK"))
    payload[0x0C0:0x0C4] = (1).to_bytes(4, "big")
    items = [
        _object("sbnk", "SBNK", "BANK", bytes(payload), 10),
        _object(
            "prog-001",
            "PROG",
            "001",
            _prog_payload("BANK", kind_byte=0x10, rch_assign_gate=0xFF),
            20,
        ),
        _object(
            "prog-002",
            "PROG",
            "002",
            _prog_payload("BANK", kind_byte=0x10, rch_assign_gate=0xFF),
            30,
        ),
    ]

    graph = build_relationship_graph(items)

    bitmap_row = next(row for row in graph.sbnk_bitmap_rows if row.sbnk_object_key == "sbnk")
    assert bitmap_row.mismatch_class == "known_direct_assignment_missing_bitmap"
    assert bitmap_row.bitmap_programs == "001"
    assert bitmap_row.direct_assignment_without_bitmap_programs == "002"
    relationship = next(
        row for row in graph.relationships if row.relationship_type == "SBNK_PROGRAM_BITMAP_TO_PROG"
    )
    assert relationship.target_key == "001"
    assert relationship.quality == "Tentative"
    assert (
        relationship.basis
        == "sbnk-program-link-bitmap-known-direct-assignment-missing-bitmap-diagnostic"
    )
    assert relationship.diagnostic_category == "program-link-bitmap"


def test_prog_assignment_keeps_raw_handle_diagnostic_when_name_is_unmatched() -> None:
    items = [
        _object(
            "target-sbac",
            "SBAC",
            "GROUP",
            _sbac_payload("BANK"),
            10,
            fat_file="VOL/F001/SBAC/F001",
            metadata={
                "iso_extent_sector": 123,
                "iso_data_offset": 251904,
                "iso_recovery_quality": "Known",
            },
        ),
        _object(
            "known-prog",
            "PROG",
            "001",
            _prog_payload("GROUP", kind_byte=0x11, raw_handle=0x09137000),
            20,
            fat_file="VOL/F001/PROG/F001",
        ),
        _object(
            "handle-prog",
            "PROG",
            "002",
            _prog_payload("STALE NAME", kind_byte=0x11, raw_handle=0x09137000),
            30,
            fat_file="VOL/F002/PROG/F001",
            metadata={
                "iso_extent_sector": 456,
                "iso_data_offset": 933888,
                "iso_recovery_quality": "Known",
            },
        ),
    ]

    graph = build_relationship_graph(items)

    prog_row = next(row for row in graph.prog_bank_rows if row.prog_object_key == "handle-prog")
    assert prog_row.match_quality == "Unknown"
    assert prog_row.match_method == "assignment-unmatched"
    assert prog_row.assignment_raw_handle_0x10 == 0x09137000
    assert prog_row.matched_target_object_key == ""
    assert prog_row.candidate_object_keys == ""
    assert prog_row.prog_iso_extent_sector == 456
    assert prog_row.prog_iso_data_offset == 933888
    assert prog_row.matched_target_iso_extent_sector is None
    assert prog_row.matched_target_iso_data_offset is None
    assert prog_row.matched_target_iso_recovery_quality == ""
    assert not any(
        row.source_key == "handle-prog" and row.target_key == "target-sbac"
        for row in graph.relationships
    )


def test_null_handle_unmatched_prog_assignment_is_ignored() -> None:
    items = [
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload("MISSING", kind_byte=0x11, raw_handle=0),
            10,
            fat_file="VOL/F001/PROG/F001",
        ),
    ]

    graph = build_relationship_graph(items)

    assert not graph.prog_bank_rows
    assert len(graph.prog_ignored_rows) == 1
    assert graph.prog_ignored_rows[0].reason == "ignored-null-handle-unmatched-assignment"
    assert not graph.relationships


def test_reserved_tail_prog_rows_after_last_active_assignment_are_suppressed() -> None:
    items = [
        _object(
            "target-sbac",
            "SBAC",
            "BANK",
            _sbac_payload("SAMPLE"),
            10,
            fat_file="VOL/F001/SBAC/F001",
        ),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload_rows(
                [
                    ("BANK", 0x11, 0x09130000),
                    ("TAIL", 0x00, 0x5E2C0120),
                ]
            ),
            20,
            fat_file="VOL/F001/PROG/F001",
        ),
    ]

    graph = build_relationship_graph(items)

    assert len(graph.prog_bank_rows) == 1
    assert graph.prog_bank_rows[0].assignment_index == 0
    assert graph.prog_ignored_rows == ()


def test_reserved_prog_row_inside_active_assignment_range_stays_visible() -> None:
    items = [
        _object(
            "target-sbac",
            "SBAC",
            "BANK",
            _sbac_payload("SAMPLE"),
            10,
            fat_file="VOL/F001/SBAC/F001",
        ),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload_rows(
                [
                    ("TAIL", 0x00, 0x5E2C0120),
                    ("BANK", 0x11, 0x09130000),
                ]
            ),
            20,
            fat_file="VOL/F001/PROG/F001",
        ),
    ]

    graph = build_relationship_graph(items)

    assert len(graph.prog_bank_rows) == 1
    assert graph.prog_bank_rows[0].assignment_index == 1
    assert len(graph.prog_ignored_rows) == 1
    assert graph.prog_ignored_rows[0].assignment_index == 0
    assert (
        graph.prog_ignored_rows[0].reason
        == "ignored-reserved-or-tail-slot-no-known-kind-and-no-name-match"
    )


def test_prog_assignment_raw_handle_does_not_select_same_folder_candidate() -> None:
    items = [
        _object(
            "local-sbac",
            "SBAC",
            "LOCAL",
            _sbac_payload("BANK"),
            10,
            fat_file="VOL/F001/SBAC/F001",
        ),
        _object(
            "other-sbac",
            "SBAC",
            "OTHER",
            _sbac_payload("BANK"),
            20,
            fat_file="VOL/F002/SBAC/F001",
        ),
        _object(
            "known-local-prog",
            "PROG",
            "001",
            _prog_payload("LOCAL", kind_byte=0x11, raw_handle=0x09137000),
            30,
            fat_file="VOL/F001/PROG/F001",
        ),
        _object(
            "known-other-prog",
            "PROG",
            "002",
            _prog_payload("OTHER", kind_byte=0x11, raw_handle=0x09137000),
            40,
            fat_file="VOL/F002/PROG/F001",
        ),
        _object(
            "handle-prog",
            "PROG",
            "003",
            _prog_payload("STALE NAME", kind_byte=0x11, raw_handle=0x09137000),
            50,
            fat_file="VOL/F001/PROG/F002",
        ),
    ]

    graph = build_relationship_graph(items)

    prog_row = next(row for row in graph.prog_bank_rows if row.prog_object_key == "handle-prog")
    assert prog_row.match_quality == "Unknown"
    assert prog_row.match_method == "assignment-unmatched"
    assert prog_row.assignment_raw_handle_0x10 == 0x09137000
    assert prog_row.matched_target_object_key == ""
    assert prog_row.candidate_object_keys == ""


def test_prog_assignment_raw_handle_does_not_use_sbac_slot_handle() -> None:
    items = [
        _object(
            "target-sbnk",
            "SBNK",
            "BANK",
            _sbnk_payload("BANK"),
            10,
            fat_file="VOL/F001/SBNK/F001",
        ),
        _object(
            "sbac",
            "SBAC",
            "GROUP",
            _sbac_payload("BANK", raw_handle=0x09137048),
            20,
            fat_file="VOL/F001/SBAC/F001",
        ),
        _object(
            "handle-prog",
            "PROG",
            "001",
            _prog_payload("STALE NAME", kind_byte=0x10, raw_handle=0x09137048),
            30,
            fat_file="VOL/F002/PROG/F001",
        ),
    ]

    graph = build_relationship_graph(items)

    prog_row = next(row for row in graph.prog_bank_rows if row.prog_object_key == "handle-prog")
    assert prog_row.match_quality == "Unknown"
    assert prog_row.match_method == "assignment-unmatched-preserved-source-selector"
    assert prog_row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_UNKNOWN
    assert prog_row.assignment_raw_handle_0x10 == 0x09137048
    assert prog_row.matched_target_object_key == ""
    assert prog_row.candidate_object_keys == ""


def test_unmatched_direct_prog_assignment_reports_preserved_source_selector() -> None:
    items = [
        _object(
            "local-sbnk",
            "SBNK",
            "AFOXE CAIXINHA1",
            _sbnk_payload("AFOXE CAIXINHA1"),
            10,
            fat_file="VOL/F001/SBNK/F001",
        ),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload("TripHop 1", kind_byte=0x10, raw_handle=0x091350A8),
            20,
            fat_file="VOL/F001/PROG/F001",
        ),
    ]

    graph = build_relationship_graph(items)

    prog_row = next(row for row in graph.prog_bank_rows if row.prog_object_key == "prog")
    assert prog_row.match_quality == "Unknown"
    assert prog_row.match_method == "assignment-unmatched-preserved-source-selector"
    assert prog_row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_UNKNOWN
    assert prog_row.assignment_raw_handle_0x10 == 0x091350A8
    assert prog_row.candidate_count == 0
    assert prog_row.candidate_object_keys == ""
    assert prog_row.matched_target_object_key == ""
    assert "diagnostics only" in prog_row.match_notes
    assert "raw-selector-diagnostic-only" in prog_row.notes

    relationship = next(
        row for row in graph.relationships if row.relationship_type == "PROG_ASSIGNMENT_TO_SBNK"
    )
    assert relationship.quality == "Unknown"
    assert relationship.target_key == ""
    assert relationship.basis == "assignment-unmatched-preserved-source-selector"


def test_visible_off_direct_prog_assignment_reports_missing_local_target() -> None:
    items = [
        _object(
            "local-sbnk",
            "SBNK",
            "AFOXE CAIXINHA1",
            _sbnk_payload("AFOXE CAIXINHA1"),
            10,
            fat_file="VOL/F001/SBNK/F001",
        ),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload(
                "TripHop 1",
                kind_byte=0x10,
                raw_handle=0x091350A8,
                rch_assign_gate=0x00,
            ),
            20,
            fat_file="VOL/F001/PROG/F001",
        ),
    ]

    graph = build_relationship_graph(items)

    prog_row = next(row for row in graph.prog_bank_rows if row.prog_object_key == "prog")
    assert prog_row.match_quality == "Unknown"
    assert prog_row.match_method == "assignment-visible-off-missing-local-sbnk"
    assert prog_row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF
    assert prog_row.assignment_raw_handle_0x10 == 0x091350A8
    assert prog_row.candidate_count == 0
    assert prog_row.candidate_object_keys == ""
    assert prog_row.matched_target_object_key == ""
    assert "not treat it as active Program content" in prog_row.match_notes
    assert "visible-off-missing-local-sbnk" in prog_row.notes
    assert "raw-selector-diagnostic-only" in prog_row.notes

    relationship = next(
        row for row in graph.relationships if row.relationship_type == "PROG_ASSIGNMENT_TO_SBNK"
    )
    assert relationship.quality == "Unknown"
    assert relationship.target_key == ""
    assert relationship.basis == "assignment-visible-off-missing-local-sbnk"
    assert relationship.diagnostic_category == "visible-off-assignment"


def test_iso_visible_off_sbac_prog_assignment_reports_missing_local_group() -> None:
    items = [
        _object(
            "local-sbac",
            "SBAC",
            "EXISTING GROUP",
            _sbac_payload("EXISTING BANK"),
            10,
            fat_file="VOL/F001/SBAC/F001",
            container_kind="iso",
        ),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload(
                "Tower Chords",
                kind_byte=0x11,
                raw_handle=0x091350A8,
                rch_assign_gate=0x00,
            ),
            20,
            fat_file="VOL/F001/PROG/F001",
            container_kind="iso",
        ),
    ]

    graph = build_relationship_graph(items)

    prog_row = next(row for row in graph.prog_bank_rows if row.prog_object_key == "prog")
    assert prog_row.match_quality == "Unknown"
    assert prog_row.match_method == "assignment-visible-off-iso-missing-local-sbac"
    assert prog_row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF
    assert prog_row.selector_expected_category == "SBAC"
    assert prog_row.candidate_count == 0
    assert prog_row.matched_target_object_key == ""
    assert "assignment-visible-off-iso-missing-local-sbac" in prog_row.notes
    assert "raw-selector-diagnostic-only" in prog_row.notes

    relationship = next(
        row for row in graph.relationships if row.relationship_type == "PROG_ASSIGNMENT_TO_SBAC"
    )
    assert relationship.quality == "Unknown"
    assert relationship.target_key == ""
    assert relationship.basis == "assignment-visible-off-iso-missing-local-sbac"
    assert relationship.diagnostic_category == "visible-off-assignment"


def test_visible_off_sbac_prog_assignment_reports_missing_local_group() -> None:
    items = [
        _object(
            "local-sbac",
            "SBAC",
            "EXISTING GROUP",
            _sbac_payload("EXISTING BANK"),
            10,
            fat_file="VOL/F001/SBAC/F001",
        ),
        _object(
            "prog",
            "PROG",
            "001",
            _prog_payload(
                "Tower Chords",
                kind_byte=0x11,
                raw_handle=0x091350A8,
                rch_assign_gate=0x00,
            ),
            20,
            fat_file="VOL/F001/PROG/F001",
        ),
    ]

    graph = build_relationship_graph(items)

    prog_row = next(row for row in graph.prog_bank_rows if row.prog_object_key == "prog")
    assert prog_row.match_quality == "Unknown"
    assert prog_row.match_method == "assignment-visible-off-missing-local-sbac"
    assert prog_row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF
    assert prog_row.selector_expected_category == "SBAC"
    assert prog_row.candidate_count == 0
    assert prog_row.candidate_object_keys == ""
    assert prog_row.matched_target_object_key == ""
    assert "not treat it as active Program content" in prog_row.match_notes
    assert "visible-off-missing-local-sbac" in prog_row.notes
    assert "raw-selector-diagnostic-only" in prog_row.notes

    relationship = next(
        row for row in graph.relationships if row.relationship_type == "PROG_ASSIGNMENT_TO_SBAC"
    )
    assert relationship.quality == "Unknown"
    assert relationship.target_key == ""
    assert relationship.basis == "assignment-visible-off-missing-local-sbac"
    assert relationship.diagnostic_category == "visible-off-assignment"
