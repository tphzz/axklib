"""User-facing content tree views for axklib containers."""

from __future__ import annotations

import csv
import json
import tempfile
from collections import Counter, defaultdict
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import cast

from axklib.containers import (
    AxklibContainer,
    AxklibContainerLoadResult,
    OpenOptions,
    open_many,
    sfs_inventory,
)
from axklib.model import AxklibObject, DataQuality
from axklib.parameters import current as current_parameters
from axklib.relationships import Relationship, RelationshipGraph, build_relationship_graph
from axklib.reports import to_plain

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


@dataclass(frozen=True)
class ContentTreeIssue:
    code: str
    severity: str
    message: str
    source_path: str


@dataclass(frozen=True)
class ContentNode:
    node_id: str
    node_type: str
    display_name: str
    object_key: str = ""
    object_type: str = ""
    count: int | None = None
    quality: DataQuality = DataQuality.KNOWN
    basis: str = ""
    notes: str = ""
    children: tuple[ContentNode, ...] = ()


@dataclass(frozen=True)
class ContentTree:
    source_path: str
    container_kind: str
    detected_format: str
    roots: tuple[ContentNode, ...]
    issues: tuple[ContentTreeIssue, ...] = ()


@dataclass(frozen=True)
class ContentTreeLoadResult:
    trees: tuple[ContentTree, ...]
    load_errors: tuple[AxklibContainerLoadResult, ...]


@dataclass(frozen=True)
class ContentTreeRenderOptions:
    max_depth: int | None = None
    show_quality: bool = False
    show_unresolved: bool = False


@dataclass(frozen=True)
class ContentLabelMap:
    iso_group_labels: Mapping[tuple[str, str], str]
    iso_volume_labels: Mapping[tuple[str, str, str], str]

    def iso_group_label(self, source_key: str, raw_group: str) -> str | None:
        return self.iso_group_labels.get((source_key, raw_group))

    def iso_volume_label(self, source_key: str, raw_group: str, raw_volume: str) -> str | None:
        return self.iso_volume_labels.get((source_key, raw_group, raw_volume))


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


def _label_entry_string(entry: object, key: str) -> str:
    if not isinstance(entry, dict):
        raise ValueError("content label entries must be objects")
    value = entry.get(key)
    if not isinstance(value, str):
        raise ValueError(f"content label entry missing string field: {key}")
    return value


def _label_entries(payload: Mapping[str, object], key: str) -> list[object]:
    value = payload.get(key, [])
    if not isinstance(value, list):
        raise ValueError(f"content label map field must be a list: {key}")
    return value


def content_label_map_from_dict(payload: Mapping[str, object]) -> ContentLabelMap:
    group_labels: dict[tuple[str, str], str] = {}
    for entry in _label_entries(payload, "iso_group_labels"):
        source = _label_entry_string(entry, "source")
        raw_group = _label_entry_string(entry, "raw_group")
        label = _label_entry_string(entry, "label")
        group_labels[(source, raw_group)] = label

    volume_labels: dict[tuple[str, str, str], str] = {}
    for entry in _label_entries(payload, "iso_volume_labels"):
        source = _label_entry_string(entry, "source")
        raw_group = _label_entry_string(entry, "raw_group")
        raw_volume = _label_entry_string(entry, "raw_volume")
        label = _label_entry_string(entry, "label")
        volume_labels[(source, raw_group, raw_volume)] = label

    return ContentLabelMap(iso_group_labels=group_labels, iso_volume_labels=volume_labels)


def load_content_label_map(path: str | Path) -> ContentLabelMap:
    payload = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError("content label map root must be an object")
    return content_label_map_from_dict(cast(Mapping[str, object], payload))

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


def _sort_nodes(nodes: Iterable[ContentNode]) -> tuple[ContentNode, ...]:
    return tuple(
        sorted(
            nodes,
            key=lambda node: (
                CATEGORY_ORDER.get(node.display_name, 99),
                _program_slot_sort_key(node),
                node.display_name.lower(),
                node.object_type,
                node.object_key,
            ),
        )
    )


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


def _iso_raw_group_and_volume(item: AxklibObject) -> tuple[str, str]:
    parts = [part for part in item.fat_file.replace("\\", "/").split("/") if part]
    group = parts[0] if len(parts) >= 1 else ""
    volume = parts[1] if len(parts) >= 2 else ""
    return group, volume


