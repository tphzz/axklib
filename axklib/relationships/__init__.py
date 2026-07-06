"""Shared current-object relationship graph helpers."""

from __future__ import annotations

import csv
import json
import tempfile
from collections import Counter, defaultdict
from collections.abc import Sequence
from dataclasses import dataclass, replace
from pathlib import Path

from axklib.containers import (
    AxklibContainer,
    AxklibContainerLoadResult,
    OpenOptions,
    open_many,
    sfs_inventory,
)
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

ASSIGNMENT_ROW_STATE_DECODED = "decoded-row"
ACTIVE_ASSIGNMENT_STATE_UNKNOWN = "unknown"
ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE = "confirmed-active"
ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF = "confirmed-visible-off"
ACTIVE_ASSIGNMENT_STATE_CONFIRMED_DUPLICATE_NOT_ACTIVE = "confirmed-duplicate-not-active"
ACTIVE_ASSIGNMENT_STATE_SOURCE_LOAD_ASSIGNMENT = "source-load-assignment"
ACTIVE_ASSIGNMENT_STATES = (
    ACTIVE_ASSIGNMENT_STATE_UNKNOWN,
    ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE,
    ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF,
    ACTIVE_ASSIGNMENT_STATE_CONFIRMED_DUPLICATE_NOT_ACTIVE,
    ACTIVE_ASSIGNMENT_STATE_SOURCE_LOAD_ASSIGNMENT,
)
RELATIONSHIP_DIAGNOSTIC_VISIBLE_OFF_ASSIGNMENT = "visible-off-assignment"
RELATIONSHIP_DIAGNOSTIC_PROGRAM_LINK_BITMAP = "program-link-bitmap"
RELATIONSHIP_DIAGNOSTIC_SBNK_MEMBER_LINK = "sbnk-member-link"
RELATIONSHIP_DIAGNOSTIC_ACTIVE_ASSIGNMENT_MISSING_TARGET = "active-assignment-missing-target"
RELATIONSHIP_DIAGNOSTIC_AMBIGUOUS_TARGET = "ambiguous-target"
RELATIONSHIP_DIAGNOSTIC_MISSING_TARGET = "missing-target"
SBNK_PROGRAM_LINK_BITMAP_DIAGNOSTIC_BASIS_PREFIX = "sbnk-program-link-bitmap-"
RCH_ASSIGN_DISPLAY_UNKNOWN = "unknown"
RCH_ASSIGN_DISPLAY_OFF = "off"
RCH_ASSIGN_DISPLAY_SAMPLE_FOLLOW = "=SMP"
RCH_ASSIGN_DISPLAY_BASIC_RECEIVE_CHANNEL = "BasicRch"


def classify_rch_assign_display(
    *,
    assignment_row_state: str,
    midi_receive_channel_assign_byte_0x15: int | None = None,
    rch_assign_gate_byte_0x28: int | None = None,
) -> str:
    """Return the visible Rch Assign family for a decoded Program assignment row."""

    if assignment_row_state != ASSIGNMENT_ROW_STATE_DECODED:
        return RCH_ASSIGN_DISPLAY_UNKNOWN
    if rch_assign_gate_byte_0x28 == 0x00:
        return RCH_ASSIGN_DISPLAY_OFF
    if rch_assign_gate_byte_0x28 != 0xFF:
        return RCH_ASSIGN_DISPLAY_UNKNOWN
    if midi_receive_channel_assign_byte_0x15 == 0xFF:
        return RCH_ASSIGN_DISPLAY_SAMPLE_FOLLOW
    if midi_receive_channel_assign_byte_0x15 is None:
        return RCH_ASSIGN_DISPLAY_UNKNOWN
    if 0 <= midi_receive_channel_assign_byte_0x15 <= 15:
        return f"{midi_receive_channel_assign_byte_0x15 + 1:02d}"
    if midi_receive_channel_assign_byte_0x15 == 16:
        return RCH_ASSIGN_DISPLAY_BASIC_RECEIVE_CHANNEL
    if 17 <= midi_receive_channel_assign_byte_0x15 <= 32:
        return f"B{midi_receive_channel_assign_byte_0x15 - 16:02d}"
    return RCH_ASSIGN_DISPLAY_UNKNOWN


def classify_active_assignment_state(
    *, assignment_row_state: str, rch_assign_gate_byte_0x28: int | None = None
) -> str:
    """Return active Program assignment state for a decoded assignment row."""

    if assignment_row_state != ASSIGNMENT_ROW_STATE_DECODED:
        return ACTIVE_ASSIGNMENT_STATE_UNKNOWN
    if rch_assign_gate_byte_0x28 == 0xFF:
        return ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE
    if rch_assign_gate_byte_0x28 == 0x00:
        return ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF
    return ACTIVE_ASSIGNMENT_STATE_UNKNOWN


def classify_source_load_assignment_state(
    *,
    container_kind: str,
    matched_target_object_key: str,
    active_assignment_state: str,
    assignment_output1_byte_0x1d: int | None,
    assignment_rch_assign_gate_byte_0x28: int | None,
) -> str:
    """Separate ISO source links from sampler-authored active/off state."""

    if (
        container_kind == "iso"
        and matched_target_object_key
        and active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF
        and assignment_output1_byte_0x1d == 0x00
        and assignment_rch_assign_gate_byte_0x28 == 0x00
    ):
        return ACTIVE_ASSIGNMENT_STATE_SOURCE_LOAD_ASSIGNMENT
    return active_assignment_state


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
    assignment_index: int | None = None
    assignment_name: str = ""
    assignment_row_state: str = ""
    active_assignment_state: str = ""
    assignment_rch_assign_display: str = ""
    diagnostic_category: str = ""


