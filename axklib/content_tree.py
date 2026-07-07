"""User-facing content tree views for axklib containers."""

from __future__ import annotations

import csv
import re
import tempfile
from collections import Counter, defaultdict
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, replace
from pathlib import Path
from typing import cast

from axklib.audio.structured import _safe_display_path_name
from axklib.containers import (
    AxklibContainer,
    AxklibContainerLoadResult,
    OpenOptions,
    open_many,
    sfs_inventory,
)
from axklib.model import AxklibObject, DataQuality
from axklib.parameters import current as current_parameters
from axklib.relationships import (
    ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE,
    ACTIVE_ASSIGNMENT_STATE_SOURCE_LOAD_ASSIGNMENT,
    Relationship,
    RelationshipGraph,
    build_relationship_graph,
)
from axklib.reports import to_plain
from axklib.validation import validate_container

CATEGORY_ORDER = {
    "Programs": 0,
    "Sample Banks": 1,
    "Waveforms": 2,
    "Sequences": 3,
}
TYPE_CATEGORY = {
    "PROG": "Programs",
    "SBNK": "Sample Banks",
    "SMPL": "Waveforms",
    "SEQU": "Sequences",
}
NODE_LABEL = {
    "PROG": "program",
    "SBAC": "internal_sample_bank_link",
    "SBNK": "sample_bank",
    "SMPL": "waveform",
    "SEQU": "sequence",
}
INTERNAL_OBJECT_TYPES = {"SBAC"}
UNRESOLVED_PROGRAM_ASSIGNMENT_NODE = "unresolved_program_assignment"
ACTIVE_MISSING_LOCAL_TARGET_BASIS = "assignment-active-missing-local-target"
ACTIVE_MISSING_LOCAL_TARGET_NOTE = "active assignment references missing local target"


TYPE_LABEL_BY_OBJECT = {
    "PROG": "PROGRAM",
    "SBAC": "SAMPLE BANK GROUP",
    "SBNK": "SAMPLE BANK",
    "SMPL": "WAVEFORM",
    "SEQU": "SEQUENCE",
}
TYPE_LABEL_BY_NODE = {
    "partition": "PARTITION",
    "volume": "VOLUME",
    "category": "CATEGORY",
    "recovery_artifact": "RECOVERY",
    "unresolved": "UNKNOWN",
    "relationship_target": "UNKNOWN",
    UNRESOLVED_PROGRAM_ASSIGNMENT_NODE: "UNKNOWN",
}


@dataclass(frozen=True)
class ContentTreeIssue:
    code: str
    severity: str
    message: str
    source_path: str
    sampler_path: str = ""
    object_key: str = ""


@dataclass(frozen=True)
class ContentNode:
    node_id: str
    node_type: str
    display_name: str
    object_key: str = ""
    object_type: str = ""
    count: int | None = None
    details: tuple[str, ...] = ()
    quality: DataQuality = DataQuality.KNOWN
    basis: str = ""
    notes: str = ""
    selector_path: str = ""
    children: tuple[ContentNode, ...] = ()


@dataclass(frozen=True)
class ContentTree:
    source_path: str
    container_kind: str
    detected_format: str
    roots: tuple[ContentNode, ...]
    issues: tuple[ContentTreeIssue, ...] = ()


@dataclass(frozen=True)
class ContentTreePathRow:
    source_path: str
    scope: str
    path: str
    display_name: str
    node_type: str
    object_type: str = ""
    object_key: str = ""


@dataclass(frozen=True)
class ContentTreeLoadResult:
    trees: tuple[ContentTree, ...]
    load_errors: tuple[AxklibContainerLoadResult, ...]


CONTENT_TREE_VALIDATION_CODES = {
    "REL_ACTIVE_PROGRAM_SBNK_MEMBER_TARGET_MISSING",
    "REL_SBNK_MEMBER_TARGET_MISSING",
}


def _issue_volume_path(issue: ContentTreeIssue) -> str:
    return issue.sampler_path.split("|", 1)[0].strip()


def _affected_error_volume_paths(issues: Sequence[ContentTreeIssue]) -> set[str]:
    return {
        path
        for issue in issues
        if issue.severity == "error" and (path := _issue_volume_path(issue))
    }


def _display_volume_name(name: str, raw_volume_path: str, affected_error_paths: set[str]) -> str:
    display = _safe_display(name, "<unnamed volume>")
    if raw_volume_path and raw_volume_path in affected_error_paths:
        return f"{display} (errors detected)"
    return display


def _raw_volume_suffix(raw_volume_path: str) -> str:
    parts = [part for part in raw_volume_path.replace("\\", "/").split("/") if part]
    return parts[-1] if parts else ""


def _disambiguated_volume_name(
    name: str, raw_volume_path: str, duplicate_volume_names: Counter[str]
) -> str:
    if duplicate_volume_names[name] <= 1:
        return name
    suffix = _raw_volume_suffix(raw_volume_path)
    if not suffix:
        return name
    return f"{name} ({suffix})"


def _placement_volume_key(placement: ObjectPlacement) -> tuple[str, str]:
    return (placement.volume_name, placement.raw_volume_path or placement.volume_name)


def _compact_issue_message(issue: ContentTreeIssue) -> str:
    if issue.code == "REL_ACTIVE_PROGRAM_SBNK_MEMBER_TARGET_MISSING":
        text = issue.message.replace(" and are reachable from active Program assignments.", ".")
        return f"active Program path may not load completely: {text}"
    return issue.message


def _compact_program_examples(sampler_path: str, *, limit: int = 2) -> str:
    if "|" not in sampler_path:
        return ""
    raw_examples = [part.strip() for part in sampler_path.split("|", 1)[1].split(";")]
    examples = [part for part in raw_examples if part and not part.startswith("+")]
    more_count = 0
    for part in raw_examples:
        match = re.fullmatch(r"\+(\d+) more", part.strip())
        if match:
            more_count = int(match.group(1))
            break
    hidden_count = max(0, len(examples) - limit) + more_count
    shown = examples[:limit]
    if hidden_count:
        shown.append(f"+{hidden_count} more")
    return "; ".join(shown)


