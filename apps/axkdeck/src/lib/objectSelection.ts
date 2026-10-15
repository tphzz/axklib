import type { PackageExportObject } from './types';
import { updateCollectionSelection, collectionSelectionMode } from './collectionSelection';

export type ObjectSelectionMode = 'replace' | 'toggle' | 'range' | 'add-range' | 'all';

export const maximumPackageExportRoots = 1024;

export interface PackageExportSelectionState {
    items: PackageExportObject[];
    anchors: Record<string, string>;
}

export interface PackageExportSelectionUpdate {
    selection: PackageExportSelectionState;
    limitExceeded: boolean;
}

const kindOrder: Record<PackageExportObject['kind'], number> = {
    PROGRAM: 0,
    SEQU: 1,
    SBAC: 2,
    SBNK: 3,
    SMPL: 4,
};

function compareText(left: string, right: string): number {
    return left < right ? -1 : left > right ? 1 : 0;
}

function comparePackageRoots(left: PackageExportObject, right: PackageExportObject): number {
    return (
        left.partitionIndex - right.partitionIndex ||
        compareText(left.volumeName, right.volumeName) ||
        kindOrder[left.kind] - kindOrder[right.kind] ||
        compareText(left.name, right.name) ||
        compareText(left.objectId, right.objectId)
    );
}

export function emptyPackageExportSelection(): PackageExportSelectionState {
    return { items: [], anchors: {} };
}

export function updatePackageExportSelection(
    current: PackageExportSelectionState,
    domainKey: string,
    domain: readonly PackageExportObject[],
    visible: readonly PackageExportObject[],
    targetId: string,
    mode: ObjectSelectionMode,
): PackageExportSelectionUpdate {
    const result = updateCollectionSelection(
        current,
        domainKey,
        domain,
        visible,
        targetId,
        mode,
        maximumPackageExportRoots,
    );
    if (!result.limitExceeded) result.selection.items.sort(comparePackageRoots);
    return result;
}

export function selectionMode(event: MouseEvent): ObjectSelectionMode {
    return collectionSelectionMode(event);
}
