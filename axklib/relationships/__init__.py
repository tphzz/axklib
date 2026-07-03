"""Shared current-object relationship graph helpers."""

from __future__ import annotations

import json
from collections import Counter, defaultdict
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from axklib.containers import AxklibContainerLoadResult, OpenOptions, open_many
from axklib.model import AxklibObject as ObjectItem
from axklib.objects import current as objects
from axklib.parameters.current import (
    PROG_SLOT_KIND_TARGET_CATEGORY,
    SBAC_SLOT_COUNT_OFFSET,
    decode_current_sbnk_members,
    decode_sbnk_program_link_bitmaps,
    iter_prog_assignments,
    iter_sbac_slots,
    parse_program_number,
)


@dataclass(frozen=True)
class Relationship:
    """Directed object relationship with quality and quality.
    
    Use this as the canonical edge type for PROG/SBAC/SBNK/SMPL ownership, membership, and cross-check relationships."""
    key: str
    source_key: str
    target_key: str
    relationship_type: str
    quality: str
    basis: str
    raw_fields: str = ""
    ambiguity_notes: str = ""
    source_image: str = ""
    scope_key: str = ""


@dataclass(frozen=True)
class RelationshipGraph:
    """Collection of relationship edges plus detailed diagnostic rows.
    
    Use it to navigate parent/child relationships while preserving ambiguity, ignored rows, and bitmap consistency diagnostics for reports."""
    relationships: tuple[Relationship, ...]
    sbac_sbnk_rows: tuple[SbacSbnkRow, ...]
    prog_bank_rows: tuple[ProgBankRow, ...]
    prog_ignored_rows: tuple[ProgIgnoredRow, ...]
    sbnk_bitmap_rows: tuple[SbnkProgramBitmapRow, ...]

    def children(self, object_key: str) -> tuple[Relationship, ...]:
        return tuple(row for row in self.relationships if row.source_key == object_key)

    def parents(self, object_key: str) -> tuple[Relationship, ...]:
        return tuple(row for row in self.relationships if row.target_key == object_key)

    def ambiguous(self) -> tuple[Relationship, ...]:
        return tuple(row for row in self.relationships if row.quality == "Tentative")


@dataclass(frozen=True)
class RelationshipGraphLoadResult:
    """Relationship graph build result for multiple input paths.
    
    Use it when a command needs the graph and also needs to surface container load errors in a structured way."""
    graph: RelationshipGraph
    load_errors: tuple[AxklibContainerLoadResult, ...]


@dataclass(frozen=True)
class ObjectRef:
    """Normalized object identity used internally while matching relationships.

    Use it to compare candidates by source, scope, object key, physical location,
    type, and name without passing full payloads through matching code.
    """

    image: str
    container_kind: str
    scope_key: str
    object_key: str
    partition_index: int | None
    sfs_id: int | None
    fat_file: str
    payload_offset: int | None
    payload_size: int
    type: str
    name: str


@dataclass(frozen=True)
class MatchResult:
    """Result of resolving a decoded relationship target against candidate objects.

    Use it to keep the matched object, matching method, quality, basis notes,
    and all unresolved candidates together.
    """

    ref: ObjectRef | None
    method: str
    quality: str
    notes: str
    candidate_refs: list[ObjectRef]


@dataclass(frozen=True)
class SbacSbnkRow:
    """Detailed decoded SBAC slot to SBNK candidate/match row.
    
    Use this for coverage reports and audits that need to explain how a sampler-visible bank group resolves to child SBNK objects."""
    image: str
    container_kind: str
    scope_key: str
    sbac_object_key: str
    sbac_partition_index: int | None
    sbac_sfs_id: int | None
    sbac_fat_file: str
    sbac_payload_offset: int | None
    sbac_name: str
    sbac_payload_size: int
    sbac_slot_count_0x144: int
    slot_index: int
    slot_offset: int
    slot_sbnk_name: str
    slot_raw_handle_0x10: int
    match_method: str
    match_quality: str
    match_notes: str
    candidate_count: int
    candidate_object_keys: str
    candidate_fat_files: str
    candidate_names: str
    matched_sbnk_object_key: str
    matched_sbnk_partition_index: int | None
    matched_sbnk_sfs_id: int | None
    matched_sbnk_fat_file: str
    matched_sbnk_payload_offset: int | None
    matched_sbnk_name: str
    notes: str


@dataclass(frozen=True)
class ProgBankRow:
    """Detailed decoded PROG assignment row and target match.
    
    Use this to explain which sample bank, sample, or group a program slot assignment points to, including ambiguous candidates."""
    image: str
    container_kind: str
    scope_key: str
    prog_object_key: str
    prog_partition_index: int | None
    prog_sfs_id: int | None
    prog_fat_file: str
    prog_payload_offset: int | None
    prog_name: str
    prog_payload_size: int
    assignment_index: int
    assignment_offset: int
    assignment_name: str
    assignment_raw_handle_0x10: int
    assignment_kind_byte_0x14: int
    assignment_flag_byte_0x15: int
    selector_expected_category: str
    match_method: str
    match_quality: str
    match_notes: str
    candidate_count: int
    candidate_categories: str
    candidate_object_keys: str
    candidate_fat_files: str
    candidate_names: str
    matched_target_type: str
    matched_target_object_key: str
    matched_target_partition_index: int | None
    matched_target_sfs_id: int | None
    matched_target_fat_file: str
    matched_target_payload_offset: int | None
    matched_target_name: str
    matched_sbac_child_sbnk_count: int | None
    notes: str