def _validation_content_issues(container: AxklibContainer) -> tuple[ContentTreeIssue, ...]:
    report = validate_container(container)
    return tuple(
        ContentTreeIssue(
            code=issue.code,
            severity=issue.severity.value,
            message=issue.message,
            source_path=issue.source_path,
            sampler_path=issue.sampler_path,
            object_key=issue.object_key,
        )
        for issue in report.issues
        if issue.code in CONTENT_TREE_VALIDATION_CODES
    )


def _issue_display_path_map(container: AxklibContainer) -> dict[str, str]:
    placements, _issues = load_known_object_placements(container)
    volume_paths = {
        (placement.partition_name, placement.volume_name, placement.raw_volume_path)
        for placement in placements.values()
        if placement.raw_volume_path
    }
    duplicate_names_by_partition: dict[str, Counter[str]] = defaultdict(Counter)
    for partition_name, volume_name, _raw_volume_path in volume_paths:
        duplicate_names_by_partition[partition_name][volume_name] += 1

    result: dict[str, str] = {}
    for placement in placements.values():
        if not placement.raw_volume_path:
            continue
        volume_name = _disambiguated_volume_name(
            placement.volume_name,
            placement.raw_volume_path,
            duplicate_names_by_partition[placement.partition_name],
        )
        display = (
            f"{placement.partition_name}/{volume_name}" if placement.partition_name else volume_name
        )
        if display:
            result[placement.raw_volume_path] = display
    return result


def _with_display_issue_paths(
    issues: tuple[ContentTreeIssue, ...], container: AxklibContainer
) -> tuple[ContentTreeIssue, ...]:
    display_by_raw = _issue_display_path_map(container)
    if not display_by_raw:
        return issues
    remapped: list[ContentTreeIssue] = []
    for issue in issues:
        sampler_path = issue.sampler_path
        for raw, display in sorted(
            display_by_raw.items(), key=lambda item: len(item[0]), reverse=True
        ):
            sampler_path = sampler_path.replace(raw, display)
        remapped.append(replace(issue, sampler_path=sampler_path))
    return tuple(remapped)


def _with_validation_issues(tree: ContentTree, container: AxklibContainer) -> ContentTree:
    issues = _with_display_issue_paths(_validation_content_issues(container), container)
    if not issues:
        return tree
    return replace(tree, issues=(*tree.issues, *issues))


@dataclass(frozen=True)
class ContentTreeRenderOptions:
    max_depth: int | None = None
    show_quality: bool = False
    show_unresolved: bool = False


@dataclass(frozen=True)
class ObjectPlacement:
    object_key: str
    partition_index: int | None
    partition_name: str
    volume_name: str
    category_name: str
    entry_name: str
    quality: str
    basis: str
    raw_volume_path: str = ""


@dataclass(frozen=True)
class SfsVolumePlacement:
    partition_index: int | None
    partition_name: str
    volume_name: str
    raw_volume_path: str


def _safe_display(value: object, fallback: str) -> str:
    text = str(value or "").strip()
    return text or fallback


def _program_slot_number(item: AxklibObject) -> int | None:
    text = item.name.strip()
    if len(text) == 3 and text.isdigit():
        value = int(text)
        if 1 <= value <= 128:
            return value
    return None


def _program_display_name(item: AxklibObject, slot_number: int | None) -> str:
    if len(item.payload) >= 0x080:
        decoded = current_parameters.clean_ascii(item.payload[0x078:0x080])
        if decoded:
            return decoded
    if slot_number is not None:
        return f"Pgm {slot_number:03d}"
    return _safe_display(item.name, f"<unnamed PROG {item.object_key}>")


def _program_slot_label(item: AxklibObject) -> str:
    slot_number = _program_slot_number(item)
    if slot_number is None:
        return _safe_display(item.name, f"<unnamed PROG {item.object_key}>")
    return f"{slot_number:03d}: {_program_display_name(item, slot_number)}"


def _program_slot_sort_key(node: ContentNode) -> int:
    if node.node_type != "program_slot" and node.object_type != "PROG":
        return 999
    prefix = node.display_name.split(":", 1)[0].strip()
    if len(prefix) == 3 and prefix.isdigit():
        return int(prefix)
    return 999


def _default_program_slot_node(slot_number: int) -> ContentNode:
    return ContentNode(
        node_id=f"program-slot:{slot_number:03d}:default",
        node_type="program_slot",
        display_name=f"{slot_number:03d}: Pgm {slot_number:03d}",
        quality=DataQuality.KNOWN,
        basis="sampler UI default program slot convention",
    )


def _is_quiet_default_program_node(node: ContentNode) -> bool:
    slot = _program_slot_sort_key(node)
    if not 1 <= slot <= 128:
        return False
    if node.children or node.notes or node.quality != DataQuality.KNOWN:
        return False
    if node.node_type == "program_slot" and not node.object_key:
        return True
    return node.object_type == "PROG" and node.display_name == f"{slot:03d}: Pgm {slot:03d}"


def _with_default_program_slots(nodes: Sequence[ContentNode]) -> tuple[ContentNode, ...]:
    by_slot: dict[int, ContentNode] = {}
    others: list[ContentNode] = []
    for node in nodes:
        slot = _program_slot_sort_key(node)
        if 1 <= slot <= 128:
            by_slot[slot] = node
        else:
            others.append(node)
    if not by_slot:
        return _sort_nodes(nodes)
    result = [by_slot.get(slot, _default_program_slot_node(slot)) for slot in range(1, 129)]
    result.extend(others)
    return tuple(result)


def _program_category_children(
    nodes: Sequence[ContentNode], *, include_default_programs: bool
) -> tuple[ContentNode, ...]:
    if include_default_programs:
        return _with_default_program_slots(nodes)
    return tuple(node for node in _sort_nodes(nodes) if not _is_quiet_default_program_node(node))


