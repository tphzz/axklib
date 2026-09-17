import type { DiskTreeItem } from '../../lib/types';

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