@dataclass(frozen=True)
class ProgIgnoredRow:
    """Decoded PROG assignment row intentionally ignored by relationship matching.
    
    Use this to keep unsupported, empty, or non-bank assignment rows visible without turning them into graph edges."""
    image: str
    container_kind: str
    scope_key: str
    prog_object_key: str
    prog_partition_index: int | None
    prog_sfs_id: int | None
    prog_fat_file: str
    prog_payload_offset: int | None
    prog_name: str
    prog_payload_size: int
    assignment_index: int
    assignment_offset: int
    raw_name_guess: str
    assignment_raw_handle_0x10: int
    assignment_kind_byte_0x14: int
    assignment_flag_byte_0x15: int
    reason: str

@dataclass(frozen=True)
class SbnkProgramBitmapRow:
    """Diagnostic row comparing SBNK program-link bitmaps against decoded program assignments.
    
    Use this to identify clean, stale, ambiguous, or indirect program-link metadata without treating bitmap words as authoritative ownership."""
    image: str
    container_kind: str
    scope_key: str
    sbnk_object_key: str
    sbnk_partition_index: int | None
    sbnk_sfs_id: int | None
    sbnk_fat_file: str
    sbnk_payload_offset: int | None
    sbnk_name: str
    linked_programs_001_032_bitmap_0x0c0: int
    linked_programs_033_064_bitmap_0x0c4: int
    linked_programs_065_096_bitmap_0x0c8: int
    linked_programs_097_128_bitmap_0x0cc: int
    bitmap_programs: str
    direct_prog_assignment_programs: str
    direct_prog_assignment_details: str
    ambiguous_direct_assignment_programs: str
    ambiguous_direct_assignment_details: str
    sbac_indirect_assignment_programs: str
    bitmap_without_direct_assignment_programs: str
    direct_assignment_without_bitmap_programs: str
    mismatch_class: str
    match_status: str
    quality: str
    notes: str



def joined_programs(programs: list[int]) -> str:
    return joined([f"{program:03d}" for program in sorted(set(programs))])


def prog_row_program(row: ProgBankRow) -> int | None:
    return parse_program_number(row.prog_name)


def prog_row_detail(row: ProgBankRow) -> str:
    program = prog_row_program(row)
    program_text = f"{program:03d}" if program is not None else row.prog_name
    return f"{program_text}@slot{row.assignment_index}:kind0x{row.assignment_kind_byte_0x14:02x}:flag0x{row.assignment_flag_byte_0x15:02x}"


def joined_prog_row_details(rows: list[ProgBankRow]) -> str:
    return joined(sorted({prog_row_detail(row) for row in rows}))


def candidate_key_set(row: ProgBankRow) -> set[str]:
    if not row.candidate_object_keys:
        return set()
    return {key for key in row.candidate_object_keys.split("|") if key}


def classify_sbnk_program_bitmap_mismatch(
    *,
    bitmap_without_direct: list[int],
    direct_without_bitmap: list[int],
    direct_rows: list[ProgBankRow],
    ambiguous_rows: list[ProgBankRow],
    indirect_programs: list[int],
) -> str:
    if not bitmap_without_direct and not direct_without_bitmap:
        return "match"
    ambiguous_programs = {program for row in ambiguous_rows if (program := prog_row_program(row)) is not None}
    direct_rows_by_program: dict[int, list[ProgBankRow]] = defaultdict(list)
    for row in direct_rows:
        program = prog_row_program(row)
        if program is not None:
            direct_rows_by_program[program].append(row)
    indirect_set = set(indirect_programs)

    if bitmap_without_direct and not direct_without_bitmap:
        if set(bitmap_without_direct).issubset(ambiguous_programs):
            return "bitmap_disambiguates_ambiguous_direct_assignment"
        return "bitmap_without_decoded_direct_assignment"

    classes: set[str] = set()
    for program in direct_without_bitmap:
        rows = direct_rows_by_program.get(program, [])
        if program in indirect_set:
            classes.add("direct_assignment_also_reached_through_sbac")
        elif rows and all(row.assignment_flag_byte_0x15 != 0xFF for row in rows):
            classes.add("nondefault_flag_direct_assignment_without_bitmap")
        else:
            classes.add("known_direct_assignment_missing_bitmap")
    if bitmap_without_direct:
        classes.add("bitmap_without_decoded_direct_assignment")
    if len(classes) == 1:
        return next(iter(classes))
    return "mixed:" + "+".join(sorted(classes))


def joined(values: Sequence[object]) -> str:
    return "|".join(str(value) for value in values)


def object_ref(item: ObjectItem) -> ObjectRef:
    return ObjectRef(
        image=item.image,
        container_kind=item.container_kind,
        scope_key=item.scope_key,
        object_key=item.object_key,
        partition_index=item.partition_index,
        sfs_id=item.sfs_id,
        fat_file=item.fat_file,
        payload_offset=item.payload_offset,
        payload_size=item.payload_size,
        type=item.type,
        name=item.name,
    )


def object_ref_sort_key(ref: ObjectRef) -> tuple[str, str, str, str, str, int, str]:
    return (
        ref.image,
        ref.scope_key,
        ref.type,
        ref.name,
        ref.object_key,
        -1 if ref.payload_offset is None else ref.payload_offset,
        ref.fat_file,
    )


