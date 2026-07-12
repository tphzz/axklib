from __future__ import annotations

from pathlib import Path

from oracle.python_semantic_summary import semantic_summary

ROOT = Path(__file__).resolve().parents[3]


def test_semantic_summary_covers_relationship_tree_orphan_and_validation_contracts() -> None:
    image = (
        ROOT
        / "tests/fixtures/images/sampler-authored/HD00_512_single_sbnk_authored.hds"
    )
    value = semantic_summary(image)
    assert value["relationship_counts"] == [
        ["SBAC_SLOT_TO_SBNK", "Known", "active-sbac-slot-name", 1],
        ["SBNK_LEFT_MEMBER_TO_SMPL", "Known", "sbnk-member-link+name", 8],
    ]
    assert value["bitmap_counts"] == [["match", "match", 8]]
    assert value["waveform_status_counts"] == [["referenced", 8]]
    assert value["validation_failed"] is False
    assert value["validation_counts"] == []
    assert ["partition 0: New Partition/New Volume/Sample Banks/B New SmpBank", "sample_bank", "SBAC", 1] in value["content_nodes"]
