export type CollectionSelectionMode = 'replace' | 'toggle' | 'range' | 'add-range' | 'all';

export interface CollectionIdentity {
    objectId: string;
}

export interface CollectionSelection<T extends CollectionIdentity = CollectionIdentity> {
    items: T[];
    anchors: Record<string, string>;
}

export function emptyCollectionSelection<T extends CollectionIdentity>(): CollectionSelection<T> {
    return { items: [], anchors: {} };
}

export function updateCollectionSelection<T extends CollectionIdentity>(
    current: CollectionSelection<T>,
    domainKey: string,
    domain: readonly T[],
    visible: readonly T[],
    targetId: string,
    mode: CollectionSelectionMode,
    maximumItems = Number.POSITIVE_INFINITY,
): { selection: CollectionSelection<T>; limitExceeded: boolean } {
    const target = domain.find((item) => item.objectId === targetId);
    if (!target) throw new Error('Selected object is no longer available');
    const selected = new Map(current.items.map((item) => [item.objectId, item]));
    const anchorId = current.anchors[domainKey] ?? '';
    let nextAnchorId = targetId;
    if (mode === 'replace') {
        selected.clear();
        selected.set(targetId, target);
    } else if (mode === 'toggle') {
        if (selected.has(targetId)) selected.delete(targetId);
        else selected.set(targetId, target);
    } else if (mode === 'range' || mode === 'add-range') {
        const start = visible.findIndex((item) => item.objectId === anchorId);
        const end = visible.findIndex((item) => item.objectId === targetId);
        const range = start < 0 || end < 0 ? [target] : visible.slice(Math.min(start, end), Math.max(start, end) + 1);
        nextAnchorId = start < 0 ? targetId : anchorId;
        if (mode === 'range') for (const item of domain) selected.delete(item.objectId);
        for (const item of range) selected.set(item.objectId, item);
    } else {
        for (const item of visible) selected.set(item.objectId, item);
    }
    if (selected.size > maximumItems) return { selection: current, limitExceeded: true };
    return {
        selection: { items: [...selected.values()], anchors: { ...current.anchors, [domainKey]: nextAnchorId } },
        limitExceeded: false,
    };
}

export function collectionSelectionMode(event: MouseEvent): CollectionSelectionMode {
    if (event.shiftKey) return event.ctrlKey || event.metaKey ? 'add-range' : 'range';
    return event.ctrlKey || event.metaKey ? 'toggle' : 'replace';
}