def refs_by_type_and_name(refs: list[ObjectRef]) -> dict[str, dict[str, list[ObjectRef]]]:
    result: dict[str, dict[str, list[ObjectRef]]] = defaultdict(lambda: defaultdict(list))
    for ref in refs:
        result[ref.type][ref.name].append(ref)
    for by_name in result.values():
        for name, refs in list(by_name.items()):
            by_name[name] = sorted(refs, key=object_ref_sort_key)
    return result


def refs_by_name(refs: list[ObjectRef]) -> dict[str, list[ObjectRef]]:
    result: dict[str, list[ObjectRef]] = defaultdict(list)
    for ref in refs:
        result[ref.name].append(ref)
    for name, refs in list(result.items()):
        result[name] = sorted(refs, key=object_ref_sort_key)
    return result


def candidate_categories(refs: list[ObjectRef]) -> str:
    return joined([ref.type for ref in refs])


def candidate_object_keys(refs: list[ObjectRef]) -> str:
    return joined([ref.object_key for ref in refs])


def candidate_files(refs: list[ObjectRef]) -> str:
    return joined([ref.fat_file for ref in refs if ref.fat_file])


def candidate_names(refs: list[ObjectRef]) -> str:
    return joined([ref.name for ref in refs])


def match_unique_sbnk_name(name: str, by_type_name: dict[str, dict[str, list[ObjectRef]]]) -> MatchResult:
    candidates = by_type_name.get("SBNK", {}).get(name, [])
    if len(candidates) == 1:
        return MatchResult(
            ref=candidates[0],
            method="active-sbac-slot-name",
            quality="Known",
            notes=(
                "Input consistency: counted SBAC slot name uniquely matches a same-scope SBNK header name. "
                "The companion 32-bit slot word is preserved as raw/opaque."
            ),
            candidate_refs=candidates,
        )
    if len(candidates) > 1:
        return MatchResult(
            ref=None,
            method="active-sbac-slot-name-ambiguous",
            quality="Tentative",
            notes="Multiple same-scope SBNK objects share the counted SBAC slot name.",
            candidate_refs=candidates,
        )
    return MatchResult(
        ref=None,
        method="active-sbac-slot-unmatched",
        quality="Unknown",
        notes="No same-scope SBNK header name matches this counted SBAC slot name.",
        candidate_refs=[],
    )


def match_prog_assignment(
    *,
    name: str,
    kind_byte: int,
    by_name: dict[str, list[ObjectRef]],
    by_type_name: dict[str, dict[str, list[ObjectRef]]],
) -> MatchResult:
    all_candidates = by_name.get(name, [])
    expected_category = PROG_SLOT_KIND_TARGET_CATEGORY.get(kind_byte, "")
    if expected_category:
        typed_candidates = by_type_name.get(expected_category, {}).get(name, [])
        if len(typed_candidates) == 1:
            quality = "Likely" if len(all_candidates) > 1 else "Known"
            return MatchResult(
                ref=typed_candidates[0],
                method=f"assignment-kind-0x{kind_byte:02x}+name",
                quality=quality,
                notes=(
                    "Input consistency: assignment name matches a same-scope object, "
                    f"and assignment kind byte 0x{kind_byte:02x} selects {expected_category} in tested current-object corpora. "
                    "Keep the selector below write-side quality until formula or validated saves support it."
                ),
                candidate_refs=typed_candidates,
            )
        if len(typed_candidates) > 1:
            return MatchResult(
                ref=None,
                method=f"assignment-kind-0x{kind_byte:02x}+name-ambiguous",
                quality="Tentative",
                notes=(
                    f"Assignment kind byte 0x{kind_byte:02x} points at {expected_category}, "
                    "but multiple same-scope objects have that name and category."
                ),
                candidate_refs=typed_candidates,
            )

    if len(all_candidates) == 1:
        return MatchResult(
            ref=all_candidates[0],
            method="assignment-name-unique",
            quality="Likely",
            notes=(
                "Input consistency: assignment name uniquely matches one same-scope object. "
                "The assignment kind byte was not independently decoded for this row."
            ),
            candidate_refs=all_candidates,
        )
    if len(all_candidates) > 1:
        return MatchResult(
            ref=None,
            method="assignment-name-ambiguous",
            quality="Tentative",
            notes="Multiple same-scope objects share this assignment name.",
            candidate_refs=all_candidates,
        )
    return MatchResult(
        ref=None,
        method="assignment-unmatched",
        quality="Unknown",
        notes="No same-scope SBAC/SBNK/SMPL object name matches this PROG assignment name.",
        candidate_refs=[],
    )




def _be32(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], "big")


def _smpl_link_id(item: ObjectItem) -> int | None:
    if len(item.payload) <= 0x7B:
        return None
    return _be32(item.payload, 0x078)


