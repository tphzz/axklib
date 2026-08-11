import type { PackageExportObject } from './types';

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
    const target = domain.find((item) => item.objectId === targetId);
    if (!target) throw new Error('Selected object is no longer available');

    const selected = new Map(current.items.map((item) => [item.objectId, item]));
    const domainIds = new Set(domain.map((item) => item.objectId));
    const anchorId = current.anchors[domainKey] ?? '';
    let nextAnchorId = targetId;
    if (mode === 'replace') {
        selected.clear();
        selected.set(target.objectId, target);
    } else if (mode === 'toggle') {
        if (selected.has(target.objectId)) selected.delete(target.objectId);
        else selected.set(target.objectId, target);
    } else if (mode === 'range' || mode === 'add-range') {
        const start = visible.findIndex((item) => item.objectId === anchorId);
        const end = visible.findIndex((item) => item.objectId === targetId);
        const range = start < 0 || end < 0 ? [target] : visible.slice(Math.min(start, end), Math.max(start, end) + 1);
        nextAnchorId = start < 0 ? targetId : anchorId;
        if (mode === 'range') {
            for (const objectId of domainIds) selected.delete(objectId);
        }
        for (const item of range) selected.set(item.objectId, item);
    } else {
        for (const item of visible) selected.set(item.objectId, item);
    }

    if (selected.size > maximumPackageExportRoots) return { selection: current, limitExceeded: true };

    return {
        selection: {
            items: [...selected.values()].sort(comparePackageRoots),
            anchors: { ...current.anchors, [domainKey]: nextAnchorId },
        },
        limitExceeded: false,
    };
}

export function selectionMode(event: MouseEvent): ObjectSelectionMode {
    if (event.shiftKey) return event.ctrlKey || event.metaKey ? 'add-range' : 'range';
    return event.ctrlKey || event.metaKey ? 'toggle' : 'replace';
}
