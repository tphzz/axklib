"""Conservative orphan classification for current SFS waveform objects."""

from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

from axklib.containers import OpenOptions, sfs_dump, sfs_inventory, sfs_scan
from axklib.containers import open as open_container
from axklib.content_tree import load_known_object_placements
from axklib.model import AxklibObject
from axklib.objects import current as current_objects
from axklib.parameters import current as current_parameters

WAVEFORM_STATUS_REFERENCED = "referenced"
WAVEFORM_STATUS_KNOWN_UNREFERENCED = "known_unreferenced"
WAVEFORM_STATUS_AMBIGUOUS_OR_UNRESOLVED = "ambiguous_or_unresolved"


@dataclass(frozen=True)
class WaveformOrphanRow:
    """Classification and supporting basis for one physical current SMPL object."""

    source_path: str
    partition_index: int
    partition_name: str
    volume_name: str
    waveform_name: str
    object_key: str
    sfs_id: int
    smpl_link_id: int | None
    status: str
    referencing_sample_banks: str
    basis: str
    notes: str


@dataclass(frozen=True)
class WaveformOrphanSummary:
    """Counts for one waveform-orphan analysis."""

    source_path: str
    waveform_count: int
    referenced_count: int
    known_unreferenced_count: int
    ambiguous_or_unresolved_count: int


@dataclass(frozen=True)
class WaveformOrphanReport:
    """Rows and aggregate counts returned by the orphan checker."""

    summary: WaveformOrphanSummary
    rows: tuple[WaveformOrphanRow, ...]


@dataclass(frozen=True)
class _CurrentObjectInput:
    object_key: str
    partition_index: int
    partition_name: str
    volume_name: str
    name: str
    sfs_id: int
    payload: bytes
    has_exact_placement: bool


def _partition_names(path: Path) -> dict[int, str]:
    parsed = sfs_dump.parse_image(
        path, sfs_dump.ReadOptions(max_nodes=4, include_node_payloads=False)
    )
    result: dict[int, str] = {}
    partitions = parsed.get("partitions")
    if not isinstance(partitions, list):
        return result
    for partition in partitions:
        if not isinstance(partition, dict):
            continue
        index = partition.get("index")
        name = partition.get("name")
        if isinstance(index, int) and isinstance(name, str):
            result[index] = name
    return result


def _raw_partition_uncertainties(path: Path) -> dict[int, list[str]]:
    parsed = sfs_dump.parse_image(
        path, sfs_dump.ReadOptions(max_nodes=4, include_node_payloads=False)
    )
    sector_size = parsed.get("sector_size_bytes")
    if not isinstance(sector_size, int):
        sector_size = sfs_dump.DEFAULT_SECTOR_SIZE
    object_rows = sfs_scan.scan_image(path, max_nodes=4)
    uncertainties: dict[int, list[str]] = defaultdict(list)
    partitions = parsed.get("partitions")
    if not isinstance(partitions, list):
        return uncertainties
    for partition in partitions:
        if not isinstance(partition, dict):
            continue
        partition_index = sfs_inventory.int_mapping_value(partition, "index")
        for record in sfs_inventory.scan_ynode_records(
            path, partition, object_rows, sector_size=sector_size
        ):
            if record.sfs_id == 0 or record.payload_kind in {
                "directory",
                "object",
            }:
                continue
            uncertainties[partition_index].append(
                f"SFS ID {record.sfs_id} has unresolved payload kind {record.payload_kind!r}"
            )
    return uncertainties