def _match_sbnk_member(
    *,
    name: str,
    link_id: int,
    by_link: dict[int, list[ObjectRef]],
    by_name: dict[str, list[ObjectRef]],
) -> MatchResult:
    link_candidates = by_link.get(link_id, [])
    exact = [ref for ref in link_candidates if ref.name == name]
    if len(exact) == 1:
        return MatchResult(
            ref=exact[0],
            method="sbnk-member-link+name",
            quality="Known",
            notes="Current SBNK member name and member link ID match exactly one same-scope SMPL object.",
            candidate_refs=exact,
        )
    if len(exact) > 1:
        return MatchResult(
            ref=None,
            method="sbnk-member-link+name-ambiguous",
            quality="Tentative",
            notes="Current SBNK member name and link ID match multiple same-scope SMPL objects.",
            candidate_refs=exact,
        )
    name_candidates = by_name.get(name, [])
    if len(name_candidates) == 1:
        return MatchResult(
            ref=name_candidates[0],
            method="sbnk-member-name-only",
            quality="Tentative",
            notes="Current SBNK member name uniquely matches a same-scope SMPL object, but the member link ID did not confirm it.",
            candidate_refs=name_candidates,
        )
    if len(link_candidates) == 1:
        return MatchResult(
            ref=link_candidates[0],
            method="sbnk-member-link-only",
            quality="Tentative",
            notes="Current SBNK member link ID uniquely matches a same-scope SMPL object, but the member name did not confirm it.",
            candidate_refs=link_candidates,
        )
    if name_candidates:
        return MatchResult(
            ref=None,
            method="sbnk-member-name-ambiguous",
            quality="Tentative",
            notes="Current SBNK member name matches multiple same-scope SMPL objects.",
            candidate_refs=name_candidates,
        )
    if link_candidates:
        return MatchResult(
            ref=None,
            method="sbnk-member-link-ambiguous",
            quality="Tentative",
            notes="Current SBNK member link ID matches multiple same-scope SMPL objects.",
            candidate_refs=link_candidates,
        )
    return MatchResult(
        ref=None,
        method="sbnk-member-unmatched",
        quality="Unknown",
        notes="Current SBNK member did not match a same-scope SMPL object by known link+name quality.",
        candidate_refs=[],
    )


def build_sbnk_smpl_relationships(scope_items: list[ObjectItem]) -> list[Relationship]:
    source_image = scope_items[0].image if scope_items else ""
    scope_key = scope_items[0].scope_key if scope_items else ""
    refs = [object_ref(item) for item in scope_items]
    refs_by_key = {ref.object_key: ref for ref in refs}
    smpl_by_name = refs_by_type_and_name(refs).get("SMPL", {})
    smpl_by_link: dict[int, list[ObjectRef]] = defaultdict(list)
    for item in scope_items:
        if item.type != "SMPL":
            continue
        link_id = _smpl_link_id(item)
        if link_id is not None and item.object_key in refs_by_key:
            smpl_by_link[link_id].append(refs_by_key[item.object_key])
    for candidates in smpl_by_link.values():
        candidates.sort(key=object_ref_sort_key)

    rows: list[Relationship] = []
    for item in scope_items:
        if item.type != "SBNK":
            continue
        try:
            members = decode_current_sbnk_members(item.payload)
        except Exception:
            continue
        member_specs = [
            ("left", "SBNK_LEFT_MEMBER_TO_SMPL", members.left.sample_name, members.left.smpl_link_id, "SBNK+left member"),
        ]
        if members.inactive_right.sample_name:
            member_specs.append(
                (
                    "right",
                    "SBNK_RIGHT_MEMBER_TO_SMPL",
                    members.inactive_right.sample_name,
                    members.inactive_right.smpl_link_id,
                    "SBNK+right member",
                )
            )
        for role, rel_type, name, link_id, raw_fields in member_specs:
            if not name:
                continue
            match = _match_sbnk_member(
                name=name,
                link_id=link_id,
                by_link=smpl_by_link,
                by_name=smpl_by_name,
            )
            target = match.ref.object_key if match.ref is not None else candidate_object_keys(match.candidate_refs)
            rows.append(
                Relationship(
                    key=_relationship_key(item.object_key, rel_type, target, match.method),
                    source_key=item.object_key,
                    target_key=target,
                    relationship_type=rel_type,
                    quality=match.quality,
                    basis=match.method,
                    raw_fields=f"{raw_fields} {role}; name={name}; link_id=0x{link_id:08x}",
                    ambiguity_notes=match.notes,
                    source_image=source_image,
                    scope_key=scope_key,
                )
            )
    return rows