def relationship_diagnostic_category(row: Relationship) -> str:
    """Return the high-level diagnostic bucket for report consumers."""

    if row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF:
        return RELATIONSHIP_DIAGNOSTIC_VISIBLE_OFF_ASSIGNMENT
    if row.basis.startswith(SBNK_PROGRAM_LINK_BITMAP_DIAGNOSTIC_BASIS_PREFIX):
        return RELATIONSHIP_DIAGNOSTIC_PROGRAM_LINK_BITMAP
    if row.basis.startswith("sbnk-member-link-id-only"):
        return RELATIONSHIP_DIAGNOSTIC_SBNK_MEMBER_LINK
    if row.basis == "assignment-active-missing-local-target":
        return RELATIONSHIP_DIAGNOSTIC_ACTIVE_ASSIGNMENT_MISSING_TARGET
    if row.quality == "Tentative":
        return RELATIONSHIP_DIAGNOSTIC_AMBIGUOUS_TARGET
    if row.quality == "Unknown":
        return RELATIONSHIP_DIAGNOSTIC_MISSING_TARGET
    return ""


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
    iso_extent_sector: int | None = None
    iso_data_offset: int | None = None
    iso_file_size: int | None = None
    iso_recovery_quality: str = ""
    fat_directory_offset: int | None = None
    fat_first_cluster: int | None = None
    fat_cluster_count: int | None = None
    fat_file_size: int | None = None
    fat_object_offset: int | None = None
    fat_stored_payload_offset: int | None = None
    placement_partition_index: int | None = None
    placement_volume_name: str = ""
    placement_category_name: str = ""
    placement_entry_name: str = ""
    placement_quality: str = ""


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
    sbac_iso_extent_sector: int | None = None
    sbac_iso_data_offset: int | None = None
    sbac_iso_file_size: int | None = None
    sbac_iso_recovery_quality: str = ""
    sbac_fat_directory_offset: int | None = None
    sbac_fat_first_cluster: int | None = None
    sbac_fat_cluster_count: int | None = None
    sbac_fat_file_size: int | None = None
    sbac_fat_object_offset: int | None = None
    sbac_fat_stored_payload_offset: int | None = None
    matched_sbnk_iso_extent_sector: int | None = None
    matched_sbnk_iso_data_offset: int | None = None
    matched_sbnk_iso_file_size: int | None = None
    matched_sbnk_iso_recovery_quality: str = ""
    matched_sbnk_fat_directory_offset: int | None = None
    matched_sbnk_fat_first_cluster: int | None = None
    matched_sbnk_fat_cluster_count: int | None = None
    matched_sbnk_fat_file_size: int | None = None
    matched_sbnk_fat_object_offset: int | None = None
    matched_sbnk_fat_stored_payload_offset: int | None = None


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
    assignment_output1_byte_0x1d: int | None
    assignment_rch_assign_gate_byte_0x28: int | None
    assignment_rch_assign_display: str
    selector_expected_category: str
    assignment_row_state: str
    active_assignment_state: str
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
    prog_iso_extent_sector: int | None = None
    prog_iso_data_offset: int | None = None
    prog_iso_file_size: int | None = None
    prog_iso_recovery_quality: str = ""
    prog_fat_directory_offset: int | None = None
    prog_fat_first_cluster: int | None = None
    prog_fat_cluster_count: int | None = None
    prog_fat_file_size: int | None = None
    prog_fat_object_offset: int | None = None
    prog_fat_stored_payload_offset: int | None = None
    matched_target_iso_extent_sector: int | None = None
    matched_target_iso_data_offset: int | None = None
    matched_target_iso_file_size: int | None = None
    matched_target_iso_recovery_quality: str = ""
    matched_target_fat_directory_offset: int | None = None
    matched_target_fat_first_cluster: int | None = None
    matched_target_fat_cluster_count: int | None = None
    matched_target_fat_file_size: int | None = None
    matched_target_fat_object_offset: int | None = None
    matched_target_fat_stored_payload_offset: int | None = None


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
    ambiguous_programs = {
        program for row in ambiguous_rows if (program := prog_row_program(row)) is not None
    }
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


def sbnk_program_bitmap_relationship_basis(row: SbnkProgramBitmapRow) -> str:
    if row.quality != "Tentative":
        return "program-link-bitmap"
    slug = row.mismatch_class.replace(":", "-").replace("_", "-").replace("+", "-")
    return f"{SBNK_PROGRAM_LINK_BITMAP_DIAGNOSTIC_BASIS_PREFIX}{slug}-diagnostic"


def joined(values: Sequence[object]) -> str:
    return "|".join(str(value) for value in values)


def metadata_int(metadata: dict[str, object], key: str) -> int | None:
    value = metadata.get(key)
    return value if isinstance(value, int) else None


def metadata_str(metadata: dict[str, object], key: str) -> str:
    value = metadata.get(key)
    return value if isinstance(value, str) else ""


def _parse_int_cell(value: str) -> int | None:
    if not value:
        return None
    try:
        return int(value, 0)
    except ValueError:
        return None


def _sfs_placement_metadata_by_offset(container: AxklibContainer) -> dict[int, dict[str, object]]:
    if container.kind != "sfs":
        return {}
    try:
        with tempfile.TemporaryDirectory(prefix="axklib-relationships-") as tmp:
            inventory_dir = Path(tmp) / "inventory"
            sfs_inventory.build_inventory(container.source_path, inventory_dir)
            path = inventory_dir / "volume_objects.csv"
            result: dict[int, dict[str, object]] = {}
            with path.open("r", newline="", encoding="utf-8") as handle:
                for row in csv.DictReader(handle):
                    object_offset = _parse_int_cell(row.get("object_offset", ""))
                    if object_offset is None:
                        continue
                    partition_index = _parse_int_cell(row.get("partition_index", ""))
                    result[object_offset] = {
                        "sfs_placement_partition_index": partition_index,
                        "sfs_placement_volume_name": row.get("volume_name", ""),
                        "sfs_placement_category_name": row.get("category_name", ""),
                        "sfs_placement_entry_name": row.get("entry_name", ""),
                        "sfs_placement_quality": row.get("match_quality", ""),
                    }
            return result
    except Exception:
        return {}


def _object_with_metadata(item: ObjectItem, metadata: dict[str, object]) -> ObjectItem:
    merged = dict(item.metadata)
    merged.update(metadata)
    return ObjectItem(
        ref=item.ref,
        container=item.container,
        volume=item.volume,
        object_type=item.object_type,
        object_format=item.object_format,
        name=item.name,
        payload=item.payload,
        header=item.header,
        quality=item.quality,
        metadata=merged,
        temporary_extensions=item.temporary_extensions,
    )