def _classify_waveform_inputs(
    *,
    source_path: str,
    waveforms: list[_CurrentObjectInput],
    banks: list[_CurrentObjectInput],
    partition_uncertainties: dict[int, list[str]],
    global_uncertainties: list[str],
) -> WaveformOrphanReport:
    waveform_metadata: dict[str, current_objects.CurrentSmplMetadata] = {}
    for waveform in waveforms:
        try:
            waveform_metadata[waveform.object_key] = (
                current_objects.decode_current_smpl_metadata(waveform.payload)
            )
        except Exception as exc:
            partition_uncertainties.setdefault(waveform.partition_index, []).append(
                f"waveform {waveform.name!r} metadata is unresolved: {exc}"
            )

    referenced_by: dict[str, list[str]] = defaultdict(list)
    for bank in banks:
        bank_label = (
            f"{bank.volume_name}/{bank.name}" if bank.has_exact_placement else bank.name
        )
        try:
            decoded = current_parameters.decode_current_sbnk_members(bank.payload)
        except Exception as exc:
            partition_uncertainties.setdefault(bank.partition_index, []).append(
                f"sample bank {bank_label!r} cannot be decoded: {exc}"
            )
            continue
        members = [decoded.left]
        if decoded.right is not None:
            members.append(decoded.right)
        for member in members:
            if not member.sample_name or not member.smpl_link_id:
                partition_uncertainties.setdefault(bank.partition_index, []).append(
                    f"sample bank {bank_label!r} has an unresolved {member.lane} member"
                )
                continue
            candidates = [
                waveform
                for waveform in waveforms
                if waveform.partition_index == bank.partition_index
                and (metadata := waveform_metadata.get(waveform.object_key)) is not None
                and waveform.name == member.sample_name
                and metadata.smpl_link_id_0x078 == member.smpl_link_id
            ]
            if len(candidates) != 1:
                partition_uncertainties.setdefault(bank.partition_index, []).append(
                    f"sample bank {bank_label!r} {member.lane} member {member.sample_name!r} "
                    f"matches {len(candidates)} current SMPL objects"
                )
                continue
            referenced_by[candidates[0].object_key].append(bank_label)

    rows: list[WaveformOrphanRow] = []
    for waveform in waveforms:
        partition_index = waveform.partition_index
        metadata = waveform_metadata.get(waveform.object_key)
        references = tuple(sorted(set(referenced_by.get(waveform.object_key, []))))
        uncertainties = [
            *global_uncertainties,
            *partition_uncertainties.get(partition_index, []),
        ]
        if not waveform.has_exact_placement:
            uncertainties.append("waveform has no exact visible SMPL directory placement")
        if metadata is None:
            uncertainties.append("waveform current SMPL metadata is unreadable")
        if references:
            status = WAVEFORM_STATUS_REFERENCED
            basis = "unique current SBNK member match by waveform name and SMPL link ID"
            notes = ""
        elif uncertainties:
            status = WAVEFORM_STATUS_AMBIGUOUS_OR_UNRESOLVED
            basis = "orphan status withheld because partition ownership is unresolved"
            notes = " | ".join(dict.fromkeys(uncertainties))
        else:
            status = WAVEFORM_STATUS_KNOWN_UNREFERENCED
            basis = (
                "exact SMPL directory placement and no member reference after complete "
                "current SBNK resolution"
            )
            notes = ""
        rows.append(
            WaveformOrphanRow(
                source_path=source_path,
                partition_index=partition_index,
                partition_name=waveform.partition_name,
                volume_name=waveform.volume_name,
                waveform_name=waveform.name,
                object_key=waveform.object_key,
                sfs_id=waveform.sfs_id,
                smpl_link_id=metadata.smpl_link_id_0x078 if metadata is not None else None,
                status=status,
                referencing_sample_banks=";".join(references),
                basis=basis,
                notes=notes,
            )
        )
    rows.sort(
        key=lambda row: (
            row.partition_index,
            row.volume_name,
            row.waveform_name,
            row.sfs_id,
        )
    )
    summary = WaveformOrphanSummary(
        source_path=source_path,
        waveform_count=len(rows),
        referenced_count=sum(row.status == WAVEFORM_STATUS_REFERENCED for row in rows),
        known_unreferenced_count=sum(
            row.status == WAVEFORM_STATUS_KNOWN_UNREFERENCED for row in rows
        ),
        ambiguous_or_unresolved_count=sum(
            row.status == WAVEFORM_STATUS_AMBIGUOUS_OR_UNRESOLVED for row in rows
        ),
    )
    return WaveformOrphanReport(summary=summary, rows=tuple(rows))


def analyze_hds_waveform_orphans(path: str | Path) -> WaveformOrphanReport:
    """Classify current SMPL objects without changing the source image.

    A waveform is Known unreferenced only when it has exact SFS placement,
    readable current metadata, and every current SBNK member in its partition
    resolves uniquely by both SMPL link ID and waveform name. Any unresolved
    SBNK member or allocated unknown record makes otherwise-unreferenced
    waveforms in that partition ambiguous.
    """
    source = Path(path)
    container = open_container(
        source, options=OpenOptions(strict=True, include_payloads=True)
    )
    if container.kind != "sfs":
        raise ValueError(f"waveform orphan analysis requires an SFS image: {source}")
    placements, placement_issues = load_known_object_placements(container)
    partition_names = _partition_names(source)
    partition_uncertainties = _raw_partition_uncertainties(source)
    global_uncertainties = [f"{issue.code}: {issue.message}" for issue in placement_issues]

    def item_input(item: AxklibObject) -> _CurrentObjectInput:
        object_key = item.object_key
        placement = placements.get(object_key)
        partition_index = item.partition_index
        sfs_id = item.sfs_id
        normalized_partition_index = (
            partition_index if isinstance(partition_index, int) else -1
        )
        return _CurrentObjectInput(
            object_key=object_key,
            partition_index=normalized_partition_index,
            partition_name=(
                placement.partition_name
                if placement is not None
                else partition_names.get(normalized_partition_index, "")
            ),
            volume_name=placement.volume_name if placement is not None else "",
            name=item.name,
            sfs_id=sfs_id if isinstance(sfs_id, int) else -1,
            payload=item.payload,
            has_exact_placement=placement is not None,
        )

    waveforms = [item_input(item) for item in container.objects if item.type == "SMPL"]
    banks = [item_input(item) for item in container.objects if item.type == "SBNK"]
    return _classify_waveform_inputs(
        source_path=str(source),
        waveforms=waveforms,
        banks=banks,
        partition_uncertainties=partition_uncertainties,
        global_uncertainties=global_uncertainties,
    )


__all__ = [
    "WAVEFORM_STATUS_AMBIGUOUS_OR_UNRESOLVED",
    "WAVEFORM_STATUS_KNOWN_UNREFERENCED",
    "WAVEFORM_STATUS_REFERENCED",
    "WaveformOrphanReport",
    "WaveformOrphanRow",
    "WaveformOrphanSummary",
    "analyze_hds_waveform_orphans",
]