def build_sbnk_program_bitmap_rows(
    items: list[ObjectItem],
    sbac_rows: list[SbacSbnkRow],
    prog_rows: list[ProgBankRow],
) -> list[SbnkProgramBitmapRow]:
    direct_rows_by_sbnk: dict[str, list[ProgBankRow]] = defaultdict(list)
    ambiguous_rows_by_sbnk: dict[str, list[ProgBankRow]] = defaultdict(list)
    indirect_by_sbnk: dict[str, list[int]] = defaultdict(list)
    sbac_children: dict[str, list[str]] = defaultdict(list)
    for sbac_row in sbac_rows:
        if sbac_row.match_quality == "Known" and sbac_row.matched_sbnk_object_key:
            sbac_children[sbac_row.sbac_object_key].append(sbac_row.matched_sbnk_object_key)

    for prog_row in prog_rows:
        program = parse_program_number(prog_row.prog_name)
        if program is None:
            continue
        if prog_row.matched_target_type == "SBNK" and prog_row.matched_target_object_key:
            direct_rows_by_sbnk[prog_row.matched_target_object_key].append(prog_row)
        elif prog_row.selector_expected_category == "SBNK" and prog_row.match_quality == "Tentative":
            for candidate_key in candidate_key_set(prog_row):
                ambiguous_rows_by_sbnk[candidate_key].append(prog_row)
        elif prog_row.matched_target_type == "SBAC" and prog_row.matched_target_object_key:
            for sbnk_key in sbac_children.get(prog_row.matched_target_object_key, []):
                indirect_by_sbnk[sbnk_key].append(program)

    rows: list[SbnkProgramBitmapRow] = []
    for item in items:
        if item.type != "SBNK":
            continue
        bitmap_programs, words = decode_sbnk_program_link_bitmaps(item.payload)
        direct_rows = direct_rows_by_sbnk.get(item.object_key, [])
        ambiguous_rows = ambiguous_rows_by_sbnk.get(item.object_key, [])
        indirect_programs = indirect_by_sbnk.get(item.object_key, [])
        direct_programs = [program for row in direct_rows if (program := prog_row_program(row)) is not None]
        ambiguous_programs = [program for row in ambiguous_rows if (program := prog_row_program(row)) is not None]
        bitmap_set = set(bitmap_programs)
        direct_set = set(direct_programs)
        bitmap_without_direct = sorted(bitmap_set - direct_set)
        direct_without_bitmap = sorted(direct_set - bitmap_set)
        mismatch_class = classify_sbnk_program_bitmap_mismatch(
            bitmap_without_direct=bitmap_without_direct,
            direct_without_bitmap=direct_without_bitmap,
            direct_rows=direct_rows,
            ambiguous_rows=ambiguous_rows,
            indirect_programs=indirect_programs,
        )
        if bitmap_without_direct or direct_without_bitmap:
            match_status = "mismatch"
            quality = "Tentative"
            notes = (
                "SBNK program-link bitmap does not match direct PROG->SBNK assignments in this scope. "
                "Preserve as a diagnostic until object identity, stale bitmap state, or another ownership path is proven."
            )
        else:
            match_status = "match"
            quality = "Known"
            notes = (
                "Validated standalone assignment rows support SBNK+0x0c0..0x0cf as four big-endian "
                "program-link bitmap words for direct PROG->SBNK/sample assignments. PROG->SBAC assignments are "
                "reported separately as indirection and are not expected to set child SBNK bits."
            )
        rows.append(
            SbnkProgramBitmapRow(
                image=item.image,
                container_kind=item.container_kind,
                scope_key=item.scope_key,
                sbnk_object_key=item.object_key,
                sbnk_partition_index=item.partition_index,
                sbnk_sfs_id=item.sfs_id,
                sbnk_fat_file=item.fat_file,
                sbnk_payload_offset=item.payload_offset,
                sbnk_name=item.name,
                linked_programs_001_032_bitmap_0x0c0=words[0],
                linked_programs_033_064_bitmap_0x0c4=words[1],
                linked_programs_065_096_bitmap_0x0c8=words[2],
                linked_programs_097_128_bitmap_0x0cc=words[3],
                bitmap_programs=joined_programs(bitmap_programs),
                direct_prog_assignment_programs=joined_programs(direct_programs),
                direct_prog_assignment_details=joined_prog_row_details(direct_rows),
                ambiguous_direct_assignment_programs=joined_programs(ambiguous_programs),
                ambiguous_direct_assignment_details=joined_prog_row_details(ambiguous_rows),
                sbac_indirect_assignment_programs=joined_programs(indirect_programs),
                bitmap_without_direct_assignment_programs=joined_programs(bitmap_without_direct),
                direct_assignment_without_bitmap_programs=joined_programs(direct_without_bitmap),
                mismatch_class=mismatch_class,
                match_status=match_status,
                quality=quality,
                notes=notes,
            )
        )
    return rows