def _iso_source_key(container: AxklibContainer) -> str:
    return Path(container.source_path).stem


def _iso_group_label(
    container: AxklibContainer, raw_group: str, content_label_map: ContentLabelMap | None = None
) -> str:
    source_key = _iso_source_key(container)
    if content_label_map is not None:
        known = content_label_map.iso_group_label(source_key, raw_group)
        if known:
            return known
    return raw_group or "ISO objects"

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
    container: AxklibContainer, content_label_map: ContentLabelMap | None = None
) -> dict[tuple[str, str], str]:
    by_volume: dict[tuple[str, str], list[AxklibObject]] = defaultdict(list)
    source_key = _iso_source_key(container)
    for item in container.objects:
        raw_group, raw_volume = _iso_raw_group_and_volume(item)
        if not raw_group or not raw_volume:
            continue
        if content_label_map is not None and content_label_map.iso_volume_label(
            source_key, raw_group, raw_volume
        ):
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
    container: AxklibContainer,
    raw_group: str,
    raw_volume: str,
    content_labels: dict[tuple[str, str], str] | None = None,
    content_label_map: ContentLabelMap | None = None,
) -> tuple[str, str]:
    source_key = _iso_source_key(container)
    if content_label_map is not None:
        known = content_label_map.iso_volume_label(source_key, raw_group, raw_volume)
        if known:
            return known, "ISO directory path plus caller-supplied content label"
    if content_labels:
        derived = content_labels.get((raw_group, raw_volume))
        if derived:
            return derived, "ISO directory path plus content-derived volume label fallback"
    return (
        raw_volume or _iso_group_label(container, raw_group, content_label_map),
        "ISO directory path raw volume label",
    )

def _non_sfs_object_placement(
    container: AxklibContainer,
    item: AxklibObject,
    *,
    iso_content_volume_labels: dict[tuple[str, str], str] | None = None,
    content_label_map: ContentLabelMap | None = None,
) -> ObjectPlacement:
    if container.kind == "fat12_floppy":
        partition_name = ""
        volume_name = "FAT root"
        quality = "fat12_floppy container object metadata"
    elif container.kind == "iso":
        raw_group, raw_volume = _iso_raw_group_and_volume(item)
        partition_name = _iso_group_label(container, raw_group, content_label_map)
        volume_name, quality = _iso_volume_label_and_source(
            container,
            raw_group,
            raw_volume,
            iso_content_volume_labels,
            content_label_map=content_label_map,
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
        category_name="Sample Banks" if item.type == "SBAC" else TYPE_CATEGORY.get(item.type, item.type),
        entry_name=item.name,
        quality="Known",
        basis=quality,
    )

def load_known_object_placements(
    container: AxklibContainer,
    content_label_map: ContentLabelMap | None = None,
) -> tuple[dict[str, ObjectPlacement], tuple[ContentTreeIssue, ...]]:
    if container.kind != "sfs":
        iso_content_volume_labels = (
            _iso_content_derived_volume_labels(container, content_label_map)
            if container.kind == "iso"
            else None
        )
        return {
            item.object_key: _non_sfs_object_placement(
                container,
                item,
                iso_content_volume_labels=iso_content_volume_labels,
                content_label_map=content_label_map,
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
    # PROG_ASSIGNMENT_TO_SBAC rows prove decoded row/inventory data, not active assignment.
    "PROG_ASSIGNMENT_TO_SBNK",
    "PROG_ASSIGNMENT_TO_OBJECT",
    "SBAC_SLOT_TO_SBNK",
    "SBNK_LEFT_MEMBER_TO_SMPL",
    "SBNK_RIGHT_MEMBER_TO_SMPL",
}


def _relationship_sort_key(row: Relationship) -> tuple[str, str, str, str]:
    return (row.relationship_type, row.quality, row.target_key, row.key)


def _is_navigable_relationship(row: Relationship) -> bool:
    return row.quality == "Known" and row.relationship_type in _NAVIGABLE_RELATIONSHIP_TYPES


def _has_known_sbac_parent(item: AxklibObject, graph: RelationshipGraph) -> bool:
    return any(
        row.relationship_type == "SBAC_SLOT_TO_SBNK" and row.quality == "Known"
        for row in graph.parents(item.object_key)
    )


def _sbnk_visible_name(item: AxklibObject) -> str:
    return _safe_display(item.name, f"<unnamed SBNK {item.object_key}>")


def _sampler_sample_node(item: AxklibObject, relationship: Relationship | None = None) -> ContentNode:
    return _object_node(
        node_id=f"object:{item.object_key}",
        object_type=item.type,
        display_name=_sbnk_visible_name(item),
        object_key=item.object_key,
        quality=relationship.quality if relationship else "Known",
        basis=relationship.basis if relationship else "container object metadata",
        notes=relationship.ambiguity_notes if relationship else "",
    )


def _sampler_sample_bank_group_node(
    item: AxklibObject,
    *,
    graph: RelationshipGraph,
    object_by_key: dict[str, AxklibObject],
    relationship: Relationship | None = None,
) -> ContentNode:
    children: list[ContentNode] = []
    for child_row in sorted(graph.children(item.object_key), key=_relationship_sort_key):
        if (
            child_row.relationship_type != "SBAC_SLOT_TO_SBNK"
            or not _is_navigable_relationship(child_row)
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
                    quality=DataQuality.UNKNOWN,
                    basis=child_row.basis,
                    notes=child_row.ambiguity_notes,
                )
            )
            continue
        children.append(_sampler_sample_node(child_target, child_row))

    return ContentNode(
        node_id=f"object:{item.object_key}",
        node_type="sample_bank",
        display_name=f"B {_safe_display(item.name, f'<unnamed SBAC {item.object_key}>')}",
        object_key=item.object_key,
        object_type="SBAC",
        quality=DataQuality.KNOWN,
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
) -> ContentNode:
    target = object_by_key.get(target_key)
    if target is None:
        return ContentNode(
            node_id=f"relationship:{relationship.key}:{target_key}",
            node_type="relationship_target",
            display_name=target_key,
            object_key=target_key,
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
    )