def _sort_nodes(nodes: Iterable[ContentNode]) -> tuple[ContentNode, ...]:
    return tuple(
        sorted(
            nodes,
            key=lambda node: (
                0 if node.node_type == UNRESOLVED_PROGRAM_ASSIGNMENT_NODE else 1,
                CATEGORY_ORDER.get(node.display_name, 99),
                _program_slot_sort_key(node),
                node.display_name.lower(),
                node.object_type,
                node.object_key,
            ),
        )
    )


def _selector_component(node: ContentNode) -> str:
    if node.node_type == "partition":
        partition_index: int | None = None
        raw_index = node.node_id.split(":", 1)[1] if ":" in node.node_id else ""
        if raw_index and raw_index != "None":
            try:
                partition_index = int(raw_index)
            except ValueError:
                partition_index = None
        partition_name = node.display_name
        prefix = f"partition {partition_index}: " if partition_index is not None else ""
        if prefix and partition_name.startswith(prefix):
            partition_name = partition_name[len(prefix) :]
        if partition_index is None:
            return _safe_display_path_name(partition_name, "partition")
        safe_name = re.sub(
            r"[^A-Za-z0-9._ -]+",
            "_",
            partition_name.strip() or f"partition_{partition_index:02d}",
        )
        safe_name = re.sub(r"\s+", "_", safe_name).strip("._-")
        return f"partition_{partition_index:02d}_{safe_name or f'partition_{partition_index:02d}'}"
    if node.node_type == "volume":
        name = node.display_name.removesuffix(" (errors detected)")
        return _safe_display_path_name(name, "volume")
    return node.display_name.replace("/", "_").replace("\\", "_").strip() or node.node_type


def _join_selector_path(parent: str, component: str) -> str:
    if not parent:
        return component
    return f"{parent}/{component}"


def _with_selector_paths_for_node(node: ContentNode, parent: str = "") -> ContentNode:
    selector_path = _join_selector_path(parent, _selector_component(node))
    children = tuple(_with_selector_paths_for_node(child, selector_path) for child in node.children)
    return replace(node, selector_path=selector_path, children=children)


def _with_selector_paths(tree: ContentTree) -> ContentTree:
    return replace(tree, roots=tuple(_with_selector_paths_for_node(root) for root in tree.roots))


def _path_scope(node: ContentNode) -> str:
    if node.node_type == "volume":
        return "volume"
    if node.object_type == "PROG":
        return "program"
    if node.object_type == "SBAC":
        return "sbac"
    if node.object_type == "SBNK":
        return "sbnk"
    return ""


def content_tree_path_rows(tree: ContentTree) -> tuple[ContentTreePathRow, ...]:
    rows: list[ContentTreePathRow] = []

    def visit(node: ContentNode) -> None:
        scope = _path_scope(node)
        if scope and node.selector_path:
            rows.append(
                ContentTreePathRow(
                    source_path=tree.source_path,
                    scope=scope,
                    path=node.selector_path,
                    display_name=node.display_name,
                    node_type=node.node_type,
                    object_type=node.object_type,
                    object_key=node.object_key,
                )
            )
        for child in node.children:
            visit(child)

    for root in tree.roots:
        visit(root)
    return tuple(rows)


def render_content_tree_paths(tree: ContentTree) -> str:
    lines = ["source_path\tscope\tpath\tdisplay_name\tobject_type\tobject_key"]
    for row in content_tree_path_rows(tree):
        lines.append(
            "\t".join(
                (
                    row.source_path,
                    row.scope,
                    row.path,
                    row.display_name,
                    row.object_type,
                    row.object_key,
                )
            )
        )
    return "\n".join(lines)


def _with_count(
    node_type: str, name: str, children: Sequence[ContentNode], node_id: str
) -> ContentNode:
    return ContentNode(
        node_id=node_id,
        node_type=node_type,
        display_name=name,
        count=len(children),
        children=_sort_nodes(children),
    )


def _object_node(
    *,
    node_id: str,
    object_type: str,
    display_name: str,
    object_key: str,
    quality: str = "Known",
    basis: str = "container object metadata",
    notes: str = "",
    details: tuple[str, ...] = (),
    children: tuple[ContentNode, ...] = (),
) -> ContentNode:
    conf = DataQuality.KNOWN
    for item in DataQuality:
        if quality == item.value:
            conf = item
            break
    return ContentNode(
        node_id=node_id,
        node_type=NODE_LABEL.get(object_type, "object"),
        display_name=display_name,
        object_key=object_key,
        object_type=object_type,
        details=details,
        quality=conf,
        basis=basis,
        notes=notes,
        children=children,
    )


def _load_sfs_placements(
    container: AxklibContainer,
) -> tuple[dict[int, ObjectPlacement], tuple[ContentTreeIssue, ...]]:
    if container.kind != "sfs":
        return {}, ()
    try:
        with tempfile.TemporaryDirectory(prefix="axklib-content-tree-") as tmp:
            inventory_dir = Path(tmp) / "inventory"
            sfs_inventory.build_inventory(container.source_path, inventory_dir)
            path = inventory_dir / "volume_objects.csv"
            placements: dict[int, ObjectPlacement] = {}
            with path.open("r", newline="", encoding="utf-8") as handle:
                for row in csv.DictReader(handle):
                    object_offset_text = row.get("object_offset", "")
                    if not object_offset_text:
                        continue
                    try:
                        object_offset = int(object_offset_text, 0)
                    except ValueError:
                        continue
                    try:
                        partition = int(row.get("partition_index", ""))
                    except ValueError:
                        partition = None
                    placements[object_offset] = ObjectPlacement(
                        object_key="",
                        partition_index=partition,
                        partition_name=row.get("partition_name", ""),
                        volume_name=row.get("volume_name", ""),
                        category_name=row.get("category_name", ""),
                        entry_name=row.get("entry_name", ""),
                        quality=row.get("match_quality", ""),
                        basis=row.get("match_method", "SFS volume inventory"),
                        raw_volume_path=row.get("volume_name", ""),
                    )
            return placements, ()
    except Exception as exc:
        return {}, (
            ContentTreeIssue(
                code="CONTENT_TREE_SFS_INVENTORY_FAILED",
                severity="warning",
                message=f"SFS volume/category inventory was unavailable: {exc}",
                source_path=str(container.source_path),
            ),
        )


