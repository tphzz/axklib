import type { ObjectSelectionMode } from './objectSelection';
import type { DiskTreeItem } from './types';

export interface VolumeSelectionState {
    items: DiskTreeItem[];
    anchorId: string;
}

export interface VolumeSelectionUpdate {
    selection: VolumeSelectionState;
    active: DiskTreeItem;
}

export function emptyVolumeSelection(): VolumeSelectionState {
    return { items: [], anchorId: '' };
}

export function updateVolumeSelection(
    current: VolumeSelectionState,
    visibleVolumes: readonly DiskTreeItem[],
    target: DiskTreeItem,
    mode: ObjectSelectionMode,
): VolumeSelectionUpdate {
    if (target.kind !== 'volume') return { selection: emptyVolumeSelection(), active: target };

    const selected = new Map(current.items.map((item) => [item.id, item]));
    let anchorId = target.id;
    if (mode === 'replace') {
        selected.clear();
        selected.set(target.id, target);
    } else if (mode === 'toggle') {
        if (selected.has(target.id)) selected.delete(target.id);
        else selected.set(target.id, target);
    } else if (mode === 'range' || mode === 'add-range') {
        const anchorIndex = visibleVolumes.findIndex((item) => item.id === current.anchorId);
        const targetIndex = visibleVolumes.findIndex((item) => item.id === target.id);
        const range =
            anchorIndex < 0 || targetIndex < 0
                ? [target]
                : visibleVolumes.slice(Math.min(anchorIndex, targetIndex), Math.max(anchorIndex, targetIndex) + 1);
        anchorId = anchorIndex < 0 ? target.id : current.anchorId;
        if (mode === 'range') selected.clear();
        for (const item of range) selected.set(item.id, item);
    } else {
        for (const item of visibleVolumes) selected.set(item.id, item);
    }

    const items = visibleVolumes.filter((item) => selected.has(item.id));
    for (const item of selected.values()) {
        if (!items.some((candidate) => candidate.id === item.id)) items.push(item);
    }
    const active = selected.has(target.id)
        ? target
        : (nearestSelectedVolume(visibleVolumes, target.id, selected) ?? target);
    return { selection: { items, anchorId }, active };
}

function nearestSelectedVolume(
    visibleVolumes: readonly DiskTreeItem[],
    targetId: string,
    selected: ReadonlyMap<string, DiskTreeItem>,
): DiskTreeItem | null {
    const targetIndex = visibleVolumes.findIndex((item) => item.id === targetId);
    if (targetIndex >= 0) {
        for (let distance = 1; distance < visibleVolumes.length; distance += 1) {
            const after = visibleVolumes[targetIndex + distance];
            if (after && selected.has(after.id)) return after;
            const before = visibleVolumes[targetIndex - distance];
            if (before && selected.has(before.id)) return before;
        }
    }
    return selected.values().next().value ?? null;
}