def scan_scope(items: list[ObjectItem]) -> tuple[list[SbacSbnkRow], list[ProgBankRow], list[ProgIgnoredRow], list[SbnkProgramBitmapRow]]:
    refs = [object_ref(item) for item in items]
    by_type_name = refs_by_type_and_name(refs)
    by_name = refs_by_name([ref for ref in refs if ref.type in {"SMPL", "SBNK", "SBAC"}])
    sbac_rows: list[SbacSbnkRow] = []
    sbac_child_counts: dict[str, int] = {}

    for item in items:
        payload = item.payload
        if item.type != "SBAC":
            continue
        slot_count, max_slots, slots = iter_sbac_slots(payload)
        if not slots and len(payload) <= SBAC_SLOT_COUNT_OFFSET:
            continue
        active_matches = 0
        for slot in slots:
            if not slot.name:
                continue
            match = match_unique_sbnk_name(slot.name, by_type_name)
            if match.ref is not None:
                active_matches += 1
            notes: list[str] = []
            if slot_count > max_slots:
                notes.append("slot_count_exceeds_payload_capacity")
            if match.quality != "Known":
                notes.append(f"match_method={match.method}")
            sbac_rows.append(
                SbacSbnkRow(
                    image=item.image,
                    container_kind=item.container_kind,
                    scope_key=item.scope_key,
                    sbac_object_key=item.object_key,
                    sbac_partition_index=item.partition_index,
                    sbac_sfs_id=item.sfs_id,
                    sbac_fat_file=item.fat_file,
                    sbac_payload_offset=item.payload_offset,
                    sbac_name=item.name,
                    sbac_payload_size=item.payload_size,
                    sbac_slot_count_0x144=slot_count,
                    slot_index=slot.index,
                    slot_offset=slot.offset,
                    slot_sbnk_name=slot.name,
                    slot_raw_handle_0x10=slot.raw_handle_0x10,
                    match_method=match.method,
                    match_quality=match.quality,
                    match_notes=match.notes,
                    candidate_count=len(match.candidate_refs),
                    candidate_object_keys=candidate_object_keys(match.candidate_refs),
                    candidate_fat_files=candidate_files(match.candidate_refs),
                    candidate_names=candidate_names(match.candidate_refs),
                    matched_sbnk_object_key=match.ref.object_key if match.ref else "",
                    matched_sbnk_partition_index=match.ref.partition_index if match.ref else None,
                    matched_sbnk_sfs_id=match.ref.sfs_id if match.ref else None,
                    matched_sbnk_fat_file=match.ref.fat_file if match.ref else "",
                    matched_sbnk_payload_offset=match.ref.payload_offset if match.ref else None,
                    matched_sbnk_name=match.ref.name if match.ref else "",
                    notes=";".join(notes),
                )
            )
        sbac_child_counts[item.object_key] = active_matches

    prog_rows: list[ProgBankRow] = []
    ignored_rows: list[ProgIgnoredRow] = []
    for item in items:
        payload = item.payload
        if item.type != "PROG":
            continue
        for assignment in iter_prog_assignments(payload):
            index = assignment.index
            offset = assignment.offset
            assignment_name = objects.clean_ascii(assignment.raw_name.encode("ascii", errors="replace"))
            if not assignment_name:
                continue
            raw_handle = assignment.raw_handle_0x10
            kind_byte = assignment.kind_byte_0x14
            flag_byte = assignment.flag_byte_0x15
            if kind_byte not in PROG_SLOT_KIND_TARGET_CATEGORY and assignment_name not in by_name:
                ignored_rows.append(
                    ProgIgnoredRow(
                        image=item.image,
                        container_kind=item.container_kind,
                        scope_key=item.scope_key,
                        prog_object_key=item.object_key,
                        prog_partition_index=item.partition_index,
                        prog_sfs_id=item.sfs_id,
                        prog_fat_file=item.fat_file,
                        prog_payload_offset=item.payload_offset,
                        prog_name=item.name,
                        prog_payload_size=item.payload_size,
                        assignment_index=index,
                        assignment_offset=offset,
                        raw_name_guess=assignment_name,
                        assignment_raw_handle_0x10=raw_handle,
                        assignment_kind_byte_0x14=kind_byte,
                        assignment_flag_byte_0x15=flag_byte,
                        reason="ignored-reserved-or-tail-slot-no-known-kind-and-no-name-match",
                    )
                )
                continue
            match = match_prog_assignment(
                name=assignment_name,
                kind_byte=kind_byte,
                by_name=by_name,
                by_type_name=by_type_name,
            )
            prog_notes: list[str] = []
            if match.quality not in {"Known", "Likely"}:
                prog_notes.append(f"match_method={match.method}")
            if kind_byte not in PROG_SLOT_KIND_TARGET_CATEGORY:
                prog_notes.append("unmapped_assignment_kind_byte")
            child_count = (
                sbac_child_counts.get(match.ref.object_key)
                if match.ref is not None and match.ref.type == "SBAC"
                else None
            )
            prog_rows.append(
                ProgBankRow(
                    image=item.image,
                    container_kind=item.container_kind,
                    scope_key=item.scope_key,
                    prog_object_key=item.object_key,
                    prog_partition_index=item.partition_index,
                    prog_sfs_id=item.sfs_id,
                    prog_fat_file=item.fat_file,
                    prog_payload_offset=item.payload_offset,
                    prog_name=item.name,
                    prog_payload_size=item.payload_size,
                    assignment_index=index,
                    assignment_offset=offset,
                    assignment_name=assignment_name,
                    assignment_raw_handle_0x10=raw_handle,
                    assignment_kind_byte_0x14=kind_byte,
                    assignment_flag_byte_0x15=flag_byte,
                    selector_expected_category=PROG_SLOT_KIND_TARGET_CATEGORY.get(kind_byte, ""),
                    match_method=match.method,
                    match_quality=match.quality,
                    match_notes=match.notes,
                    candidate_count=len(match.candidate_refs),
                    candidate_categories=candidate_categories(match.candidate_refs),
                    candidate_object_keys=candidate_object_keys(match.candidate_refs),
                    candidate_fat_files=candidate_files(match.candidate_refs),
                    candidate_names=candidate_names(match.candidate_refs),
                    matched_target_type=match.ref.type if match.ref else "",
                    matched_target_object_key=match.ref.object_key if match.ref else "",
                    matched_target_partition_index=match.ref.partition_index if match.ref else None,
                    matched_target_sfs_id=match.ref.sfs_id if match.ref else None,
                    matched_target_fat_file=match.ref.fat_file if match.ref else "",
                    matched_target_payload_offset=match.ref.payload_offset if match.ref else None,
                    matched_target_name=match.ref.name if match.ref else "",
                    matched_sbac_child_sbnk_count=child_count,
                    notes=";".join(prog_notes),
                )
            )
    sbnk_bitmap_rows = build_sbnk_program_bitmap_rows(items, sbac_rows, prog_rows)
    return sbac_rows, prog_rows, ignored_rows, sbnk_bitmap_rows