def _objects_with_sfs_placement_metadata(container: AxklibContainer) -> tuple[ObjectItem, ...]:
    metadata_by_offset = _sfs_placement_metadata_by_offset(container)
    if not metadata_by_offset:
        return container.objects
    items: list[ObjectItem] = []
    for item in container.objects:
        metadata = (
            metadata_by_offset.get(item.payload_offset) if item.payload_offset is not None else None
        )
        items.append(_object_with_metadata(item, metadata) if metadata is not None else item)
    return tuple(items)


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
        iso_extent_sector=metadata_int(item.metadata, "iso_extent_sector"),
        iso_data_offset=metadata_int(item.metadata, "iso_data_offset"),
        iso_file_size=metadata_int(item.metadata, "iso_file_size"),
        iso_recovery_quality=metadata_str(item.metadata, "iso_recovery_quality"),
        fat_directory_offset=metadata_int(item.metadata, "fat_directory_offset"),
        fat_first_cluster=metadata_int(item.metadata, "fat_first_cluster"),
        fat_cluster_count=metadata_int(item.metadata, "fat_cluster_count"),
        fat_file_size=metadata_int(item.metadata, "fat_file_size"),
        fat_object_offset=metadata_int(item.metadata, "fat_object_offset"),
        fat_stored_payload_offset=metadata_int(item.metadata, "fat_stored_payload_offset"),
        placement_partition_index=metadata_int(item.metadata, "sfs_placement_partition_index"),
        placement_volume_name=metadata_str(item.metadata, "sfs_placement_volume_name"),
        placement_category_name=metadata_str(item.metadata, "sfs_placement_category_name"),
        placement_entry_name=metadata_str(item.metadata, "sfs_placement_entry_name"),
        placement_quality=metadata_str(item.metadata, "sfs_placement_quality"),
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


TYPE_DIRECTORY_NAMES = {"SMPL", "SBNK", "SBAC", "SEQU", "PROG", "PRF3"}


def logical_object_group(ref: ObjectRef) -> str:
    parts = ref.fat_file.replace("\\", "/").split("/")
    if len(parts) >= 3 and parts[-2] in TYPE_DIRECTORY_NAMES:
        return "/".join(parts[:-2])
    return ""


def colocated_candidates(source_ref: ObjectRef, candidates: list[ObjectRef]) -> list[ObjectRef]:
    source_group = logical_object_group(source_ref)
    if not source_group:
        return []
    return [ref for ref in candidates if logical_object_group(ref) == source_group]


def is_iso_cross_folder_match(source_ref: ObjectRef, target_ref: ObjectRef) -> bool:
    """Return true when two ISO objects are in different logical object folders."""

    if source_ref.container_kind != "iso" or target_ref.container_kind != "iso":
        return False
    source_group = logical_object_group(source_ref)
    target_group = logical_object_group(target_ref)
    return bool(source_group and target_group and source_group != target_group)


def same_sfs_volume_candidates(
    source_ref: ObjectRef, candidates: list[ObjectRef]
) -> list[ObjectRef]:
    if (
        source_ref.container_kind != "sfs"
        or source_ref.placement_quality != "Known"
        or not source_ref.placement_volume_name
    ):
        return []
    return [
        ref
        for ref in candidates
        if ref.container_kind == "sfs"
        and ref.image == source_ref.image
        and ref.scope_key == source_ref.scope_key
        and ref.placement_quality == "Known"
        and ref.placement_partition_index == source_ref.placement_partition_index
        and ref.placement_volume_name == source_ref.placement_volume_name
    ]


def match_unique_sbnk_name(
    name: str,
    *,
    source_ref: ObjectRef,
    by_type_name: dict[str, dict[str, list[ObjectRef]]],
) -> MatchResult:
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
        colocated = colocated_candidates(source_ref, candidates)
        if len(colocated) == 1:
            return MatchResult(
                ref=colocated[0],
                method="active-sbac-slot-name+same-folder",
                quality="Likely",
                notes=(
                    "Multiple same-scope SBNK objects share the counted SBAC slot name, "
                    "but exactly one candidate is in the same logical object folder as the SBAC."
                ),
                candidate_refs=candidates,
            )
        same_volume = same_sfs_volume_candidates(source_ref, candidates)
        if len(same_volume) == 1:
            return MatchResult(
                ref=same_volume[0],
                method="active-sbac-slot-name+same-volume",
                quality="Likely",
                notes=(
                    "Multiple same-scope SBNK objects share the counted SBAC slot name, "
                    "but exactly one candidate is in the same SFS volume as the SBAC."
                ),
                candidate_refs=candidates,
            )
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
    source_ref: ObjectRef,
    by_name: dict[str, list[ObjectRef]],
    by_type_name: dict[str, dict[str, list[ObjectRef]]],
    active_assignment_state: str = ACTIVE_ASSIGNMENT_STATE_UNKNOWN,
) -> MatchResult:
    all_candidates = by_name.get(name, [])
    expected_category = PROG_SLOT_KIND_TARGET_CATEGORY.get(kind_byte, "")
    if expected_category:
        typed_candidates = by_type_name.get(expected_category, {}).get(name, [])
        if len(typed_candidates) == 1:
            return MatchResult(
                ref=typed_candidates[0],
                method=f"assignment-kind-0x{kind_byte:02x}+name",
                quality="Known",
                notes=(
                    "Input consistency: assignment name matches a same-scope object, "
                    f"and assignment kind byte 0x{kind_byte:02x} selects {expected_category} in tested current-object corpora. "
                    "Keep the selector below write-side quality until formula or validated saves support it."
                ),
                candidate_refs=typed_candidates,
            )
        if len(typed_candidates) > 1:
            colocated = colocated_candidates(source_ref, typed_candidates)
            if len(colocated) == 1:
                return MatchResult(
                    ref=colocated[0],
                    method=f"assignment-kind-0x{kind_byte:02x}+name+same-folder",
                    quality="Likely",
                    notes=(
                        f"Assignment kind byte 0x{kind_byte:02x} points at {expected_category}, "
                        "and exactly one duplicate-name candidate is in the same logical object folder as the PROG."
                    ),
                    candidate_refs=typed_candidates,
                )
            same_volume = (
                same_sfs_volume_candidates(source_ref, typed_candidates)
                if active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE
                else []
            )
            if len(same_volume) == 1:
                return MatchResult(
                    ref=same_volume[0],
                    method=f"assignment-kind-0x{kind_byte:02x}+name+same-volume",
                    quality="Likely",
                    notes=(
                        f"Assignment kind byte 0x{kind_byte:02x} points at {expected_category}, "
                        "multiple same-scope SFS objects share that name and category, "
                        "and exactly one active-row candidate is in the same SFS volume as the PROG."
                    ),
                    candidate_refs=typed_candidates,
                )
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
        colocated = colocated_candidates(source_ref, all_candidates)
        if len(colocated) == 1:
            return MatchResult(
                ref=colocated[0],
                method="assignment-name+same-folder",
                quality="Likely",
                notes=(
                    "Multiple same-scope objects share this assignment name, "
                    "but exactly one candidate is in the same logical object folder as the PROG."
                ),
                candidate_refs=all_candidates,
            )
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