def _load_sfs_volume_placements(
    container: AxklibContainer,
) -> tuple[tuple[SfsVolumePlacement, ...], tuple[ContentTreeIssue, ...]]:
    if container.kind != "sfs":
        return (), ()
    try:
        with tempfile.TemporaryDirectory(prefix="axklib-content-tree-") as tmp:
            inventory_dir = Path(tmp) / "inventory"
            sfs_inventory.build_inventory(container.source_path, inventory_dir)
            path = inventory_dir / "volumes.csv"
            volumes: list[SfsVolumePlacement] = []
            with path.open("r", newline="", encoding="utf-8") as handle:
                for row in csv.DictReader(handle):
                    try:
                        partition = int(row.get("partition_index", ""))
                    except ValueError:
                        partition = None
                    volume_name = row.get("volume_name", "")
                    volume_path = row.get("volume_path", "").strip("/")
                    if not volume_name:
                        continue
                    volumes.append(
                        SfsVolumePlacement(
                            partition_index=partition,
                            partition_name=row.get("partition_name", ""),
                            volume_name=volume_name,
                            raw_volume_path=volume_path or volume_name,
                        )
                    )
            return tuple(volumes), ()
    except Exception as exc:
        return (), (
            ContentTreeIssue(
                code="CONTENT_TREE_SFS_INVENTORY_FAILED",
                severity="warning",
                message=f"SFS volume/category inventory was unavailable: {exc}",
                source_path=str(container.source_path),
            ),
        )


def _iso_raw_group_and_volume(item: AxklibObject) -> tuple[str, str]:
    parts = [part for part in item.fat_file.replace("\\", "/").split("/") if part]
    group = parts[0] if len(parts) >= 1 else ""
    volume = parts[1] if len(parts) >= 2 else ""
    return group, volume


def _metadata_string(item: AxklibObject, key: str) -> str:
    value = item.metadata.get(key)
    return value if isinstance(value, str) else ""


def _iso_group_label(item: AxklibObject, raw_group: str) -> str:
    return _metadata_string(item, "iso_group_label") or raw_group or "ISO objects"


def _display_name_for_iso_volume_fallback(item: AxklibObject) -> str:
    if item.type == "PROG":
        slot = _program_slot_number(item)
        name = _program_display_name(item, slot)
        if slot is not None and name == f"Pgm {slot:03d}":
            return ""
        return _safe_display(name, "")
    if item.type in {"SBAC", "SBNK", "SMPL", "SEQU"}:
        return _safe_display(item.name, "")
    return ""


def _iso_content_derived_volume_labels(
    container: AxklibContainer,
) -> dict[tuple[str, str], str]:
    by_volume: dict[tuple[str, str], list[AxklibObject]] = defaultdict(list)
    for item in container.objects:
        raw_group, raw_volume = _iso_raw_group_and_volume(item)
        if not raw_group or not raw_volume or _metadata_string(item, "iso_volume_label"):
            continue
        by_volume[(raw_group, raw_volume)].append(item)

    preferred_type_order = {"PROG": 0, "SBAC": 1, "SBNK": 2, "SMPL": 3, "SEQU": 4}
    raw_labels: dict[tuple[str, str], str] = {}
    for key, items in by_volume.items():
        for item in sorted(
            items,
            key=lambda obj: (
                preferred_type_order.get(obj.type, 99),
                obj.name.lower(),
                obj.object_key,
            ),
        ):
            label = _display_name_for_iso_volume_fallback(item)
            if label:
                raw_labels[key] = label
                break

    used: dict[str, set[str]] = defaultdict(set)
    labels: dict[tuple[str, str], str] = {}
    for (raw_group, raw_volume), label in sorted(raw_labels.items()):
        normalized = label.lower()
        if normalized in used[raw_group]:
            label = f"{label} ({raw_volume})"
            normalized = label.lower()
        used[raw_group].add(normalized)
        labels[(raw_group, raw_volume)] = label
    return labels


def _iso_volume_label_and_source(
    item: AxklibObject,
    raw_group: str,
    raw_volume: str,
    content_labels: dict[tuple[str, str], str] | None = None,
) -> tuple[str, str]:
    known = _metadata_string(item, "iso_volume_label")
    if known:
        return known, "ISO Yamaha CD-ROM menu label metadata"
    if content_labels:
        derived = content_labels.get((raw_group, raw_volume))
        if derived:
            return derived, "ISO directory path plus content-derived volume label fallback"
    return (
        raw_volume or _iso_group_label(item, raw_group),
        "ISO directory path raw volume label",
    )


def _non_sfs_object_placement(
    container: AxklibContainer,
    item: AxklibObject,
    *,
    iso_content_volume_labels: dict[tuple[str, str], str] | None = None,
) -> ObjectPlacement:
    if container.kind == "fat12_floppy":
        partition_name = ""
        volume_name = "FAT root"
        quality = "fat12_floppy container object metadata"
    elif container.kind == "iso":
        raw_group, raw_volume = _iso_raw_group_and_volume(item)
        partition_name = _iso_group_label(item, raw_group)
        volume_name, quality = _iso_volume_label_and_source(
            item,
            raw_group,
            raw_volume,
            iso_content_volume_labels,
        )
    else:
        partition_name = ""
        volume_name = "Standalone object"
        quality = f"{container.kind} container object metadata"
    return ObjectPlacement(
        object_key=item.object_key,
        partition_index=None,
        partition_name=partition_name,
        volume_name=volume_name,
        category_name="Sample Banks"
        if item.type == "SBAC"
        else TYPE_CATEGORY.get(item.type, item.type),
        entry_name=item.name,
        quality="Known",
        basis=quality,
        raw_volume_path=f"{raw_group}/{raw_volume}" if container.kind == "iso" else volume_name,
    )


