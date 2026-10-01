import type { FilesContext } from './controller.svelte';
import type { FilesystemEntry } from '../../lib/filesystem';

export interface FilesRelocation {
    fromRevision: number;
    toRevision: number;
    entries: { entry: FilesystemEntry; path: string; name: string }[];
    destination?: FilesystemEntry;
}

export function relocateContext(context: FilesContext, revision: number): FilesContext {
    const relocation = context.relocation;
    if (!relocation || context.revision !== relocation.fromRevision || revision !== relocation.toRevision)
        return context;
    const pathFor = (root: string, path: string): string => {
        const match = relocation.entries.find(
            ({ entry }) =>
                entry.rootId === root &&
                (path === entry.path || (entry.kind === 'directory' && path.startsWith(entry.path + '/'))),
        );
        return match ? match.path + path.slice(match.entry.path.length) : path;
    };
    const remap = (entry: FilesystemEntry): FilesystemEntry => {
        const exact = relocation.entries.find((item) => item.entry.id === entry.id && item.entry.path === entry.path);
        return { ...entry, path: pathFor(entry.rootId, entry.path), name: exact?.name ?? entry.name };
    };
    return {
        ...context,
        revision,
        states: Object.fromEntries(
            Object.entries(context.states).map(([id, state]) => {
                const destination = relocation.destination;
                const expanded = state.expanded.map(remap);
                if (
                    destination?.rootId === id &&
                    destination.kind === 'directory' &&
                    !expanded.some((item) => item.id === destination.id)
                )
                    expanded.push(destination);
                return [
                    id,
                    {
                        ...state,
                        focused: state.focused ? remap(state.focused) : null,
                        selection: { ...state.selection, items: state.selection.items.map(remap) },
                        expanded,
                        loadedCounts: Object.fromEntries(
                            Object.entries(state.loadedCounts).map(([path, count]) => [pathFor(id, path), count]),
                        ),
                    },
                ];
            }),
        ),
    };
}