def scan_images(
    paths: list[Path], container: str
) -> tuple[list[SbacSbnkRow], list[ProgBankRow], list[ProgIgnoredRow], list[SbnkProgramBitmapRow]]:
    result = build_relationship_graph_for_path_results(paths, container=container)
    return (
        list(result.graph.sbac_sbnk_rows),
        list(result.graph.prog_bank_rows),
        list(result.graph.prog_ignored_rows),
        list(result.graph.sbnk_bitmap_rows),
    )

def build_summary(
    sbac_rows: list[SbacSbnkRow],
    prog_rows: list[ProgBankRow],
    ignored_rows: list[ProgIgnoredRow],
    sbnk_bitmap_rows: list[SbnkProgramBitmapRow],
) -> list[dict[str, object]]:
    scope_keys = sorted(
        {row.scope_key for row in sbac_rows}
        | {row.scope_key for row in prog_rows}
        | {row.scope_key for row in ignored_rows}
        | {row.scope_key for row in sbnk_bitmap_rows}
    )
    buckets = ["ALL", *scope_keys]
    summary: list[dict[str, object]] = []
    for scope in buckets:
        sbac_items = sbac_rows if scope == "ALL" else [row for row in sbac_rows if row.scope_key == scope]
        prog_items = prog_rows if scope == "ALL" else [row for row in prog_rows if row.scope_key == scope]
        ignored_items = ignored_rows if scope == "ALL" else [row for row in ignored_rows if row.scope_key == scope]
        bitmap_items = sbnk_bitmap_rows if scope == "ALL" else [row for row in sbnk_bitmap_rows if row.scope_key == scope]
        prog_methods = Counter(row.match_method for row in prog_items)
        prog_kinds = Counter(f"0x{row.assignment_kind_byte_0x14:02x}" for row in prog_items)
        bitmap_mismatch_classes = Counter(row.mismatch_class for row in bitmap_items if row.match_status == "mismatch")
        summary.append(
            {
                "scope_key": scope,
                "container_kinds": joined(
                    sorted(
                        {row.container_kind for row in sbac_items}
                        | {row.container_kind for row in prog_items}
                        | {row.container_kind for row in ignored_items}
                        | {row.container_kind for row in bitmap_items}
                    )
                ),
                "sbac_slot_row_count": len(sbac_items),
                "sbac_known_sbnk_name_count": sum(1 for row in sbac_items if row.match_quality == "Known"),
                "sbac_unknown_count": sum(1 for row in sbac_items if row.match_quality != "Known"),
                "prog_assignment_row_count": len(prog_items),
                "prog_ignored_reserved_or_tail_row_count": len(ignored_items),
                "prog_known_count": sum(1 for row in prog_items if row.match_quality == "Known"),
                "prog_likely_count": sum(1 for row in prog_items if row.match_quality == "Likely"),
                "prog_tentative_or_unknown_count": sum(1 for row in prog_items if row.match_quality not in {"Known", "Likely"}),
                "prog_sbac_target_count": sum(1 for row in prog_items if row.matched_target_type == "SBAC"),
                "prog_sbnk_target_count": sum(1 for row in prog_items if row.matched_target_type == "SBNK"),
                "prog_smpl_target_count": sum(1 for row in prog_items if row.matched_target_type == "SMPL"),
                "sbnk_program_bitmap_row_count": len(bitmap_items),
                "sbnk_program_bitmap_match_count": sum(1 for row in bitmap_items if row.match_status == "match"),
                "sbnk_program_bitmap_mismatch_count": sum(1 for row in bitmap_items if row.match_status == "mismatch"),
                "sbnk_program_bitmap_with_direct_assignment_count": sum(1 for row in bitmap_items if row.direct_prog_assignment_programs),
                "sbnk_program_bitmap_with_sbac_indirect_assignment_count": sum(1 for row in bitmap_items if row.sbac_indirect_assignment_programs),
                "sbnk_program_bitmap_mismatch_class_counts": json.dumps(dict(sorted(bitmap_mismatch_classes.items()))),
                "prog_assignment_method_counts": json.dumps(dict(sorted(prog_methods.items()))),
                "prog_assignment_kind_byte_counts": json.dumps(dict(sorted(prog_kinds.items()))),
            }
        )
    return summary





def _relationship_sort_key(row: Relationship) -> tuple[str, str, str, str, str]:
    return (row.source_image, row.scope_key, row.source_key, row.relationship_type, row.target_key)


def _relationship_key(source_key: str, relationship_type: str, target_key: str, method: str) -> str:
    return f"{source_key}|{relationship_type}|{target_key}|{method}"


