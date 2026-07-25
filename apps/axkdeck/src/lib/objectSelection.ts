export type ObjectSelectionMode = 'replace' | 'toggle' | 'range' | 'add-range' | 'all';

export interface ObjectSelectionResult {
    objectIds: string[];
    anchorId: string;
}

const maximumSelectionSize = 1024;

function orderedSelection(domainIds: readonly string[], selected: ReadonlySet<string>): string[] {
    return domainIds.filter((id) => selected.has(id));
}

export function updateObjectSelection(
    currentIds: readonly string[],
    anchorId: string,
    domainIds: readonly string[],
    visibleIds: readonly string[],
    targetId: string,
    mode: ObjectSelectionMode,
): ObjectSelectionResult {
    if (!domainIds.includes(targetId)) throw new Error('Selected object is no longer available');

    const selected = new Set(currentIds.filter((id) => domainIds.includes(id)));
    let nextAnchor = targetId;
    if (mode === 'all') {
        selected.clear();
        visibleIds.forEach((id) => selected.add(id));
    } else if (mode === 'toggle') {
        if (selected.has(targetId)) selected.delete(targetId);
        else selected.add(targetId);
    } else if (mode === 'range' || mode === 'add-range') {
        const start = visibleIds.indexOf(anchorId);
        const end = visibleIds.indexOf(targetId);
        const range =
            start < 0 || end < 0 ? [targetId] : visibleIds.slice(Math.min(start, end), Math.max(start, end) + 1);
        if (mode === 'range') selected.clear();
        range.forEach((id) => selected.add(id));
        nextAnchor = start < 0 ? targetId : anchorId;
    } else {
        selected.clear();
        selected.add(targetId);
    }

    return {
        objectIds: orderedSelection(domainIds, selected).slice(0, maximumSelectionSize),
        anchorId: nextAnchor,
    };
}

export function selectionMode(event: MouseEvent): ObjectSelectionMode {
    if (event.shiftKey) return event.ctrlKey || event.metaKey ? 'add-range' : 'range';
    return event.ctrlKey || event.metaKey ? 'toggle' : 'replace';
}
