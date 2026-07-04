from __future__ import annotations

from pathlib import Path

from axklib import content_tree
from axklib.containers import AxklibContainer
from axklib.model import AxklibObject
from axklib.relationships import Relationship, RelationshipGraph


def _object(object_key: str, object_type: str, name: str, payload: bytes = b"") -> AxklibObject:
    return AxklibObject(
        image="fixture.ima",
        container_kind="fat12_floppy",
        scope_key="fixture",
        object_key=object_key,
        partition_index=None,
        sfs_id=None,
        fat_file="",
        payload_offset=None,
        payload_size=0,
        type=object_type,
        name=name,
        payload=payload,
    )


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
            ),
            Relationship(
                key="prog-early-bank",
                source_key="prog1",
                target_key="sbac0",
                relationship_type="PROG_ASSIGNMENT_TO_SBAC",
                quality="Known",
                basis="test",
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
        content_tree.ContentTreeRenderOptions(
            max_depth=5, show_quality=True, show_unresolved=True
        ),
    )
    lines = rendered.splitlines()
    program_line = next(line for line in lines if "001: T.MORIO" in line)
    default_slot_line = next(line for line in lines if "002: Pgm 002" in line)
    early_group_line = next(line for line in lines if "B A Bank Group" in line)
    group_line = next(line for line in lines if "B Internal Bank Group" in line)
    early_bank_line = next(
        line for line in lines if "A Bank 1" in line and line.startswith("        ")
    )
    bank_line = next(
        line
        for line in lines
        if "Bank 1" in line and line.startswith("        ") and "A Bank 1" not in line
    )
    sample_line = next(line for line in lines if "Waveform 1" in line)
    assert lines.index(default_slot_line) == lines.index(program_line) + 1
    assert lines.index(default_slot_line) < lines.index(early_group_line) < lines.index(early_bank_line)
    assert lines.index(early_bank_line) < lines.index(group_line) < lines.index(bank_line)
    assert lines.index(bank_line) < lines.index(sample_line)
    assert "[Known]" in program_line
    assert "[Known]" in group_line
    assert "Sample Bank Accessories" not in rendered
    assert "candidate split preserved" not in rendered
    assert "[Tentative]" not in rendered
    assert "diagnostic bitmap" not in rendered


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
    sample_banks_line = next(line for line in lines if "Sample Banks (1)" in line)
    bank_group_line = next(line for line in lines if "B TS-BASS" in line)
    bank_line = next(line for line in lines if "TS-BASS1" in line)
    waveforms_line = next(line for line in lines if "Waveforms (1)" in line)
    waveform_line = next(line for line in lines if "BASS-SS" in line)

    assert lines.index(sample_banks_line) < lines.index(bank_group_line) < lines.index(bank_line)
    assert lines.index(bank_line) < lines.index(waveforms_line) < lines.index(waveform_line)
    assert bank_group_line.startswith("      B TS-BASS")
    assert bank_line.startswith("        TS-BASS1")
    assert waveform_line.startswith("      BASS-SS")

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





def test_iso_volume_labels_can_use_caller_supplied_map_and_content_fallback() -> None:
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
    )
    container = AxklibContainer(
        source_path=Path("public_fixture.iso"),
        kind="iso",
        detected_format="iso9660",
        objects=(known, fallback, bank),
        recovery_quality_summary={},
    )

    label_map = content_tree.ContentLabelMap(
        iso_group_labels={
            ("public_fixture", "GROUP_A"): "GROUP_ALPHA",
            ("public_fixture", "GROUP_B"): "GROUP_BETA",
        },
        iso_volume_labels={("public_fixture", "GROUP_A", "V001"): "Mapped Volume"},
    )
    placements, issues = content_tree.load_known_object_placements(container, label_map)

    assert issues == ()
    assert placements["known-prog"].partition_name == "GROUP_ALPHA"
    assert placements["known-prog"].volume_name == "Mapped Volume"
    assert placements["known-prog"].basis == "ISO directory path plus caller-supplied content label"
    assert placements["fallback-prog"].partition_name == "GROUP_BETA"
    assert placements["fallback-prog"].volume_name == "ALMFngr"
    assert placements["fallback-bank"].volume_name == "ALMFngr"
    assert placements["fallback-prog"].basis == "ISO directory path plus content-derived volume label fallback"

