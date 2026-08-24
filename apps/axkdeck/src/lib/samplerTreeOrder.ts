import type { DiskTreeItem } from './types';

type SamplerOrderedKind = Extract<DiskTreeItem['kind'], 'partition' | 'volume'>;

function compareSamplerNames(left: string, right: string): number {
    const sharedLength = Math.min(left.length, right.length);
    for (let index = 0; index < sharedLength; index += 1) {
        const difference = left.charCodeAt(index) - right.charCodeAt(index);
        if (difference !== 0) return difference;
    }
    return left.length - right.length;
}

function compareOptionalNumber(left: number | undefined, right: number | undefined): number {
    if (left === right) return 0;
    if (left === undefined) return 1;
    if (right === undefined) return -1;
    return left - right;
}

function comparePhysicalIdentity(left: DiskTreeItem, right: DiskTreeItem): number {
    return (
        compareOptionalNumber(left.partitionIndex, right.partitionIndex) ||
        compareOptionalNumber(left.volumeDirectoryId, right.volumeDirectoryId) ||
        compareSamplerNames(left.id, right.id)
    );
}

function compareSamplerTreeItems(left: DiskTreeItem, right: DiskTreeItem): number {
    return compareSamplerNames(left.name, right.name) || comparePhysicalIdentity(left, right);
}

export function orderSamplerTreeItems(items: readonly DiskTreeItem[], kind: SamplerOrderedKind): DiskTreeItem[] {
    const ordered = items.filter((item) => item.kind === kind).sort(compareSamplerTreeItems);
    let orderedIndex = 0;
    return items.map((item) => (item.kind === kind ? ordered[orderedIndex++]! : item));
}