def load_known_object_placements(
    container: AxklibContainer,
) -> tuple[dict[str, ObjectPlacement], tuple[ContentTreeIssue, ...]]:
    if container.kind != "sfs":
        iso_content_volume_labels = (
            _iso_content_derived_volume_labels(container) if container.kind == "iso" else None
        )
        return {
            item.object_key: _non_sfs_object_placement(
                container,
                item,
                iso_content_volume_labels=iso_content_volume_labels,
            )
            for item in container.objects
        }, ()
    placements_by_offset, issues = _load_sfs_placements(container)
    if not placements_by_offset:
        return {}, issues
    result: dict[str, ObjectPlacement] = {}
    for item in container.objects:
        if item.payload_offset is None:
            continue
        placement = placements_by_offset.get(item.payload_offset)
        if placement is None or placement.quality != "Known":
            continue
        result[item.object_key] = ObjectPlacement(
            object_key=item.object_key,
            partition_index=placement.partition_index,
            partition_name=placement.partition_name,
            volume_name=placement.volume_name,
            category_name=placement.category_name,
            entry_name=placement.entry_name,
            quality=placement.quality,
            basis=placement.basis,
        )
    return result, issues


def _relationship_targets(value: str) -> tuple[str, ...]:
    return tuple(part for part in value.split("|") if part)


_NAVIGABLE_RELATIONSHIP_TYPES = {
    "PROG_ASSIGNMENT_TO_SBAC",
    "PROG_ASSIGNMENT_TO_SBNK",
    "PROG_ASSIGNMENT_TO_OBJECT",
    "SBAC_SLOT_TO_SBNK",
    "SBNK_LEFT_MEMBER_TO_SMPL",
    "SBNK_RIGHT_MEMBER_TO_SMPL",
}


def _relationship_sort_key(row: Relationship) -> tuple[str, str, str, str]:
    return (row.relationship_type, row.quality, row.target_key, row.key)


def _is_active_program_assignment(row: Relationship) -> bool:
    if not row.relationship_type.startswith("PROG_ASSIGNMENT_TO_"):
        return True
    if not row.active_assignment_state:
        return True
    return row.active_assignment_state in {
        ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE,
        ACTIVE_ASSIGNMENT_STATE_SOURCE_LOAD_ASSIGNMENT,
    }


def _program_assignment_details(row: Relationship | None) -> tuple[str, ...]:
    if row is None or not row.relationship_type.startswith("PROG_ASSIGNMENT_TO_"):
        return ()
    if row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_SOURCE_LOAD_ASSIGNMENT:
        return ("Rch Assign: =SMP",)
    if row.active_assignment_state != ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE:
        return ()
    value = row.assignment_rch_assign_display.strip()
    if not value or value.lower() in {"off", "unknown"}:
        return ()
    return (f"Rch Assign: {value}",)


def _is_navigable_relationship(row: Relationship) -> bool:
    if row.relationship_type not in _NAVIGABLE_RELATIONSHIP_TYPES:
        return False
    if not _is_active_program_assignment(row):
        return False
    if row.relationship_type.startswith("PROG_ASSIGNMENT_TO_"):
        return row.quality in {"Known", "Likely"}
    return row.quality == "Known"


def _is_active_missing_local_target(row: Relationship) -> bool:
    return (
        row.relationship_type.startswith("PROG_ASSIGNMENT_TO_")
        and row.active_assignment_state == ACTIVE_ASSIGNMENT_STATE_CONFIRMED_ACTIVE
        and row.quality == DataQuality.UNKNOWN.value
        and row.basis == ACTIVE_MISSING_LOCAL_TARGET_BASIS
    )


def _unresolved_program_assignment_node(row: Relationship) -> ContentNode:
    fallback_name = row.target_key
    if not fallback_name:
        fallback_name = (
            f"assignment {row.assignment_index + 1}"
            if row.assignment_index is not None
            else "assignment"
        )
    name = _safe_display(row.assignment_name, fallback_name)
    return ContentNode(
        node_id=f"relationship:{row.key}:missing-local-target",
        node_type=UNRESOLVED_PROGRAM_ASSIGNMENT_NODE,
        display_name=name,
        object_key=row.target_key,
        object_type="SBNK",
        details=_program_assignment_details(row),
        quality=DataQuality.UNKNOWN,
        basis=row.basis,
        notes=ACTIVE_MISSING_LOCAL_TARGET_NOTE,
    )


def _has_known_sbac_parent(item: AxklibObject, graph: RelationshipGraph) -> bool:
    return any(
        row.relationship_type == "SBAC_SLOT_TO_SBNK" and row.quality == "Known"
        for row in graph.parents(item.object_key)
    )


def _sbnk_visible_name(item: AxklibObject) -> str:
    return _safe_display(item.name, f"<unnamed SBNK {item.object_key}>")


def _sampler_sample_node(
    item: AxklibObject, relationship: Relationship | None = None
) -> ContentNode:
    return _object_node(
        node_id=f"object:{item.object_key}",
        object_type=item.type,
        display_name=_sbnk_visible_name(item),
        object_key=item.object_key,
        quality=relationship.quality if relationship else "Known",
        basis=relationship.basis if relationship else "container object metadata",
        notes=relationship.ambiguity_notes if relationship else "",
        details=_program_assignment_details(relationship),
    )


def _sampler_sample_bank_group_node(
    item: AxklibObject,
    *,
    graph: RelationshipGraph,
    object_by_key: dict[str, AxklibObject],
    relationship: Relationship | None = None,
    with_children: bool = True,
) -> ContentNode:
    children: list[ContentNode] = []
    if with_children:
        for child_row in sorted(graph.children(item.object_key), key=_relationship_sort_key):
            if child_row.relationship_type != "SBAC_SLOT_TO_SBNK" or not _is_navigable_relationship(
                child_row
            ):
                continue
            child_target = object_by_key.get(child_row.target_key)
            if child_target is None or child_target.type != "SBNK":
                children.append(
                    ContentNode(
                        node_id=f"relationship:{child_row.key}:{child_row.target_key}",
                        node_type="relationship_target",
                        display_name=child_row.target_key,
                        object_key=child_row.target_key,
                        details=_program_assignment_details(child_row),
                        quality=DataQuality.UNKNOWN,
                        basis=child_row.basis,
                        notes=child_row.ambiguity_notes,
                    )
                )
                continue
            children.append(_sampler_sample_node(child_target, child_row))

    quality = DataQuality.KNOWN
    if relationship is not None:
        for item_quality in DataQuality:
            if relationship.quality == item_quality.value:
                quality = item_quality
                break

    return ContentNode(
        node_id=f"object:{item.object_key}",
        node_type="sample_bank",
        display_name=f"B {_safe_display(item.name, f'<unnamed SBAC {item.object_key}>')}",
        object_key=item.object_key,
        object_type="SBAC",
        details=_program_assignment_details(relationship),
        quality=quality,
        basis=relationship.basis if relationship else "current SBAC slot relationships",
        children=_sort_nodes(children),
    )