def build_relationship_graph(items: list[ObjectItem]) -> RelationshipGraph:
    by_scope: dict[str, list[ObjectItem]] = defaultdict(list)
    for item in items:
        by_scope[item.scope_key].append(item)

    all_sbac_rows: list[SbacSbnkRow] = []
    all_prog_rows: list[ProgBankRow] = []
    all_ignored_rows: list[ProgIgnoredRow] = []
    all_sbnk_bitmap_rows: list[SbnkProgramBitmapRow] = []
    relationships: list[Relationship] = []
    for scope_items in by_scope.values():
        sbac_rows, prog_rows, ignored_rows, sbnk_bitmap_rows = scan_scope(scope_items)
        all_sbac_rows.extend(sbac_rows)
        all_prog_rows.extend(prog_rows)
        all_ignored_rows.extend(ignored_rows)
        all_sbnk_bitmap_rows.extend(sbnk_bitmap_rows)
        relationships.extend(build_sbnk_smpl_relationships(scope_items))

        for sbac_row in sbac_rows:
            target = sbac_row.matched_sbnk_object_key or sbac_row.candidate_object_keys
            relationships.append(
                Relationship(
                    key=_relationship_key(sbac_row.sbac_object_key, "SBAC_SLOT_TO_SBNK", target, sbac_row.match_method),
                    source_key=sbac_row.sbac_object_key,
                    target_key=target,
                    relationship_type="SBAC_SLOT_TO_SBNK",
                    quality=sbac_row.match_quality,
                    basis=sbac_row.match_method,
                    raw_fields=f"SBAC slot {sbac_row.slot_index} at 0x{sbac_row.slot_offset:03x}",
                    ambiguity_notes=sbac_row.match_notes,
                    source_image=sbac_row.image,
                    scope_key=sbac_row.scope_key,
                )
            )
        for prog_row in prog_rows:
            rel_type = (
                "PROG_ASSIGNMENT_TO_SBAC"
                if prog_row.matched_target_type == "SBAC" or prog_row.selector_expected_category == "SBAC"
                else "PROG_ASSIGNMENT_TO_SBNK"
                if prog_row.matched_target_type == "SBNK" or prog_row.selector_expected_category == "SBNK"
                else "PROG_ASSIGNMENT_TO_OBJECT"
            )
            target = prog_row.matched_target_object_key or prog_row.candidate_object_keys
            relationships.append(
                Relationship(
                    key=_relationship_key(prog_row.prog_object_key, rel_type, target, prog_row.match_method),
                    source_key=prog_row.prog_object_key,
                    target_key=target,
                    relationship_type=rel_type,
                    quality=prog_row.match_quality,
                    basis=prog_row.match_method,
                    raw_fields=f"PROG assignment {prog_row.assignment_index} at 0x{prog_row.assignment_offset:03x}",
                    ambiguity_notes=prog_row.match_notes,
                    source_image=prog_row.image,
                    scope_key=prog_row.scope_key,
                )
            )
        for bitmap_row in sbnk_bitmap_rows:
            if bitmap_row.bitmap_programs:
                relationships.append(
                    Relationship(
                        key=_relationship_key(bitmap_row.sbnk_object_key, "SBNK_PROGRAM_BITMAP_TO_PROG", bitmap_row.bitmap_programs, "program-link-bitmap"),
                        source_key=bitmap_row.sbnk_object_key,
                        target_key=bitmap_row.bitmap_programs,
                        relationship_type="SBNK_PROGRAM_BITMAP_TO_PROG",
                        quality=bitmap_row.quality,
                        basis="program-link-bitmap",
                        raw_fields="SBNK+0x0c0..0x0cf",
                        ambiguity_notes=bitmap_row.notes,
                        source_image=bitmap_row.image,
                        scope_key=bitmap_row.scope_key,
                    )
                )
    return RelationshipGraph(
        relationships=tuple(sorted(relationships, key=_relationship_sort_key)),
        sbac_sbnk_rows=tuple(sorted(all_sbac_rows, key=lambda row: (row.image, row.scope_key, row.sbac_object_key, row.slot_index, row.candidate_object_keys))),
        prog_bank_rows=tuple(sorted(all_prog_rows, key=lambda row: (row.image, row.scope_key, row.prog_object_key, row.assignment_index, row.candidate_object_keys))),
        prog_ignored_rows=tuple(sorted(all_ignored_rows, key=lambda row: (row.image, row.scope_key, row.prog_object_key, row.assignment_index))),
        sbnk_bitmap_rows=tuple(sorted(all_sbnk_bitmap_rows, key=lambda row: (row.image, row.scope_key, row.sbnk_object_key))),
    )


def build_relationship_graph_for_loaded_results(
    results: Sequence[AxklibContainerLoadResult],
) -> RelationshipGraphLoadResult:
    items: list[ObjectItem] = []
    load_errors: list[AxklibContainerLoadResult] = []
    for result in results:
        if result.container is None:
            load_errors.append(result)
            continue
        items.extend(result.container.objects)
    return RelationshipGraphLoadResult(
        graph=build_relationship_graph(items),
        load_errors=tuple(load_errors),
    )

def build_relationship_graph_for_path_results(
    paths: list[Path],
    *,
    container: str = "auto",
    options: OpenOptions | None = None,
) -> RelationshipGraphLoadResult:
    opts = options or OpenOptions(include_payloads=True)
    if container != "auto":
        # The structured loader auto-detects normal user inputs. Keep the argument
        # for legacy callers, but avoid reintroducing direct load_objects paths.
        opts = OpenOptions(
            max_partitions=opts.max_partitions,
            include_payloads=opts.include_payloads,
            strict=opts.strict,
        )
    results = open_many(paths, options=opts)
    return build_relationship_graph_for_loaded_results(results)


def build_relationship_graph_for_paths(paths: list[Path], container: str = "auto") -> RelationshipGraph:
    return build_relationship_graph_for_path_results(paths, container=container).graph
