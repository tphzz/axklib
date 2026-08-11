import type { AudioImportTarget } from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';

export function findVolumeSourceItem(items: DiskTreeItem[], target: AudioImportTarget): DiskTreeItem | null {
    for (const item of items) {
        if (
            item.kind === 'volume' &&
            item.partitionIndex === target.partitionIndex &&
            item.name === target.volumeName
        ) {
            return item;
        }
        const nested = findVolumeSourceItem(item.children ?? [], target);
        if (nested) return nested;
    }
    return null;
}

export function sameVolumeTarget(left: AudioImportTarget | null, right: AudioImportTarget): boolean {
    return left?.partitionIndex === right.partitionIndex && left.volumeName === right.volumeName;
}