def resolve_prog_rows_from_program_context(
    prog_rows: list[ProgBankRow],
    refs_by_key: dict[str, ObjectRef],
    sbac_child_counts: dict[str, int],
) -> list[ProgBankRow]:
    """Resolve unmatched PROG rows from another resolved row in the same PROG.

    This deliberately ignores PROG row +0x10..+0x13. Sampler-authored PSLCD-101
    saves show those bytes can be preserved from an imported source even when
    the target SBAC objects are written with new HDS identities.
    """

    resolved_by_prog_and_type: dict[tuple[str, str], set[str]] = defaultdict(set)
    for row in prog_rows:
        if (
            row.match_quality in {"Known", "Likely"}
            and row.matched_target_object_key
            and row.matched_target_type
            and row.matched_target_type == row.selector_expected_category
        ):
            resolved_by_prog_and_type[(row.prog_object_key, row.matched_target_type)].add(
                row.matched_target_object_key
            )

    resolved_rows: list[ProgBankRow] = []
    for row in prog_rows:
        if (
            row.match_method != "assignment-unmatched"
            or row.candidate_count != 0
            or not row.selector_expected_category
        ):
            resolved_rows.append(row)
            continue
        target_keys = resolved_by_prog_and_type.get(
            (row.prog_object_key, row.selector_expected_category), set()
        )
        if len(target_keys) != 1:
            resolved_rows.append(row)
            continue
        target_key = next(iter(target_keys))
        target_ref = refs_by_key.get(target_key)
        if target_ref is None:
            resolved_rows.append(row)
            continue
        child_count = (
            sbac_child_counts.get(target_ref.object_key) if target_ref.type == "SBAC" else None
        )
        method = (
            f"assignment-kind-0x{row.assignment_kind_byte_0x14:02x}+program-local-target-context"
        )
        resolved_rows.append(
            replace(
                row,
                match_method=method,
                match_quality="Likely",
                match_notes=(
                    "Sampler-authored HDS placement context: this unmatched assignment row shares "
                    f"a PROG with exactly one resolved {target_ref.type} assignment. "
                    "The assignment raw handle at row +0x10..+0x13 is a diagnostic imported/source value only."
                ),
                candidate_count=1,
                candidate_categories=target_ref.type,
                candidate_object_keys=target_ref.object_key,
                candidate_fat_files=target_ref.fat_file,
                candidate_names=target_ref.name,
                matched_target_type=target_ref.type,
                matched_target_object_key=target_ref.object_key,
                matched_target_partition_index=target_ref.partition_index,
                matched_target_sfs_id=target_ref.sfs_id,
                matched_target_fat_file=target_ref.fat_file,
                matched_target_payload_offset=target_ref.payload_offset,
                matched_target_name=target_ref.name,
                matched_sbac_child_sbnk_count=child_count,
                notes="program-local-target-context;raw-handle-diagnostic-only",
                matched_target_iso_extent_sector=target_ref.iso_extent_sector,
                matched_target_iso_data_offset=target_ref.iso_data_offset,
                matched_target_iso_file_size=target_ref.iso_file_size,
                matched_target_iso_recovery_quality=target_ref.iso_recovery_quality,
                matched_target_fat_directory_offset=target_ref.fat_directory_offset,
                matched_target_fat_first_cluster=target_ref.fat_first_cluster,
                matched_target_fat_cluster_count=target_ref.fat_cluster_count,
                matched_target_fat_file_size=target_ref.fat_file_size,
                matched_target_fat_object_offset=target_ref.fat_object_offset,
                matched_target_fat_stored_payload_offset=target_ref.fat_stored_payload_offset,
            )
        )
    return resolved_rows


def promote_source_load_same_folder_rows(prog_rows: list[ProgBankRow]) -> list[ProgBankRow]:
    """Promote validated ISO source-load same-folder Program assignment matches."""

    promoted_rows: list[ProgBankRow] = []
    promoted_methods = {
        "SBNK": ("assignment-kind-0x10+name+same-folder", "0x10"),
        "SBAC": ("assignment-kind-0x11+name+same-folder", "0x11"),
    }
    for row in prog_rows:
        method_and_kind = promoted_methods.get(row.selector_expected_category)
        if (
            row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_SOURCE_LOAD_ASSIGNMENT
            and method_and_kind is not None
            and row.match_method == method_and_kind[0]
            and row.match_quality == "Likely"
            and row.matched_target_object_key
        ):
            promoted_rows.append(
                replace(
                    row,
                    match_quality="Known",
                    match_notes=(
                        f"Input consistency: assignment kind byte {method_and_kind[1]} points at "
                        f"{row.selector_expected_category}, the row is a source-load assignment, "
                        "and exactly one duplicate-name candidate is in the same logical object "
                        "folder as the PROG."
                    ),
                )
            )
            continue
        promoted_rows.append(row)
    return promoted_rows


def classify_duplicate_prog_assignment_rows(prog_rows: list[ProgBankRow]) -> list[ProgBankRow]:
    """Mark repeated non-active/source-load rows to the same target as duplicates."""

    grouped: dict[tuple[str, str, str, str], list[ProgBankRow]] = defaultdict(list)
    for row in prog_rows:
        if not row.matched_target_object_key:
            continue
        grouped[
            (
                row.prog_object_key,
                row.selector_expected_category,
                row.matched_target_object_key,
                row.assignment_name,
            )
        ].append(row)

    duplicate_keys: set[tuple[str, int]] = set()
    for rows in grouped.values():
        if len(rows) < 2:
            continue
        sorted_rows = sorted(rows, key=lambda row: row.assignment_index)
        for row in sorted_rows[1:]:
            duplicate_keys.add((row.prog_object_key, row.assignment_index))

    return [
        replace(row, active_assignment_state=ACTIVE_ASSIGNMENT_STATE_CONFIRMED_DUPLICATE_NOT_ACTIVE)
        if (row.prog_object_key, row.assignment_index) in duplicate_keys
        and row.active_assignment_state
        in {
            ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF,
            ACTIVE_ASSIGNMENT_STATE_SOURCE_LOAD_ASSIGNMENT,
        }
        else row
        for row in prog_rows
    ]