def _relationship_object_node(
    *,
    item: AxklibObject,
    graph: RelationshipGraph,
    object_by_key: dict[str, AxklibObject],
    relationship: Relationship | None,
    depth: int,
    visited: frozenset[str],
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
            if not _is_navigable_relationship(row):
                continue
            child_nodes.extend(
                _relationship_child_nodes(
                    relationship=row,
                    graph=graph,
                    object_by_key=object_by_key,
                    depth=depth - 1,
                    visited=next_visited,
                )
            )
        children = _sort_nodes(child_nodes)
    quality = relationship.quality if relationship else "Known"
    quality = relationship.basis if relationship else "container object metadata"
    notes = relationship.ambiguity_notes if relationship else ""
    return _object_node(
        node_id=f"object:{object_key}",
        object_type=object_type,
        display_name=display_name,
        object_key=object_key,
        quality=quality,
        basis=quality,
        notes=notes,
        children=children,
    )


def _build_sfs_tree(
    container: AxklibContainer, content_label_map: ContentLabelMap | None = None
) -> ContentTree:
    placements, issues = load_known_object_placements(container, content_label_map)
    graph = build_relationship_graph(list(container.objects))
    object_by_key = {item.object_key: item for item in container.objects}
    partition_children: dict[tuple[int | None, str], dict[str, dict[str, list[ContentNode]]]] = (
        defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    )
    unmapped: dict[str, list[ContentNode]] = defaultdict(list)

    for item in container.objects:
        if item.type != "SBAC":
            continue
        node = _sampler_sample_bank_group_node(item, graph=graph, object_by_key=object_by_key)
        placement = placements.get(item.object_key)
        if placement is None:
            for row in sorted(graph.children(item.object_key), key=_relationship_sort_key):
                if (
                    row.relationship_type != "SBAC_SLOT_TO_SBNK"
                    or not _is_navigable_relationship(row)
                ):
                    continue
                placement = placements.get(row.target_key)
                if placement is not None:
                    break
        if placement is not None:
            pkey = (placement.partition_index, placement.partition_name)
            partition_children[pkey][placement.volume_name]["Sample Banks"].append(node)
        elif node.children:
            unmapped["Sample Banks"].append(
                ContentNode(
                    node_id=node.node_id,
                    node_type=node.node_type,
                    display_name=node.display_name,
                    object_key=node.object_key,
                    object_type=node.object_type,
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
            )
        if placement is not None:
            pkey = (placement.partition_index, placement.partition_name)
            display_category = TYPE_CATEGORY.get(item.type, placement.category_name)
            partition_children[pkey][placement.volume_name][display_category].append(node)
        else:
            unmapped[category].append(
                _object_node(
                    node_id=f"object:{item.object_key}",
                    object_type=item.type,
                    display_name=_program_slot_label(item)
                    if item.type == "PROG"
                    else _safe_display(item.name, f"<unnamed {item.type} {item.object_key}>"),
                    object_key=item.object_key,
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
        for volume_name, categories in sorted(volumes.items(), key=lambda row: row[0].lower()):
            category_nodes = [
                _with_count(
                    "category",
                    category_name,
                    _with_default_program_slots(objects)
                    if category_name == "Programs"
                    else objects,
                    f"category:{partition_index}:{volume_name}:{category_name}",
                )
                for category_name, objects in categories.items()
            ]
            volume_nodes.append(
                _with_count(
                    "volume",
                    _safe_display(volume_name, "<unnamed volume>"),
                    category_nodes,
                    f"volume:{partition_index}:{volume_name}",
                )
            )
        part_name = partition_name or (
            f"partition {partition_index}" if partition_index is not None else "partition"
        )
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
        category_nodes = [
            _with_count(
                "category",
                category_name,
                _with_default_program_slots(objects) if category_name == "Programs" else objects,
                f"unmapped:{category_name}",
            )
            for category_name, objects in unmapped.items()
        ]
        roots.append(_with_count("unresolved", "_unmapped", category_nodes, "unmapped"))

    return ContentTree(
        source_path=str(container.source_path),
        container_kind=container.kind,
        detected_format=container.detected_format,
        roots=_sort_nodes(roots),
        issues=issues,
    )


def _build_scope_tree(container: AxklibContainer, scope_label: str) -> ContentTree:
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
            )
        )
        if quality == "raw-scan-impossible-internal-capacity":
            recovery[category].append(
                _object_node(
                    node_id=node.node_id,
                    object_type=item.type,
                    display_name=node.display_name,
                    object_key=item.object_key,
                    quality="Likely",
                    basis=str(item.quality.source),
                    notes=quality,
                    children=node.children,
                )
            )
        else:
            groups[category].append(node)

    category_nodes = [
        _with_count(
            "category",
            category_name,
            _with_default_program_slots(objects) if category_name == "Programs" else objects,
            f"category:{category_name}",
        )
        for category_name, objects in groups.items()
    ]
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
    container: AxklibContainer, content_label_map: ContentLabelMap | None = None
) -> ContentTree:
    if container.kind in {"sfs", "iso"}:
        return _build_sfs_tree(container, content_label_map)
    if container.kind == "fat12_floppy":
        return _build_scope_tree(container, "FAT root")
    return _build_scope_tree(container, "Standalone object")


def build_content_trees_for_paths(
    paths: Sequence[str | Path],
    options: OpenOptions | None = None,
    content_label_map: ContentLabelMap | None = None,
) -> ContentTreeLoadResult:
    opts = options or OpenOptions(include_payloads=False)
    results = open_many(paths, options=opts)
    trees: list[ContentTree] = []
    errors: list[AxklibContainerLoadResult] = []
    for result in results:
        if result.container is None:
            errors.append(result)
        else:
            trees.append(build_content_tree_for_container(result.container, content_label_map))
    return ContentTreeLoadResult(trees=tuple(trees), load_errors=tuple(errors))


def _node_suffix(node: ContentNode, options: ContentTreeRenderOptions) -> str:
    parts: list[str] = []
    if node.count is not None:
        parts.append(f"({node.count})")
    if options.show_quality or node.quality != DataQuality.KNOWN:
        parts.append(f"[{node.quality.value}]")
    if node.notes and (options.show_unresolved or node.quality != DataQuality.KNOWN):
        parts.append(f"- {node.notes}")
    return " " + " ".join(parts) if parts else ""


def _render_node(
    node: ContentNode, lines: list[str], options: ContentTreeRenderOptions, depth: int
) -> None:
    if options.max_depth is not None and depth > options.max_depth:
        return
    lines.append(f"{'  ' * depth}{node.display_name}{_node_suffix(node, options)}")
    for child in node.children:
        _render_node(child, lines, options, depth + 1)


def render_content_tree_text(
    tree: ContentTree, options: ContentTreeRenderOptions | None = None
) -> str:
    opts = options or ContentTreeRenderOptions()
    lines = [f"{tree.source_path} [{tree.container_kind}]"]
    for issue in tree.issues:
        lines.append(f"  warning: {issue.code}: {issue.message}")
    for root in tree.roots:
        _render_node(root, lines, opts, 1)
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