def _relationship_child_nodes(
    *,
    relationship: Relationship,
    graph: RelationshipGraph,
    object_by_key: dict[str, AxklibObject],
    depth: int,
    visited: frozenset[str],
    include_unresolved: bool,
) -> list[ContentNode]:
    child_nodes: list[ContentNode] = []
    for target_key in _relationship_targets(relationship.target_key):
        target = object_by_key.get(target_key)
        if (
            relationship.relationship_type == "PROG_ASSIGNMENT_TO_SBAC"
            and target is not None
            and target.type == "SBAC"
        ):
            child_nodes.append(
                _sampler_sample_bank_group_node(
                    target,
                    graph=graph,
                    object_by_key=object_by_key,
                    relationship=relationship,
                    with_children=False,
                )
            )
            continue
        if relationship.relationship_type.startswith("PROG_ASSIGNMENT_TO_") and target is not None:
            child_nodes.append(
                _relationship_object_node(
                    item=target,
                    graph=graph,
                    object_by_key=object_by_key,
                    relationship=relationship,
                    depth=0,
                    visited=visited,
                    include_unresolved=include_unresolved,
                )
            )
            continue
        child_nodes.append(
            _node_for_relationship_target(
                target_key=target_key,
                relationship=relationship,
                graph=graph,
                object_by_key=object_by_key,
                depth=depth,
                visited=visited,
                include_unresolved=include_unresolved,
            )
        )
    return child_nodes


def _node_for_relationship_target(
    *,
    target_key: str,
    relationship: Relationship,
    graph: RelationshipGraph,
    object_by_key: dict[str, AxklibObject],
    depth: int,
    visited: frozenset[str],
    include_unresolved: bool,
) -> ContentNode:
    target = object_by_key.get(target_key)
    if target is None:
        return ContentNode(
            node_id=f"relationship:{relationship.key}:{target_key}",
            node_type="relationship_target",
            display_name=target_key,
            object_key=target_key,
            details=_program_assignment_details(relationship),
            quality=DataQuality.TENTATIVE
            if relationship.quality == "Tentative"
            else DataQuality.UNKNOWN,
            basis=relationship.basis,
            notes=relationship.ambiguity_notes,
        )
    return _relationship_object_node(
        item=target,
        graph=graph,
        object_by_key=object_by_key,
        relationship=relationship,
        depth=depth,
        visited=visited,
        include_unresolved=include_unresolved,
    )


def _relationship_object_node(
    *,
    item: AxklibObject,
    graph: RelationshipGraph,
    object_by_key: dict[str, AxklibObject],
    relationship: Relationship | None,
    depth: int,
    visited: frozenset[str],
    include_unresolved: bool,
) -> ContentNode:
    object_key = item.object_key
    object_type = item.type
    display_name = (
        _program_slot_label(item)
        if object_type == "PROG"
        else _safe_display(item.name, f"<unnamed {object_type} {object_key}>")
    )
    children: tuple[ContentNode, ...] = ()
    if depth > 0 and object_key not in visited:
        next_visited = frozenset((*visited, object_key))
        child_nodes: list[ContentNode] = []
        for row in sorted(graph.children(object_key), key=_relationship_sort_key):
            if include_unresolved and _is_active_missing_local_target(row):
                child_nodes.append(_unresolved_program_assignment_node(row))
                continue
            if not _is_navigable_relationship(row):
                continue
            child_nodes.extend(
                _relationship_child_nodes(
                    relationship=row,
                    graph=graph,
                    object_by_key=object_by_key,
                    depth=depth - 1,
                    visited=next_visited,
                    include_unresolved=include_unresolved,
                )
            )
        children = _sort_nodes(child_nodes)
    quality = relationship.quality if relationship else "Known"
    basis = relationship.basis if relationship else "container object metadata"
    notes = relationship.ambiguity_notes if relationship else ""
    return _object_node(
        node_id=f"object:{object_key}",
        object_type=object_type,
        display_name=display_name,
        object_key=object_key,
        quality=quality,
        basis=basis,
        notes=notes,
        details=_program_assignment_details(relationship),
        children=children,
    )