def classify_unresolved_direct_prog_rows(prog_rows: list[ProgBankRow]) -> list[ProgBankRow]:
    """Label unresolved Program assignments without promoting raw selectors."""

    classified_rows: list[ProgBankRow] = []
    for row in prog_rows:
        if not (
            row.match_method == "assignment-unmatched"
            and row.assignment_raw_handle_0x10 != 0
            and row.candidate_count == 0
            and not row.matched_target_object_key
        ):
            classified_rows.append(row)
            continue

        notes = [note for note in row.notes.split(";") if note]
        if (
            row.selector_expected_category == "SBNK"
            and row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE
        ):
            notes.extend(["active-missing-local-target", "raw-selector-diagnostic-only"])
            classified_rows.append(
                replace(
                    row,
                    match_method="assignment-active-missing-local-target",
                    match_notes=(
                        "This active direct PROG assignment names an SBNK object that is not present "
                        "as an exact same-scope target. Preserve the row as an active decoded Program "
                        "row with a missing local target; the row +0x10..+0x13 selector is "
                        "diagnostic only and is not used to select a neighboring SBNK target."
                    ),
                    notes=";".join(notes),
                )
            )
            continue

        notes.append("raw-selector-diagnostic-only")
        if row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF:
            if row.selector_expected_category == "SBNK":
                notes.append("visible-off-missing-local-sbnk")
                classified_rows.append(
                    replace(
                        row,
                        match_method="assignment-visible-off-missing-local-sbnk",
                        match_notes=(
                            "This visible/off direct PROG assignment names an SBNK object that is not "
                            "present as an exact same-scope target. Preserve the row as decoded "
                            "Program inventory, but do not treat it as active Program content; the "
                            "row +0x10..+0x13 selector is diagnostic only."
                        ),
                        notes=";".join(notes),
                    )
                )
                continue
            if row.selector_expected_category == "SBAC":
                method = (
                    "assignment-visible-off-iso-missing-local-sbac"
                    if row.container_kind == "iso"
                    else "assignment-visible-off-missing-local-sbac"
                )
                notes.append(method)
                classified_rows.append(
                    replace(
                        row,
                        match_method=method,
                        match_notes=(
                            "This visible/off PROG assignment names an SBAC group object that is not "
                            "present as an exact same-scope target. Preserve the row as decoded "
                            "Program inventory, but do not treat it as active Program content; the "
                            "row +0x10..+0x13 selector is diagnostic only."
                        ),
                        notes=";".join(notes),
                    )
                )
                continue

        if row.selector_expected_category == "SBNK":
            classified_rows.append(
                replace(
                    row,
                    match_method="assignment-unmatched-preserved-source-selector",
                    match_notes=(
                        "No same-scope SBNK object name matches this direct PROG assignment name, "
                        "and the active/off state is not classified by the current public rule. "
                        "The row +0x10..+0x13 selector is preserved for diagnostics only and is "
                        "not used to select a local SBNK target."
                    ),
                    notes=";".join(notes),
                )
            )
            continue
        classified_rows.append(row)
    return classified_rows


def classify_visible_off_ambiguous_prog_rows(
    prog_rows: list[ProgBankRow], refs_by_key: dict[str, ObjectRef]
) -> list[ProgBankRow]:
    """Label visible/off ambiguous rows as inventory diagnostics, not active content loss."""

    classified_rows: list[ProgBankRow] = []
    for row in prog_rows:
        if (
            row.active_assignment_state != ACTIVE_ASSIGNMENT_STATE_CONFIRMED_VISIBLE_OFF
            or row.matched_target_object_key
        ):
            classified_rows.append(row)
            continue

        notes = [note for note in row.notes.split(";") if note]
        source_ref = refs_by_key.get(row.prog_object_key)
        candidate_refs = [refs_by_key[key] for key in candidate_key_set(row) if key in refs_by_key]
        same_volume = (
            same_sfs_volume_candidates(source_ref, candidate_refs) if source_ref is not None else []
        )
        if (
            row.match_method
            in {"assignment-kind-0x10+name-ambiguous", "assignment-kind-0x11+name-ambiguous"}
            and len(same_volume) == 1
        ):
            expected = "SBNK object" if row.selector_expected_category == "SBNK" else "SBAC group"
            target_slug = "sbnk" if row.selector_expected_category == "SBNK" else "sbac"
            method = f"assignment-visible-off-same-volume-{target_slug}-diagnostic"
            notes.append(method)
            classified_rows.append(
                replace(
                    row,
                    match_method=method,
                    match_notes=(
                        f"This visible/off PROG assignment names an {expected}, and exactly one "
                        "duplicate-name candidate is in the same SFS volume as the PROG. Preserve "
                        "the same-volume candidate as diagnostic Program inventory only; do not "
                        "treat the row as active Program content or a resolved target."
                    ),
                    notes=";".join(notes),
                )
            )
            continue
        if row.match_method == "assignment-kind-0x11+name-ambiguous":
            notes.append("visible-off-name-ambiguous-sbac")
            classified_rows.append(
                replace(
                    row,
                    match_method="assignment-visible-off-name-ambiguous-sbac",
                    match_notes=(
                        "This visible/off PROG assignment names an SBAC group, but multiple "
                        "same-scope SBAC objects share that name. Preserve the candidate set as "
                        "decoded Program inventory, but do not treat the row as active Program content."
                    ),
                    notes=";".join(notes),
                )
            )
            continue
        if row.match_method == "assignment-kind-0x10+name-ambiguous":
            notes.append("visible-off-name-ambiguous-sbnk")
            classified_rows.append(
                replace(
                    row,
                    match_method="assignment-visible-off-name-ambiguous-sbnk",
                    match_notes=(
                        "This visible/off direct PROG assignment names an SBNK object, but multiple "
                        "same-scope SBNK objects share that name. Preserve the candidate set as "
                        "decoded Program inventory, but do not treat the row as active Program content."
                    ),
                    notes=";".join(notes),
                )
            )
            continue
        if row.match_method == "assignment-name-ambiguous":
            candidate_categories = {ref.type for ref in candidate_refs}
            if row.selector_expected_category == "SBNK" and candidate_categories == {"SMPL"}:
                method = "assignment-visible-off-name-ambiguous-smpl-candidates"
                note = (
                    "This visible/off direct PROG assignment names an SBNK object, but only "
                    "duplicate physical SMPL waveform objects match that name. Preserve the "
                    "candidate set as decoded Program inventory, but do not treat the row as "
                    "active Program content."
                )
            else:
                method = "assignment-visible-off-name-ambiguous-non-target-category"
                note = (
                    "This visible/off PROG assignment has duplicate same-scope name matches, "
                    "but the ambiguous candidates do not match the decoded target category. "
                    "Preserve the candidate set as decoded Program inventory, but do not treat "
                    "the row as active Program content."
                )
            notes.append(method)
            classified_rows.append(
                replace(
                    row,
                    match_method=method,
                    match_notes=note,
                    notes=";".join(notes),
                )
            )
            continue

        classified_rows.append(row)
    return classified_rows


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
    source_ref: ObjectRef,
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
        colocated = colocated_candidates(source_ref, exact)
        if len(colocated) == 1:
            return MatchResult(
                ref=colocated[0],
                method="sbnk-member-link+name+same-folder",
                quality="Likely",
                notes=(
                    "Current SBNK member name and member link ID match multiple same-scope SMPL objects, "
                    "but exactly one candidate is in the same logical object folder as the SBNK."
                ),
                candidate_refs=exact,
            )
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
            quality="Likely",
            notes="Current SBNK member name uniquely matches a same-scope SMPL object, but the member link ID did not confirm it.",
            candidate_refs=name_candidates,
        )
    if len(link_candidates) == 1:
        if is_iso_cross_folder_match(source_ref, link_candidates[0]):
            return MatchResult(
                ref=link_candidates[0],
                method="sbnk-member-link-id-only-iso-cross-folder-name-mismatch",
                quality="Tentative",
                notes=(
                    "Current SBNK member link ID uniquely matches a same-scope SMPL object "
                    "in another ISO object folder, but the member name did not confirm it."
                ),
                candidate_refs=link_candidates,
            )
        return MatchResult(
            ref=link_candidates[0],
            method="sbnk-member-link-id-only-name-mismatch",
            quality="Tentative",
            notes="Current SBNK member link ID uniquely matches a same-scope SMPL object, but the member name did not confirm it.",
            candidate_refs=link_candidates,
        )
    if name_candidates:
        colocated = colocated_candidates(source_ref, name_candidates)
        if len(colocated) == 1:
            return MatchResult(
                ref=colocated[0],
                method="sbnk-member-name+same-folder",
                quality="Likely",
                notes=(
                    "Current SBNK member name matches multiple same-scope SMPL objects, "
                    "but exactly one candidate is in the same logical object folder as the SBNK."
                ),
                candidate_refs=name_candidates,
            )
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
        source_ref = refs_by_key[item.object_key]
        try:
            members = decode_current_sbnk_members(item.payload)
        except Exception:
            continue
        member_specs = [
            (
                "left",
                "SBNK_LEFT_MEMBER_TO_SMPL",
                members.left.sample_name,
                members.left.smpl_link_id,
                "SBNK+left member",
            ),
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
                source_ref=source_ref,
                by_link=smpl_by_link,
                by_name=smpl_by_name,
            )
            target = (
                match.ref.object_key
                if match.ref is not None
                else candidate_object_keys(match.candidate_refs)
            )
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
        elif (
            prog_row.selector_expected_category == "SBNK" and prog_row.match_quality == "Tentative"
        ):
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
        direct_programs = [
            program for row in direct_rows if (program := prog_row_program(row)) is not None
        ]
        ambiguous_programs = [
            program for row in ambiguous_rows if (program := prog_row_program(row)) is not None
        ]
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


