import type { DiskTreeItem } from '../../lib/types';
import type { VolumeSelectionState } from '../../lib/volumeSelection';

export function collectVolumes(items: readonly DiskTreeItem[]): DiskTreeItem[] {
    const result: DiskTreeItem[] = [];
    for (const item of items) {
        if (item.kind === 'volume') result.push(item);
        result.push(...collectVolumes(item.children ?? []));
    }
    return result;
}

export function findPartition(items: readonly DiskTreeItem[], partitionIndex: number): DiskTreeItem | null {
    for (const item of items) {
        if (item.kind === 'partition' && item.partitionIndex === partitionIndex) return item;
        const nested = findPartition(item.children ?? [], partitionIndex);
        if (nested) return nested;
    }
    return null;
}

export function findSourceItem(
    items: DiskTreeItem[],
    partitionIndex: number,
    volumeName?: string,
): DiskTreeItem | null {
    for (const item of items) {
        if (
            item.partitionIndex === partitionIndex &&
            (volumeName === undefined ? item.kind === 'partition' : item.kind === 'volume' && item.name === volumeName)
        ) {
            return item;
        }
        const nested = findSourceItem(item.children ?? [], partitionIndex, volumeName);
        if (nested) return nested;
    }
    return null;
}

export function findRefreshedVolume(items: readonly DiskTreeItem[], previous: DiskTreeItem): DiskTreeItem | null {
    if (previous.kind !== 'volume') return null;
    // Content IDs belong to one server snapshot; directory identities survive Sample writes.
    const matches = collectVolumes(items).filter(
        (item) =>
            item.partitionIndex === previous.partitionIndex &&
            (previous.volumeDirectoryId !== undefined
                ? item.volumeDirectoryId === previous.volumeDirectoryId
                : item.name === previous.name),
    );
    return matches.length === 1 ? matches[0]! : null;
}

export function refreshVolumeSelection(
    selection: VolumeSelectionState,
    tree: readonly DiskTreeItem[],
): VolumeSelectionState {
    const refreshed = selection.items.map((item) => ({ previous: item, current: findRefreshedVolume(tree, item) }));
    return {
        items: refreshed.flatMap(({ current }) => (current ? [current] : [])),
        anchorId: refreshed.find(({ previous }) => previous.id === selection.anchorId)?.current?.id ?? '',
    };
}
