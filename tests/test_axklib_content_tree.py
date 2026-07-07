from __future__ import annotations

from pathlib import Path

from axklib import content_tree
from axklib.containers import AxklibContainer
from axklib.model import AxklibObject
from axklib.relationships import (
    ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE,
    ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF,
    ACTIVE_ASSIGNMENT_STATE_SOURCE_LOAD_ASSIGNMENT,
    ASSIGNMENT_ROW_STATE_DECODED,
    Relationship,
    RelationshipGraph,
)


def _object(
    object_key: str,
    object_type: str,
    name: str,
    payload: bytes = b"",
    *,
    fat_file: str = "",
    metadata: dict[str, object] | None = None,
) -> AxklibObject:
    return AxklibObject(
        image="fixture.ima",
        container_kind="fat12_floppy",
        scope_key="fixture",
        object_key=object_key,
        partition_index=None,
        sfs_id=None,
        fat_file=fat_file,
        payload_offset=None,
        payload_size=0,
        type=object_type,
        name=name,
        payload=payload,
        metadata=metadata or {},
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


def _sbac_payload(slot_name: str) -> bytes:
    payload = _base_payload("SBAC", "AC01", 0x180)
    payload[0x144] = 1
    payload[0x14C : 0x14C + len(slot_name)] = slot_name.encode("ascii")
    return bytes(payload)


def _prog_payload(assignment_name: str, *, kind_byte: int, rch_assign_gate: int = 0xFF) -> bytes:
    payload = _base_payload("PROG", "001", 0x200)
    payload[0x078:0x080] = b"BROKEN  "
    payload[0x120 : 0x120 + len(assignment_name)] = assignment_name.encode("ascii")
    payload[0x134] = kind_byte
    payload[0x135] = 0xFF
    payload[0x148] = rch_assign_gate
    return bytes(payload)


def _find_node_by_display(
    nodes: tuple[content_tree.ContentNode, ...], display_name: str
) -> content_tree.ContentNode | None:
    for node in nodes:
        if node.display_name == display_name:
            return node
        found = _find_node_by_display(node.children, display_name)
        if found is not None:
            return found
    return None


def _find_plain_node_by_display(nodes: list[object], display_name: str) -> dict[str, object] | None:
    for node in nodes:
        if not isinstance(node, dict):
            continue
        if node.get("display_name") == display_name:
            return node
        children = node.get("children")
        if isinstance(children, list):
            found = _find_plain_node_by_display(children, display_name)
            if found is not None:
                return found
    return None


def test_content_tree_surfaces_active_reachable_sbnk_member_defect() -> None:
    program = _object(
        "prog",
        "PROG",
        "001",
        _prog_payload("BANK", kind_byte=0x11),
        fat_file="VOL/F001/PROG/F001",
    )
    sbac = _object(
        "sbac",
        "SBAC",
        "AC01",
        _sbac_payload("BANK"),
        fat_file="VOL/F001/SBAC/F001",
    )
    sbnk = _object(
        "sbnk-broken",
        "SBNK",
        "BANK",
        _sbnk_payload("BANK", member_name="MISSING", member_link_id=0x1111),
        fat_file="VOL/F001/SBNK/F001",
    )
    container = AxklibContainer(
        source_path=Path("fixture.iso"),
        kind="iso",
        detected_format="iso",
        objects=(program, sbac, sbnk),
        recovery_quality_summary={},
    )

    tree = content_tree.build_content_tree_for_container(container)
    rendered = content_tree.render_content_tree_text(tree)
    payload = content_tree.content_tree_to_json(tree)

    assert len(tree.issues) == 1
    issue = tree.issues[0]
    assert issue.code == "REL_ACTIVE_PROGRAM_SBNK_MEMBER_TARGET_MISSING"
    assert issue.severity == "error"
    assert issue.sampler_path.startswith("VOL/BROKEN | VOL/BROKEN/PROG/F001: assignment 1 BANK")
    assert "error: REL_ACTIVE_PROGRAM_SBNK_MEMBER_TARGET_MISSING" in rendered
    assert "volume: VOL/BROKEN" in rendered
    assert "active Program examples: VOL/BROKEN/PROG/F001: assignment 1 BANK" in rendered
    assert "BROKEN (errors detected)" in rendered
    assert "reachable from active Program assignments" not in rendered
    assert "active Program examples: VOL/BROKEN/PROG/F001: assignment 1 BANK\n\n" in rendered
    issues = payload["issues"]
    assert isinstance(issues, list)
    assert issues[0]["sampler_path"].startswith("VOL/BROKEN | VOL/BROKEN/PROG/F001")


def test_content_tree_uses_only_confirmed_active_direct_program_assignments(monkeypatch) -> None:
    prog_payload = bytearray(0x180)
    prog_payload[0x078:0x080] = b"ACTIVE  "
    program = _object("prog1", "PROG", "001", bytes(prog_payload))
    active_bank = _object("sbnk-active", "SBNK", "Active Bank")
    off_bank = _object("sbnk-off", "SBNK", "Off Bank")
    graph = RelationshipGraph(
        relationships=(
            Relationship(
                key="prog-active-bank",
                source_key="prog1",
                target_key="sbnk-active",
                relationship_type="PROG_ASSIGNMENT_TO_SBNK",
                quality="Likely",
                basis="test",
                assignment_index=0,
                assignment_name="Active Bank",
                assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
                active_assignment_state=ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE,
                assignment_rch_assign_display="01",
            ),
            Relationship(
                key="prog-off-bank",
                source_key="prog1",
                target_key="sbnk-off",
                relationship_type="PROG_ASSIGNMENT_TO_SBNK",
                quality="Known",
                basis="test",
                assignment_index=1,
                assignment_name="Off Bank",
                assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
                active_assignment_state=ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF,
                assignment_rch_assign_display="off",
            ),
        ),
        sbac_sbnk_rows=(),
        prog_bank_rows=(),
        prog_ignored_rows=(),
        sbnk_bitmap_rows=(),
    )
    monkeypatch.setattr(content_tree, "build_relationship_graph", lambda _items: graph)
    container = AxklibContainer(
        source_path=Path("fixture.ima"),
        kind="fat12_floppy",
        detected_format="fat12_floppy",
        objects=(program, active_bank, off_bank),
        recovery_quality_summary={},
    )

    tree = content_tree.build_content_tree_for_container(container)
    rendered = content_tree.render_content_tree_text(tree)
    payload = content_tree.content_tree_to_json(tree)

    program_node = _find_node_by_display(tree.roots, "001: ACTIVE")
    assert program_node is not None
    assert [child.display_name for child in program_node.children] == ["Active Bank"]
    assert program_node.children[0].details == ("Rch Assign: 01",)
    assert program_node.children[0].quality == content_tree.DataQuality.LIKELY
    assert program_node.children[0].basis == "test"
    assert "Active Bank [SAMPLE BANK] - Rch Assign: 01" in rendered
    assert "Rch Assign: off" not in rendered
    roots = payload["roots"]
    assert isinstance(roots, list)
    plain_active = _find_plain_node_by_display(roots, "Active Bank")
    assert plain_active is not None
    assert plain_active["details"] == ["Rch Assign: 01"]


def test_content_tree_shows_active_missing_local_target_only_when_unresolved_requested(
    monkeypatch,
) -> None:
    prog_payload = bytearray(0x180)
    prog_payload[0x078:0x080] = b"INDIA   "
    program = _object("prog1", "PROG", "009", bytes(prog_payload))
    resolved_bank = _object("sbnk-resolved", "SBNK", "Resolved Bank")
    graph = RelationshipGraph(
        relationships=(
            Relationship(
                key="prog-resolved-bank",
                source_key="prog1",
                target_key="sbnk-resolved",
                relationship_type="PROG_ASSIGNMENT_TO_SBNK",
                quality="Known",
                basis="assignment-kind-0x10+name",
                ambiguity_notes="resolved row detail that belongs in CSV reports",
                assignment_index=1,
                assignment_name="Resolved Bank",
                assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
                active_assignment_state=ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE,
                assignment_rch_assign_display="=SMP",
            ),
            Relationship(
                key="prog-missing-bank",
                source_key="prog1",
                target_key="",
                relationship_type="PROG_ASSIGNMENT_TO_SBNK",
                quality="Unknown",
                basis="assignment-active-missing-local-target",
                assignment_index=7,
                assignment_name="INDIAN 7",
                assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
                active_assignment_state=ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE,
                assignment_rch_assign_display="=SMP",
            ),
        ),
        sbac_sbnk_rows=(),
        prog_bank_rows=(),
        prog_ignored_rows=(),
        sbnk_bitmap_rows=(),
    )
    monkeypatch.setattr(content_tree, "build_relationship_graph", lambda _items: graph)
    container = AxklibContainer(
        source_path=Path("fixture.ima"),
        kind="fat12_floppy",
        detected_format="fat12_floppy",
        objects=(program, resolved_bank),
        recovery_quality_summary={},
    )

    default_tree = content_tree.build_content_tree_for_container(container)
    unresolved_tree = content_tree.build_content_tree_for_container(
        container, include_unresolved=True
    )
    default_rendered = content_tree.render_content_tree_text(default_tree)
    unresolved_rendered = content_tree.render_content_tree_text(
        unresolved_tree, content_tree.ContentTreeRenderOptions(show_unresolved=True)
    )

    assert "009: INDIA" in default_rendered
    assert "Resolved Bank" in default_rendered
    assert "INDIAN 7" not in default_rendered
    assert "Resolved Bank [SAMPLE BANK] - Rch Assign: =SMP" in unresolved_rendered
    assert "resolved row detail that belongs in CSV reports" not in unresolved_rendered
    unresolved_lines = unresolved_rendered.splitlines()
    missing_line = next(
        line for line in unresolved_lines if "INDIAN 7 [UNKNOWN] - Rch Assign: =SMP" in line
    )
    resolved_line = next(line for line in unresolved_lines if "Resolved Bank" in line)
    assert unresolved_lines.index(missing_line) < unresolved_lines.index(resolved_line)
    assert (
        "INDIAN 7 [UNKNOWN] - Rch Assign: =SMP [Unknown] - active assignment references missing local target"
        in unresolved_rendered
    )


def test_content_tree_uses_confirmed_active_sbac_program_assignments(monkeypatch) -> None:
    prog_payload = bytearray(0x180)
    prog_payload[0x078:0x080] = b"GROUP   "
    program = _object("prog1", "PROG", "001", bytes(prog_payload))
    bank_group = _object("sbac1", "SBAC", "TEKCHORD")
    bank = _object("sbnk1", "SBNK", "TEKCHORD C3")
    graph = RelationshipGraph(
        relationships=(
            Relationship(
                key="prog-bank",
                source_key="prog1",
                target_key="sbac1",
                relationship_type="PROG_ASSIGNMENT_TO_SBAC",
                quality="Likely",
                basis="test",
                assignment_index=0,
                assignment_name="TEKCHORD",
                assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
                active_assignment_state=ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE,
                assignment_rch_assign_display="=SMP",
            ),
            Relationship(
                key="group-bank",
                source_key="sbac1",
                target_key="sbnk1",
                relationship_type="SBAC_SLOT_TO_SBNK",
                quality="Known",
                basis="test",
            ),
        ),
        sbac_sbnk_rows=(),
        prog_bank_rows=(),
        prog_ignored_rows=(),
        sbnk_bitmap_rows=(),
    )
    monkeypatch.setattr(content_tree, "build_relationship_graph", lambda _items: graph)
    container = AxklibContainer(
        source_path=Path("fixture.ima"),
        kind="fat12_floppy",
        detected_format="fat12_floppy",
        objects=(program, bank_group, bank),
        recovery_quality_summary={},
    )

    tree = content_tree.build_content_tree_for_container(container)
    rendered = content_tree.render_content_tree_text(tree)

    program_node = _find_node_by_display(tree.roots, "001: GROUP")
    assert program_node is not None
    assert [child.display_name for child in program_node.children] == ["B TEKCHORD"]
    assert program_node.children[0].details == ("Rch Assign: =SMP",)
    assert program_node.children[0].quality == content_tree.DataQuality.LIKELY
    assert program_node.children[0].children == ()
    assert "B TEKCHORD [SAMPLE BANK GROUP] - Rch Assign: =SMP" in rendered
    assert "TEKCHORD C3" in rendered
    assert "TEKCHORD C3 [SAMPLE BANK] - Rch Assign" not in rendered


def test_content_tree_projects_iso_source_load_sbac_program_assignments(monkeypatch) -> None:
    prog_payload = bytearray(0x180)
    prog_payload[0x078:0x080] = b"CFGngDrk"
    program = _object("prog1", "PROG", "001", bytes(prog_payload))
    bank_group = _object("sbac1", "SBAC", "CFIII DrkTmprd")
    bank = _object("sbnk1", "SBNK", "CFIII   ff*026-S")
    graph = RelationshipGraph(
        relationships=(
            Relationship(
                key="prog-bank",
                source_key="prog1",
                target_key="sbac1",
                relationship_type="PROG_ASSIGNMENT_TO_SBAC",
                quality="Known",
                basis="assignment-kind-0x11+name",
                assignment_index=0,
                assignment_name="CFIII DrkTmprd",
                assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
                active_assignment_state=ACTIVE_ASSIGNMENT_STATE_SOURCE_LOAD_ASSIGNMENT,
                assignment_rch_assign_display="unknown",
            ),
            Relationship(
                key="group-bank",
                source_key="sbac1",
                target_key="sbnk1",
                relationship_type="SBAC_SLOT_TO_SBNK",
                quality="Known",
                basis="test",
            ),
        ),
        sbac_sbnk_rows=(),
        prog_bank_rows=(),
        prog_ignored_rows=(),
        sbnk_bitmap_rows=(),
    )
    monkeypatch.setattr(content_tree, "build_relationship_graph", lambda _items: graph)
    container = AxklibContainer(
        source_path=Path("fixture.iso"),
        kind="iso",
        detected_format="iso",
        objects=(program, bank_group, bank),
        recovery_quality_summary={},
    )

    tree = content_tree.build_content_tree_for_container(container)
    rendered = content_tree.render_content_tree_text(tree)

    program_node = _find_node_by_display(tree.roots, "001: CFGngDrk")
    assert program_node is not None
    assert [child.display_name for child in program_node.children] == ["B CFIII DrkTmprd"]
    assert program_node.children[0].details == ("Rch Assign: =SMP",)
    assert program_node.children[0].children == ()
    assert "B CFIII DrkTmprd [SAMPLE BANK GROUP] - Rch Assign: =SMP" in rendered
    assert "Rch Assign: =SMP" in rendered
    assert "CFIII   ff*026-S" in rendered


def test_content_tree_keeps_prog_sbac_rows_out_of_active_program_children(monkeypatch) -> None:
    prog_payload = bytearray(0x180)
    prog_payload[0x078:0x080] = b"T.MORIO "
    program = _object("prog1", "PROG", "001", bytes(prog_payload))
    bank_group = _object("sbac1", "SBAC", "Internal Bank Group")
    early_bank_group = _object("sbac0", "SBAC", "A Bank Group")
    bank = _object("sbnk1", "SBNK", "Bank 1")
    early_bank = _object("sbnk0", "SBNK", "A Bank 1")
    sample = _object("smpl1", "SMPL", "Waveform 1")
    alternate = _object("smpl2", "SMPL", "Waveform 2")
    graph = RelationshipGraph(
        relationships=(
            Relationship(
                key="prog-bank",
                source_key="prog1",
                target_key="sbac1",
                relationship_type="PROG_ASSIGNMENT_TO_SBAC",
                quality="Known",
                basis="test",
                assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
                active_assignment_state=ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF,
                assignment_rch_assign_display="off",
            ),
            Relationship(
                key="prog-early-bank",
                source_key="prog1",
                target_key="sbac0",
                relationship_type="PROG_ASSIGNMENT_TO_SBAC",
                quality="Known",
                basis="test",
                assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
                active_assignment_state=ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF,
                assignment_rch_assign_display="off",
            ),
            Relationship(
                key="group-bank",
                source_key="sbac1",
                target_key="sbnk1",
                relationship_type="SBAC_SLOT_TO_SBNK",
                quality="Known",
                basis="test",
            ),
            Relationship(
                key="early-group-bank",
                source_key="sbac0",
                target_key="sbnk0",
                relationship_type="SBAC_SLOT_TO_SBNK",
                quality="Known",
                basis="test",
            ),
            Relationship(
                key="bank-sample",
                source_key="sbnk1",
                target_key="smpl1",
                relationship_type="SBNK_LEFT_MEMBER_TO_SMPL",
                quality="Known",
                basis="test",
            ),
            Relationship(
                key="bank-ambiguous-sample",
                source_key="sbnk1",
                target_key="smpl1|smpl2",
                relationship_type="SBNK_RIGHT_MEMBER_TO_SMPL",
                quality="Tentative",
                basis="test ambiguous",
                ambiguity_notes="candidate split preserved",
            ),
            Relationship(
                key="bitmap-diagnostic",
                source_key="sbnk1",
                target_key="001",
                relationship_type="SBNK_PROGRAM_BITMAP_TO_PROG",
                quality="Likely",
                basis="diagnostic bitmap",
                ambiguity_notes="not a navigation edge",
            ),
        ),
        sbac_sbnk_rows=(),
        prog_bank_rows=(),
        prog_ignored_rows=(),
        sbnk_bitmap_rows=(),
    )
    monkeypatch.setattr(content_tree, "build_relationship_graph", lambda _items: graph)
    container = AxklibContainer(
        source_path=Path("fixture.ima"),
        kind="fat12_floppy",
        detected_format="fat12_floppy",
        objects=(program, bank_group, early_bank_group, bank, early_bank, sample, alternate),
        recovery_quality_summary={},
    )

    tree = content_tree.build_content_tree_for_container(container)
    rendered = content_tree.render_content_tree_text(
        tree,
        content_tree.ContentTreeRenderOptions(max_depth=5, show_quality=True, show_unresolved=True),
    )
    verbose_tree = content_tree.build_content_tree_for_container(
        container, include_default_programs=True
    )
    verbose_rendered = content_tree.render_content_tree_text(
        verbose_tree,
        content_tree.ContentTreeRenderOptions(max_depth=5, show_quality=True, show_unresolved=True),
    )
    lines = rendered.splitlines()
    verbose_lines = verbose_rendered.splitlines()
    program_line = next(line for line in lines if "001: T.MORIO" in line)
    default_slot_line = next(line for line in verbose_lines if "002: Pgm 002" in line)
    early_group_line = next(line for line in lines if "B A Bank Group" in line)
    group_line = next(line for line in lines if "B Internal Bank Group" in line)
    early_bank_line = next(line for line in lines if "-- A Bank 1" in line)
    bank_line = next(line for line in lines if "-- Bank 1" in line and "A Bank 1" not in line)
    sample_line = next(line for line in lines if "Waveform 1" in line)
    assert "002: Pgm 002" not in rendered
    assert verbose_lines.index(default_slot_line) > verbose_lines.index(
        next(line for line in verbose_lines if "001: T.MORIO" in line)
    )
    assert lines.index(program_line) < lines.index(early_group_line) < lines.index(early_bank_line)
    assert lines.index(early_bank_line) < lines.index(group_line) < lines.index(bank_line)
    assert lines.index(bank_line) < lines.index(sample_line)
    assert "[Known]" in program_line
    assert "[Known]" in group_line
    assert "Sample Bank Accessories" not in rendered
    assert "candidate split preserved" not in rendered
    assert "[Tentative]" not in rendered
    assert "diagnostic bitmap" not in rendered


def test_content_tree_hides_only_quiet_default_programs(monkeypatch) -> None:
    quiet_program = _object("prog-quiet", "PROG", "002", bytes(bytearray(0x180)))
    active_program = _object("prog-active", "PROG", "003", bytes(bytearray(0x180)))
    bank = _object("sbnk-active", "SBNK", "Active Bank")
    graph = RelationshipGraph(
        relationships=(
            Relationship(
                key="prog-active-bank",
                source_key="prog-active",
                target_key="sbnk-active",
                relationship_type="PROG_ASSIGNMENT_TO_SBNK",
                quality="Known",
                basis="test",
                assignment_index=0,
                assignment_name="Active Bank",
                assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
                active_assignment_state=ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE,
                assignment_rch_assign_display="=SMP",
            ),
        ),
        sbac_sbnk_rows=(),
        prog_bank_rows=(),
        prog_ignored_rows=(),
        sbnk_bitmap_rows=(),
    )
    monkeypatch.setattr(content_tree, "build_relationship_graph", lambda _items: graph)
    container = AxklibContainer(
        source_path=Path("fixture.ima"),
        kind="fat12_floppy",
        detected_format="fat12_floppy",
        objects=(quiet_program, active_program, bank),
        recovery_quality_summary={},
    )

    default_rendered = content_tree.render_content_tree_text(
        content_tree.build_content_tree_for_container(container),
        content_tree.ContentTreeRenderOptions(max_depth=4),
    )
    verbose_rendered = content_tree.render_content_tree_text(
        content_tree.build_content_tree_for_container(container, include_default_programs=True),
        content_tree.ContentTreeRenderOptions(max_depth=4),
    )

    assert "002: Pgm 002" not in default_rendered
    assert "003: Pgm 003" in default_rendered
    assert "Active Bank" in default_rendered
    assert "002: Pgm 002" in verbose_rendered


def test_sample_bank_category_uses_sampler_visible_bank_groups(monkeypatch) -> None:
    bank_group = _object("sbac1", "SBAC", "TS-BASS")
    bank = _object("sbnk1", "SBNK", "TS-BASS1")
    waveform = _object("smpl1", "SMPL", "BASS-SS")
    graph = RelationshipGraph(
        relationships=(
            Relationship(
                key="group-bank",
                source_key="sbac1",
                target_key="sbnk1",
                relationship_type="SBAC_SLOT_TO_SBNK",
                quality="Known",
                basis="test",
            ),
            Relationship(
                key="bank-waveform",
                source_key="sbnk1",
                target_key="smpl1",
                relationship_type="SBNK_LEFT_MEMBER_TO_SMPL",
                quality="Known",
                basis="test waveform link",
            ),
        ),
        sbac_sbnk_rows=(),
        prog_bank_rows=(),
        prog_ignored_rows=(),
        sbnk_bitmap_rows=(),
    )
    monkeypatch.setattr(content_tree, "build_relationship_graph", lambda _items: graph)
    container = AxklibContainer(
        source_path=Path("fixture.ima"),
        kind="fat12_floppy",
        detected_format="fat12_floppy",
        objects=(bank_group, bank, waveform),
        recovery_quality_summary={},
    )

    rendered = content_tree.render_content_tree_text(
        content_tree.build_content_tree_for_container(container),
        content_tree.ContentTreeRenderOptions(max_depth=4),
    )
    lines = rendered.splitlines()
    sample_banks_line = next(line for line in lines if "Sample Banks [CATEGORY] (1)" in line)
    bank_group_line = next(line for line in lines if "B TS-BASS" in line)
    bank_line = next(line for line in lines if "TS-BASS1" in line)
    waveforms_line = next(line for line in lines if "Waveforms [CATEGORY] (1)" in line)
    waveform_line = next(line for line in lines if "BASS-SS" in line)

    assert lines.index(sample_banks_line) < lines.index(bank_group_line) < lines.index(bank_line)
    assert lines.index(bank_line) < lines.index(waveforms_line) < lines.index(waveform_line)
    assert "-- B TS-BASS" in bank_group_line
    assert "-- TS-BASS1" in bank_line
    assert "-- BASS-SS" in waveform_line


def test_non_sfs_known_placements_match_info_scope_labels() -> None:
    waveform = _object("smpl1", "SMPL", "Waveform 1")
    bank = _object("sbnk1", "SBNK", "Bank 1")
    internal = _object("sbac1", "SBAC", "Internal Group")
    container = AxklibContainer(
        source_path=Path("fixture.iso"),
        kind="iso",
        detected_format="iso9660",
        objects=(waveform, bank, internal),
        recovery_quality_summary={},
    )

    placements, issues = content_tree.load_known_object_placements(container)

    assert issues == ()
    assert set(placements) == {"smpl1", "sbnk1", "sbac1"}
    assert placements["smpl1"].volume_name == "ISO objects"
    assert placements["smpl1"].category_name == "Waveforms"
    assert placements["smpl1"].entry_name == "Waveform 1"
    assert placements["sbnk1"].category_name == "Sample Banks"
    assert placements["sbac1"].category_name == "Sample Banks"
    assert placements["sbac1"].entry_name == "Internal Group"


def test_iso_volume_labels_use_decoded_metadata_and_content_fallback() -> None:
    known_payload = bytearray(0x180)
    known_payload[0x078:0x080] = b"Known   "
    fallback_payload = bytearray(0x180)
    fallback_payload[0x078:0x080] = b"ALMFngr "
    known = AxklibObject(
        image="public_fixture.iso",
        container_kind="iso",
        scope_key="fixture",
        object_key="known-prog",
        partition_index=None,
        sfs_id=None,
        fat_file="GROUP_A/V001/PROG/F001",
        payload_offset=None,
        payload_size=0,
        type="PROG",
        name="001",
        payload=bytes(known_payload),
        metadata={
            "iso_group_label": "GROUP_ALPHA",
            "iso_volume_label": "Mapped Volume",
        },
    )
    fallback = AxklibObject(
        image="public_fixture.iso",
        container_kind="iso",
        scope_key="fixture",
        object_key="fallback-prog",
        partition_index=None,
        sfs_id=None,
        fat_file="GROUP_B/V001/PROG/F001",
        payload_offset=None,
        payload_size=0,
        type="PROG",
        name="001",
        payload=bytes(fallback_payload),
        metadata={"iso_group_label": "GROUP_BETA"},
    )
    bank = AxklibObject(
        image="public_fixture.iso",
        container_kind="iso",
        scope_key="fixture",
        object_key="fallback-bank",
        partition_index=None,
        sfs_id=None,
        fat_file="GROUP_B/V001/SBAC/F001",
        payload_offset=None,
        payload_size=0,
        type="SBAC",
        name="Alem Harm",
        payload=b"",
        metadata={"iso_group_label": "GROUP_BETA"},
    )
    container = AxklibContainer(
        source_path=Path("public_fixture.iso"),
        kind="iso",
        detected_format="iso9660",
        objects=(known, fallback, bank),
        recovery_quality_summary={},
    )

    placements, issues = content_tree.load_known_object_placements(container)

    assert issues == ()
    assert placements["known-prog"].partition_name == "GROUP_ALPHA"
    assert placements["known-prog"].volume_name == "Mapped Volume"
    assert placements["known-prog"].basis == "ISO Yamaha CD-ROM menu label metadata"
    assert placements["fallback-prog"].partition_name == "GROUP_BETA"
    assert placements["fallback-prog"].volume_name == "ALMFngr"
    assert placements["fallback-bank"].volume_name == "ALMFngr"
    assert (
        placements["fallback-prog"].basis
        == "ISO directory path plus content-derived volume label fallback"
    )


def test_duplicate_iso_volume_labels_keep_raw_volumes_separate() -> None:
    first_payload = bytearray(0x180)
    first_payload[0x078:0x080] = b"ArgRotar"
    second_payload = bytearray(0x180)
    second_payload[0x078:0x080] = b"ArgRotar"
    metadata = {"iso_group_label": "ORGANS", "iso_volume_label": "Or11 Argent"}
    first = _object(
        "prog-f001",
        "PROG",
        "001",
        bytes(first_payload),
        fat_file="GROUP/F001/PROG/F001",
        metadata=metadata,
    )
    second = _object(
        "prog-f002",
        "PROG",
        "001",
        bytes(second_payload),
        fat_file="GROUP/F002/PROG/F001",
        metadata=metadata,
    )
    container = AxklibContainer(
        source_path=Path("public_fixture.iso"),
        kind="iso",
        detected_format="iso9660",
        objects=(first, second),
        recovery_quality_summary={},
    )

    tree = content_tree.build_content_tree_for_container(container)
    rendered = content_tree.render_content_tree_text(tree)

    partition = _find_node_by_display(tree.roots, "ORGANS")
    assert partition is not None
    volume_names = [node.display_name for node in partition.children]
    assert volume_names == ["Or11 Argent (F001)", "Or11 Argent (F002)"]
    assert rendered.count("001: ArgRotar [PROGRAM]") == 2
    assert "Or11 Argent [VOLUME]" not in rendered
    assert "Or11 Argent (F001) [VOLUME]" in rendered
    assert "Or11 Argent (F002) [VOLUME]" in rendered


def test_content_tree_shows_empty_sfs_volumes_from_inventory(tmp_path: Path) -> None:
    from axklib.containers import open as open_container
    from axklib.write import HdsImageBuilder

    image_path = tmp_path / "HD00_512_sparse_768m_3p_empty_volumes.hds"
    builder = HdsImageBuilder(size_bytes=768 * 1024 * 1024)
    for _index in range(3):
        partition = builder.add_partition("New Partition")
        partition.add_volume("New Volume")
    builder.write(image_path)

    container = open_container(image_path)
    tree = content_tree.build_content_tree_for_container(container, include_validation=False)
    rendered = content_tree.render_content_tree_text(tree)
    paths = content_tree.render_content_tree_paths(tree)

    assert rendered.count("New Volume [VOLUME] (0)") == 3
    assert "partition 0: New Partition [PARTITION] (1)" in rendered
    assert "partition 1: New Partition  1 [PARTITION] (1)" in rendered
    assert "partition 2: New Partition  2 [PARTITION] (1)" in rendered
    assert "partition_00_New_Partition/New Volume" in paths
    assert "partition_01_New_Partition_1/New Volume" in paths
    assert "partition_02_New_Partition_2/New Volume" in paths
