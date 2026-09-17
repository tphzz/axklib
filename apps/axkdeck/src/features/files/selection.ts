import type { FilesystemEntry } from '../../lib/filesystem';
import type { ObjectSelectionMode } from '../../lib/objectSelection';

export interface FilesSelection {
    items: FilesystemEntry[];
    anchorId: string;
}

export function updateFilesSelection(
    current: FilesSelection,
    visible: readonly FilesystemEntry[],
    target: FilesystemEntry,
    mode: ObjectSelectionMode,
): FilesSelection {
    const selected = new Map(current.items.map((entry) => [entry.id, entry]));
    let anchorId = target.id;
    if (mode === 'replace') {
        selected.clear();
        selected.set(target.id, target);
    } else if (mode === 'toggle') {
        if (selected.has(target.id)) selected.delete(target.id);
        else selected.set(target.id, target);
    } else if (mode === 'range' || mode === 'add-range') {
        const start = visible.findIndex((entry) => entry.id === current.anchorId);
        const end = visible.findIndex((entry) => entry.id === target.id);
        const range = start < 0 || end < 0 ? [target] : visible.slice(Math.min(start, end), Math.max(start, end) + 1);
        anchorId = start < 0 ? target.id : current.anchorId;
        if (mode === 'range') selected.clear();
        for (const entry of range) selected.set(entry.id, entry);
    } else {
        for (const entry of visible) selected.set(entry.id, entry);
    }
    return { items: visible.filter((entry) => selected.has(entry.id)), anchorId };
}