def _build_sfs_tree(
    container: AxklibContainer,
    *,
    include_unresolved: bool = False,
    include_default_programs: bool = False,
) -> ContentTree:
    placements, issues = load_known_object_placements(container)
    volume_placements, volume_issues = _load_sfs_volume_placements(container)
    issues = (*issues, *volume_issues)
    validation_issues = _validation_content_issues(container)
    affected_error_paths = _affected_error_volume_paths(validation_issues)
    graph = build_relationship_graph(list(container.objects))
    object_by_key = {item.object_key: item for item in container.objects}
    partition_children: dict[
        tuple[int | None, str], dict[tuple[str, str], dict[str, list[ContentNode]]]
    ] = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    unmapped: dict[str, list[ContentNode]] = defaultdict(list)

    for volume in volume_placements:
        pkey = (volume.partition_index, volume.partition_name)
        partition_children[pkey][(volume.volume_name, volume.raw_volume_path)]

    for item in container.objects:
        if item.type != "SBAC":
            continue
        node = _sampler_sample_bank_group_node(item, graph=graph, object_by_key=object_by_key)
        placement = placements.get(item.object_key)
        if placement is None:
            for row in sorted(graph.children(item.object_key), key=_relationship_sort_key):
                if row.relationship_type != "SBAC_SLOT_TO_SBNK" or not _is_navigable_relationship(
                    row
                ):
                    continue
                placement = placements.get(row.target_key)
                if placement is not None:
                    break
        if placement is not None:
            pkey = (placement.partition_index, placement.partition_name)
            partition_children[pkey][_placement_volume_key(placement)]["Sample Banks"].append(node)
        elif node.children:
            unmapped["Sample Banks"].append(
                ContentNode(
                    node_id=node.node_id,
                    node_type=node.node_type,
                    display_name=node.display_name,
                    object_key=node.object_key,
                    object_type=node.object_type,
                    details=node.details,
                    quality=DataQuality.UNKNOWN,
                    basis=node.basis,
                    notes="no-known-volume",
                    children=node.children,
                )
            )

    for item in container.objects:
        if item.type in INTERNAL_OBJECT_TYPES:
            continue
        if item.type == "SBNK" and _has_known_sbac_parent(item, graph):
            continue
        category = TYPE_CATEGORY.get(item.type, item.type)
        placement = placements.get(item.object_key)
        if item.type == "SBNK":
            node = _sampler_sample_node(item)
        else:
            node = _relationship_object_node(
                item=item,
                graph=graph,
                object_by_key=object_by_key,
                relationship=None,
                depth=3,
                visited=frozenset(),
                include_unresolved=include_unresolved,
            )
        if placement is not None:
            pkey = (placement.partition_index, placement.partition_name)
            display_category = TYPE_CATEGORY.get(item.type, placement.category_name)
            partition_children[pkey][_placement_volume_key(placement)][display_category].append(
                node
            )
        else:
            unmapped[category].append(
                _object_node(
                    node_id=f"object:{item.object_key}",
                    object_type=item.type,
                    display_name=_program_slot_label(item)
                    if item.type == "PROG"
                    else _safe_display(item.name, f"<unnamed {item.type} {item.object_key}>"),
                    object_key=item.object_key,
                    details=node.details,
                    quality="Unknown",
                    basis="container object metadata",
                    notes="no-known-volume",
                    children=node.children,
                )
            )

    roots: list[ContentNode] = []
    for (partition_index, partition_name), volumes in sorted(
        partition_children.items(),
        key=lambda row: ((row[0][0] if row[0][0] is not None else 999), row[0][1].lower()),
    ):
        volume_nodes: list[ContentNode] = []
        duplicate_volume_names = Counter(volume_name for volume_name, _raw_path in volumes)
        for (volume_name, raw_volume_path), categories in sorted(
            volumes.items(), key=lambda row: (row[0][0].lower(), row[0][1].lower())
        ):
            category_nodes: list[ContentNode] = []
            volume_display_name = _disambiguated_volume_name(
                volume_name, raw_volume_path, duplicate_volume_names
            )
            volume_node_id = raw_volume_path or volume_name
            for category_name, objects in categories.items():
                children = (
                    _program_category_children(
                        objects, include_default_programs=include_default_programs
                    )
                    if category_name == "Programs"
                    else _sort_nodes(objects)
                )
                if not children:
                    continue
                category_nodes.append(
                    _with_count(
                        "category",
                        category_name,
                        children,
                        f"category:{partition_index}:{volume_node_id}:{category_name}",
                    )
                )
            volume_nodes.append(
                _with_count(
                    "volume",
                    _display_volume_name(
                        volume_display_name,
                        raw_volume_path,
                        affected_error_paths,
                    ),
                    category_nodes,
                    f"volume:{partition_index}:{volume_node_id}",
                )
            )
        part_name = partition_name or (
            f"partition {partition_index}" if partition_index is not None else "partition"
        )
        if volume_nodes:
            roots.append(
                _with_count(
                    "partition",
                    f"partition {partition_index}: {part_name}"
                    if partition_index is not None
                    else part_name,
                    volume_nodes,
                    f"partition:{partition_index}",
                )
            )

    if unmapped:
        category_nodes = []
        for category_name, objects in unmapped.items():
            children = (
                _program_category_children(
                    objects, include_default_programs=include_default_programs
                )
                if category_name == "Programs"
                else _sort_nodes(objects)
            )
            if not children:
                continue
            category_nodes.append(
                _with_count("category", category_name, children, f"unmapped:{category_name}")
            )
        if category_nodes:
            roots.append(_with_count("unresolved", "_unmapped", category_nodes, "unmapped"))

    return ContentTree(
        source_path=str(container.source_path),
        container_kind=container.kind,
        detected_format=container.detected_format,
        roots=_sort_nodes(roots),
        issues=issues,
    )


def _build_scope_tree(
    container: AxklibContainer,
    scope_label: str,
    *,
    include_unresolved: bool = False,
    include_default_programs: bool = False,
) -> ContentTree:
    graph = build_relationship_graph(list(container.objects))
    object_by_key = {item.object_key: item for item in container.objects}
    groups: dict[str, list[ContentNode]] = defaultdict(list)
    recovery: dict[str, list[ContentNode]] = defaultdict(list)

    for item in container.objects:
        if item.type == "SBAC":
            node = _sampler_sample_bank_group_node(item, graph=graph, object_by_key=object_by_key)
            if node.children:
                groups["Sample Banks"].append(node)

    for item in container.objects:
        if item.type in INTERNAL_OBJECT_TYPES:
            continue
        if item.type == "SBNK" and _has_known_sbac_parent(item, graph):
            continue
        quality = str(item.metadata.get("iso_recovery_quality", ""))
        category = TYPE_CATEGORY.get(item.type, item.type)
        node = (
            _sampler_sample_node(item)
            if item.type == "SBNK"
            else _relationship_object_node(
                item=item,
                graph=graph,
                object_by_key=object_by_key,
                relationship=None,
                depth=3,
                visited=frozenset(),
                include_unresolved=include_unresolved,
            )
        )
        if quality == "raw-scan-impossible-internal-capacity":
            recovery[category].append(
                _object_node(
                    node_id=node.node_id,
                    object_type=item.type,
                    display_name=node.display_name,
                    object_key=item.object_key,
                    details=node.details,
                    quality="Likely",
                    basis=str(item.quality.source),
                    notes=quality,
                    children=node.children,
                )
            )
        else:
            groups[category].append(node)

    category_nodes: list[ContentNode] = []
    for category_name, objects in groups.items():
        children = (
            _program_category_children(objects, include_default_programs=include_default_programs)
            if category_name == "Programs"
            else _sort_nodes(objects)
        )
        if not children:
            continue
        category_nodes.append(
            _with_count("category", category_name, children, f"category:{category_name}")
        )
    if recovery:
        recovery_categories = [
            _with_count("category", category_name, objects, f"recovery:{category_name}")
            for category_name, objects in recovery.items()
        ]
        category_nodes.append(
            _with_count("recovery_artifact", "Recovery artifacts", recovery_categories, "recovery")
        )

    root = _with_count("volume", scope_label, category_nodes, f"scope:{scope_label}")
    return ContentTree(
        source_path=str(container.source_path),
        container_kind=container.kind,
        detected_format=container.detected_format,
        roots=(root,),
    )