def scan_scope(
    items: list[ObjectItem],
) -> tuple[list[SbacSbnkRow], list[ProgBankRow], list[ProgIgnoredRow], list[SbnkProgramBitmapRow]]:
    refs = [object_ref(item) for item in items]
    refs_by_key = {ref.object_key: ref for ref in refs}
    by_type_name = refs_by_type_and_name(refs)
    by_name = refs_by_name([ref for ref in refs if ref.type in {"SMPL", "SBNK", "SBAC"}])
    sbac_rows: list[SbacSbnkRow] = []
    sbac_child_counts: dict[str, int] = {}

    for item in items:
        payload = item.payload
        if item.type != "SBAC":
            continue
        slot_count, max_slots, slots = iter_sbac_slots(payload)
        source_ref = refs_by_key[item.object_key]
        if not slots and len(payload) <= SBAC_SLOT_COUNT_OFFSET:
            continue
        active_matches = 0
        for slot in slots:
            if not slot.name:
                continue
            match = match_unique_sbnk_name(
                slot.name, source_ref=source_ref, by_type_name=by_type_name
            )
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
                    sbac_iso_extent_sector=source_ref.iso_extent_sector,
                    sbac_iso_data_offset=source_ref.iso_data_offset,
                    sbac_iso_file_size=source_ref.iso_file_size,
                    sbac_iso_recovery_quality=source_ref.iso_recovery_quality,
                    sbac_fat_directory_offset=source_ref.fat_directory_offset,
                    sbac_fat_first_cluster=source_ref.fat_first_cluster,
                    sbac_fat_cluster_count=source_ref.fat_cluster_count,
                    sbac_fat_file_size=source_ref.fat_file_size,
                    sbac_fat_object_offset=source_ref.fat_object_offset,
                    sbac_fat_stored_payload_offset=source_ref.fat_stored_payload_offset,
                    matched_sbnk_iso_extent_sector=match.ref.iso_extent_sector
                    if match.ref
                    else None,
                    matched_sbnk_iso_data_offset=match.ref.iso_data_offset if match.ref else None,
                    matched_sbnk_iso_file_size=match.ref.iso_file_size if match.ref else None,
                    matched_sbnk_iso_recovery_quality=match.ref.iso_recovery_quality
                    if match.ref
                    else "",
                    matched_sbnk_fat_directory_offset=match.ref.fat_directory_offset
                    if match.ref
                    else None,
                    matched_sbnk_fat_first_cluster=match.ref.fat_first_cluster
                    if match.ref
                    else None,
                    matched_sbnk_fat_cluster_count=match.ref.fat_cluster_count
                    if match.ref
                    else None,
                    matched_sbnk_fat_file_size=match.ref.fat_file_size if match.ref else None,
                    matched_sbnk_fat_object_offset=match.ref.fat_object_offset
                    if match.ref
                    else None,
                    matched_sbnk_fat_stored_payload_offset=match.ref.fat_stored_payload_offset
                    if match.ref
                    else None,
                )
            )
        sbac_child_counts[item.object_key] = active_matches

    prog_rows: list[ProgBankRow] = []
    ignored_rows: list[ProgIgnoredRow] = []
    for item in items:
        payload = item.payload
        if item.type != "PROG":
            continue
        source_ref = refs_by_key[item.object_key]
        item_prog_rows: list[ProgBankRow] = []
        item_ignored_rows: list[ProgIgnoredRow] = []
        for assignment in iter_prog_assignments(payload):
            index = assignment.index
            offset = assignment.offset
            assignment_name = objects.clean_ascii(
                assignment.raw_name.encode("ascii", errors="replace")
            )
            if not assignment_name:
                continue
            raw_handle = assignment.raw_handle_0x10
            kind_byte = assignment.kind_byte_0x14
            flag_byte = assignment.flag_byte_0x15
            active_assignment_state = classify_active_assignment_state(
                assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
                rch_assign_gate_byte_0x28=assignment.output2_0x28,
            )
            rch_assign_display = classify_rch_assign_display(
                assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
                midi_receive_channel_assign_byte_0x15=assignment.midi_receive_channel_assign_0x15,
                rch_assign_gate_byte_0x28=assignment.output2_0x28,
            )
            if kind_byte not in PROG_SLOT_KIND_TARGET_CATEGORY and assignment_name not in by_name:
                item_ignored_rows.append(
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
                source_ref=source_ref,
                by_name=by_name,
                by_type_name=by_type_name,
                active_assignment_state=active_assignment_state,
            )
            if match.quality == "Unknown" and raw_handle == 0:
                item_ignored_rows.append(
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
                        reason="ignored-null-handle-unmatched-assignment",
                    )
                )
                continue
            reported_active_assignment_state = classify_source_load_assignment_state(
                container_kind=item.container_kind,
                matched_target_object_key=match.ref.object_key if match.ref else "",
                active_assignment_state=active_assignment_state,
                assignment_output1_byte_0x1d=assignment.output1_0x1d,
                assignment_rch_assign_gate_byte_0x28=assignment.output2_0x28,
            )
            reported_rch_assign_display = (
                RCH_ASSIGN_DISPLAY_UNKNOWN
                if reported_active_assignment_state
                == ACTIVE_ASSIGNMENT_STATE_SOURCE_LOAD_ASSIGNMENT
                else rch_assign_display
            )
            prog_notes: list[str] = []
            if match.quality not in {"Known", "Likely"}:
                prog_notes.append(f"match_method={match.method}")
            if kind_byte not in PROG_SLOT_KIND_TARGET_CATEGORY:
                prog_notes.append("unmapped_assignment_kind_byte")
            if reported_active_assignment_state == ACTIVE_ASSIGNMENT_STATE_SOURCE_LOAD_ASSIGNMENT:
                prog_notes.append("source-load-assignment")
            child_count = (
                sbac_child_counts.get(match.ref.object_key)
                if match.ref is not None and match.ref.type == "SBAC"
                else None
            )
            item_prog_rows.append(
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
                    assignment_output1_byte_0x1d=assignment.output1_0x1d,
                    assignment_rch_assign_gate_byte_0x28=assignment.output2_0x28,
                    assignment_rch_assign_display=reported_rch_assign_display,
                    selector_expected_category=PROG_SLOT_KIND_TARGET_CATEGORY.get(kind_byte, ""),
                    assignment_row_state=ASSIGNMENT_ROW_STATE_DECODED,
                    active_assignment_state=reported_active_assignment_state,
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
                    prog_iso_extent_sector=source_ref.iso_extent_sector,
                    prog_iso_data_offset=source_ref.iso_data_offset,
                    prog_iso_file_size=source_ref.iso_file_size,
                    prog_iso_recovery_quality=source_ref.iso_recovery_quality,
                    prog_fat_directory_offset=source_ref.fat_directory_offset,
                    prog_fat_first_cluster=source_ref.fat_first_cluster,
                    prog_fat_cluster_count=source_ref.fat_cluster_count,
                    prog_fat_file_size=source_ref.fat_file_size,
                    prog_fat_object_offset=source_ref.fat_object_offset,
                    prog_fat_stored_payload_offset=source_ref.fat_stored_payload_offset,
                    matched_target_iso_extent_sector=match.ref.iso_extent_sector
                    if match.ref
                    else None,
                    matched_target_iso_data_offset=match.ref.iso_data_offset if match.ref else None,
                    matched_target_iso_file_size=match.ref.iso_file_size if match.ref else None,
                    matched_target_iso_recovery_quality=match.ref.iso_recovery_quality
                    if match.ref
                    else "",
                    matched_target_fat_directory_offset=match.ref.fat_directory_offset
                    if match.ref
                    else None,
                    matched_target_fat_first_cluster=match.ref.fat_first_cluster
                    if match.ref
                    else None,
                    matched_target_fat_cluster_count=match.ref.fat_cluster_count
                    if match.ref
                    else None,
                    matched_target_fat_file_size=match.ref.fat_file_size if match.ref else None,
                    matched_target_fat_object_offset=match.ref.fat_object_offset
                    if match.ref
                    else None,
                    matched_target_fat_stored_payload_offset=match.ref.fat_stored_payload_offset
                    if match.ref
                    else None,
                )
            )
        last_active_assignment_index = max(
            (row.assignment_index for row in item_prog_rows), default=-1
        )
        prog_rows.extend(item_prog_rows)
        ignored_rows.extend(
            row
            for row in item_ignored_rows
            if row.reason != "ignored-reserved-or-tail-slot-no-known-kind-and-no-name-match"
            or last_active_assignment_index < 0
            or row.assignment_index <= last_active_assignment_index
        )
    prog_rows = resolve_prog_rows_from_program_context(prog_rows, refs_by_key, sbac_child_counts)
    prog_rows = promote_source_load_same_folder_rows(prog_rows)
    prog_rows = classify_unresolved_direct_prog_rows(prog_rows)
    prog_rows = classify_visible_off_ambiguous_prog_rows(prog_rows, refs_by_key)
    prog_rows = classify_duplicate_prog_assignment_rows(prog_rows)
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
        sbac_items = (
            sbac_rows if scope == "ALL" else [row for row in sbac_rows if row.scope_key == scope]
        )
        prog_items = (
            prog_rows if scope == "ALL" else [row for row in prog_rows if row.scope_key == scope]
        )
        ignored_items = (
            ignored_rows
            if scope == "ALL"
            else [row for row in ignored_rows if row.scope_key == scope]
        )
        bitmap_items = (
            sbnk_bitmap_rows
            if scope == "ALL"
            else [row for row in sbnk_bitmap_rows if row.scope_key == scope]
        )
        prog_methods = Counter(row.match_method for row in prog_items)
        prog_kinds = Counter(f"0x{row.assignment_kind_byte_0x14:02x}" for row in prog_items)
        prog_assignment_row_states = Counter(row.assignment_row_state for row in prog_items)
        prog_active_assignment_states = Counter(row.active_assignment_state for row in prog_items)
        bitmap_mismatch_classes = Counter(
            row.mismatch_class for row in bitmap_items if row.match_status == "mismatch"
        )
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
                "sbac_known_sbnk_name_count": sum(
                    1 for row in sbac_items if row.match_quality == "Known"
                ),
                "sbac_unknown_count": sum(1 for row in sbac_items if row.match_quality != "Known"),
                "prog_assignment_row_count": len(prog_items),
                "prog_ignored_reserved_or_tail_row_count": len(ignored_items),
                "prog_known_count": sum(1 for row in prog_items if row.match_quality == "Known"),
                "prog_likely_count": sum(1 for row in prog_items if row.match_quality == "Likely"),
                "prog_tentative_or_unknown_count": sum(
                    1 for row in prog_items if row.match_quality not in {"Known", "Likely"}
                ),
                "prog_sbac_target_count": sum(
                    1 for row in prog_items if row.matched_target_type == "SBAC"
                ),
                "prog_sbnk_target_count": sum(
                    1 for row in prog_items if row.matched_target_type == "SBNK"
                ),
                "prog_smpl_target_count": sum(
                    1 for row in prog_items if row.matched_target_type == "SMPL"
                ),
                "sbnk_program_bitmap_row_count": len(bitmap_items),
                "sbnk_program_bitmap_match_count": sum(
                    1 for row in bitmap_items if row.match_status == "match"
                ),
                "sbnk_program_bitmap_mismatch_count": sum(
                    1 for row in bitmap_items if row.match_status == "mismatch"
                ),
                "sbnk_program_bitmap_with_direct_assignment_count": sum(
                    1 for row in bitmap_items if row.direct_prog_assignment_programs
                ),
                "sbnk_program_bitmap_with_sbac_indirect_assignment_count": sum(
                    1 for row in bitmap_items if row.sbac_indirect_assignment_programs
                ),
                "sbnk_program_bitmap_mismatch_class_counts": json.dumps(
                    dict(sorted(bitmap_mismatch_classes.items()))
                ),
                "prog_assignment_method_counts": json.dumps(dict(sorted(prog_methods.items()))),
                "prog_assignment_kind_byte_counts": json.dumps(dict(sorted(prog_kinds.items()))),
                "prog_assignment_row_state_counts": json.dumps(
                    dict(sorted(prog_assignment_row_states.items()))
                ),
                "prog_active_assignment_state_counts": json.dumps(
                    dict(sorted(prog_active_assignment_states.items()))
                ),
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
                    key=_relationship_key(
                        sbac_row.sbac_object_key, "SBAC_SLOT_TO_SBNK", target, sbac_row.match_method
                    ),
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
                if prog_row.matched_target_type == "SBAC"
                or prog_row.selector_expected_category == "SBAC"
                else "PROG_ASSIGNMENT_TO_SBNK"
                if prog_row.matched_target_type == "SBNK"
                or prog_row.selector_expected_category == "SBNK"
                else "PROG_ASSIGNMENT_TO_OBJECT"
            )
            target = prog_row.matched_target_object_key or prog_row.candidate_object_keys
            relationships.append(
                Relationship(
                    key=_relationship_key(
                        prog_row.prog_object_key, rel_type, target, prog_row.match_method
                    ),
                    source_key=prog_row.prog_object_key,
                    target_key=target,
                    relationship_type=rel_type,
                    quality=prog_row.match_quality,
                    basis=prog_row.match_method,
                    raw_fields=f"PROG assignment {prog_row.assignment_index} at 0x{prog_row.assignment_offset:03x}",
                    ambiguity_notes=prog_row.match_notes,
                    source_image=prog_row.image,
                    scope_key=prog_row.scope_key,
                    assignment_index=prog_row.assignment_index,
                    assignment_name=prog_row.assignment_name,
                    assignment_row_state=prog_row.assignment_row_state,
                    active_assignment_state=prog_row.active_assignment_state,
                    assignment_rch_assign_display=prog_row.assignment_rch_assign_display,
                )
            )
        for bitmap_row in sbnk_bitmap_rows:
            if bitmap_row.bitmap_programs:
                basis = sbnk_program_bitmap_relationship_basis(bitmap_row)
                relationships.append(
                    Relationship(
                        key=_relationship_key(
                            bitmap_row.sbnk_object_key,
                            "SBNK_PROGRAM_BITMAP_TO_PROG",
                            bitmap_row.bitmap_programs,
                            basis,
                        ),
                        source_key=bitmap_row.sbnk_object_key,
                        target_key=bitmap_row.bitmap_programs,
                        relationship_type="SBNK_PROGRAM_BITMAP_TO_PROG",
                        quality=bitmap_row.quality,
                        basis=basis,
                        raw_fields="SBNK+0x0c0..0x0cf",
                        ambiguity_notes=bitmap_row.notes,
                        source_image=bitmap_row.image,
                        scope_key=bitmap_row.scope_key,
                    )
                )
    relationships = [
        replace(row, diagnostic_category=relationship_diagnostic_category(row))
        for row in relationships
    ]
    return RelationshipGraph(
        relationships=tuple(sorted(relationships, key=_relationship_sort_key)),
        sbac_sbnk_rows=tuple(
            sorted(
                all_sbac_rows,
                key=lambda row: (
                    row.image,
                    row.scope_key,
                    row.sbac_object_key,
                    row.slot_index,
                    row.candidate_object_keys,
                ),
            )
        ),
        prog_bank_rows=tuple(
            sorted(
                all_prog_rows,
                key=lambda row: (
                    row.image,
                    row.scope_key,
                    row.prog_object_key,
                    row.assignment_index,
                    row.candidate_object_keys,
                ),
            )
        ),
        prog_ignored_rows=tuple(
            sorted(
                all_ignored_rows,
                key=lambda row: (
                    row.image,
                    row.scope_key,
                    row.prog_object_key,
                    row.assignment_index,
                ),
            )
        ),
        sbnk_bitmap_rows=tuple(
            sorted(
                all_sbnk_bitmap_rows,
                key=lambda row: (row.image, row.scope_key, row.sbnk_object_key),
            )
        ),
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
        items.extend(_objects_with_sfs_placement_metadata(result.container))
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


def build_relationship_graph_for_paths(
    paths: list[Path], container: str = "auto"
) -> RelationshipGraph:
    return build_relationship_graph_for_path_results(paths, container=container).graph
