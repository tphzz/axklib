"""Coverage summaries derived from axklib relationship graphs."""

from __future__ import annotations

from collections import Counter
from dataclasses import dataclass

from axklib.relationships import RelationshipGraph


@dataclass(frozen=True)
class CoverageSummary:
    relationship_count: int
    known_relationship_count: int
    likely_relationship_count: int
    tentative_relationship_count: int
    unknown_relationship_count: int
    ambiguous_relationship_count: int
    sbac_sbnk_row_count: int
    prog_assignment_row_count: int
    prog_ignored_row_count: int
    sbnk_bitmap_row_count: int
    relationship_type_counts: str
    quality_counts: str


def _joined_counts(counter: Counter[str]) -> str:
    return ";".join(f"{key}:{value}" for key, value in sorted(counter.items()))


def summarize_relationship_coverage(graph: RelationshipGraph) -> CoverageSummary:
    quality_counts: Counter[str] = Counter(row.quality for row in graph.relationships)
    type_counts: Counter[str] = Counter(row.relationship_type for row in graph.relationships)
    return CoverageSummary(
        relationship_count=len(graph.relationships),
        known_relationship_count=quality_counts.get("Known", 0),
        likely_relationship_count=quality_counts.get("Likely", 0),
        tentative_relationship_count=quality_counts.get("Tentative", 0),
        unknown_relationship_count=quality_counts.get("Unknown", 0),
        ambiguous_relationship_count=len(graph.ambiguous()),
        sbac_sbnk_row_count=len(graph.sbac_sbnk_rows),
        prog_assignment_row_count=len(graph.prog_bank_rows),
        prog_ignored_row_count=len(graph.prog_ignored_rows),
        sbnk_bitmap_row_count=len(graph.sbnk_bitmap_rows),
        relationship_type_counts=_joined_counts(type_counts),
        quality_counts=_joined_counts(quality_counts),
    )