def build_content_tree_for_container(
    container: AxklibContainer,
    *,
    include_unresolved: bool = False,
    include_default_programs: bool = False,
    include_validation: bool = True,
) -> ContentTree:
    if container.kind in {"sfs", "iso"}:
        tree = _build_sfs_tree(
            container,
            include_unresolved=include_unresolved,
            include_default_programs=include_default_programs,
        )
    elif container.kind == "fat12_floppy":
        tree = _build_scope_tree(
            container,
            "FAT root",
            include_unresolved=include_unresolved,
            include_default_programs=include_default_programs,
        )
    else:
        tree = _build_scope_tree(
            container,
            "Standalone object",
            include_unresolved=include_unresolved,
            include_default_programs=include_default_programs,
        )
    if include_validation:
        tree = _with_validation_issues(tree, container)
    return _with_selector_paths(tree)


def build_content_trees_for_paths(
    paths: Sequence[str | Path],
    options: OpenOptions | None = None,
    include_unresolved: bool = False,
    include_default_programs: bool = False,
    include_validation: bool = True,
) -> ContentTreeLoadResult:
    opts = options or OpenOptions(include_payloads=False)
    results = open_many(paths, options=opts)
    trees: list[ContentTree] = []
    errors: list[AxklibContainerLoadResult] = []
    for result in results:
        if result.container is None:
            errors.append(result)
        else:
            trees.append(
                build_content_tree_for_container(
                    result.container,
                    include_unresolved=include_unresolved,
                    include_default_programs=include_default_programs,
                    include_validation=include_validation,
                )
            )
    return ContentTreeLoadResult(trees=tuple(trees), load_errors=tuple(errors))


def _node_type_suffix(node: ContentNode) -> str:
    if node.node_type in {UNRESOLVED_PROGRAM_ASSIGNMENT_NODE, "relationship_target"}:
        return " [UNKNOWN]"
    label = TYPE_LABEL_BY_NODE.get(node.node_type) or TYPE_LABEL_BY_OBJECT.get(node.object_type)
    return f" [{label}]" if label else ""


def _node_suffix(node: ContentNode, options: ContentTreeRenderOptions) -> str:
    parts: list[str] = []
    if node.count is not None:
        parts.append(f"({node.count})")
    if node.details:
        parts.append(f"- {'; '.join(node.details)}")
    if options.show_quality or node.quality != DataQuality.KNOWN:
        parts.append(f"[{node.quality.value}]")
    if node.notes and node.quality in {DataQuality.UNKNOWN, DataQuality.TENTATIVE}:
        parts.append(f"- {node.notes}")
    return " " + " ".join(parts) if parts else ""


def _should_render_node(node: ContentNode, options: ContentTreeRenderOptions, depth: int) -> bool:
    if node.node_type == UNRESOLVED_PROGRAM_ASSIGNMENT_NODE and not options.show_unresolved:
        return False
    return options.max_depth is None or depth <= options.max_depth


def _render_node(
    node: ContentNode,
    lines: list[str],
    options: ContentTreeRenderOptions,
    depth: int,
    prefix: str,
    is_last: bool,
) -> None:
    if not _should_render_node(node, options, depth):
        return
    connector = "`-- " if is_last else "|-- "
    lines.append(
        f"{prefix}{connector}{node.display_name}{_node_type_suffix(node)}{_node_suffix(node, options)}"
    )
    child_prefix = f"{prefix}{'    ' if is_last else '|   '}"
    children = [child for child in node.children if _should_render_node(child, options, depth + 1)]
    for index, child in enumerate(children):
        _render_node(child, lines, options, depth + 1, child_prefix, index == len(children) - 1)


def render_content_tree_text(
    tree: ContentTree, options: ContentTreeRenderOptions | None = None
) -> str:
    opts = options or ContentTreeRenderOptions()
    lines = [f"{tree.source_path} [{tree.container_kind}]"]
    for issue in tree.issues:
        volume_path = _issue_volume_path(issue)
        lines.append(f"{issue.severity}: {issue.code}: {_compact_issue_message(issue)}")
        if volume_path:
            lines.append(f"  volume: {volume_path}")
        examples = _compact_program_examples(issue.sampler_path)
        if examples:
            lines.append(f"  active Program examples: {examples}")
    if tree.issues:
        lines.append("")
    roots = [root for root in tree.roots if _should_render_node(root, opts, 1)]
    for index, root in enumerate(roots):
        _render_node(root, lines, opts, 1, "", index == len(roots) - 1)
    return "\n".join(lines)


def content_tree_to_json(tree: ContentTree) -> dict[str, object]:
    return cast(dict[str, object], to_plain(tree))


def summary_line(container: AxklibContainer) -> str:
    counts = Counter(item.type for item in container.objects)
    object_text = (
        "objects=0"
        if not counts
        else " ".join(
            [
                f"objects={len(container.objects)}",
                *[f"{key}={counts[key]}" for key in sorted(counts)],
            ]
        )
    )
    recovery = ",".join(
        f"{key}:{value}" for key, value in sorted(container.recovery_quality_summary.items())
    )
    return f"{container.source_path}\t{container.kind}\t{object_text}\trecovery={recovery or '-'}"
