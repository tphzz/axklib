import type { FilesystemEntry, FilesystemRootCapabilities } from '../../lib/filesystem';

export function moveTargetAllowed(entries: FilesystemEntry[], target: FilesystemEntry | null): boolean {
    return (
        !!target &&
        target.kind !== 'file' &&
        !target.filesystemMetadata &&
        !target.issue &&
        entries.length > 0 &&
        entries.length <= 10000 &&
        entries.every(
            (entry) =>
                !!entry.parentId &&
                !entry.filesystemMetadata &&
                !entry.issue &&
                entry.rootId === target.rootId &&
                entry.id !== target.id &&
                !target.ancestorIds.includes(entry.id) &&
                !entry.attributes.some((attribute) => attribute.code === 'fat.read-only'),
        )
    );
}

export function moveSelection(entries: FilesystemEntry[], target: FilesystemEntry): FilesystemEntry[] {
    const ids = new Set(entries.map((entry) => entry.id));
    return [...new Map(entries.map((entry) => [entry.id, entry])).values()].filter(
        (entry) => entry.parentId !== target.id && !entry.ancestorIds.some((id) => ids.has(id)),
    );
}

export function moveConflicts(
    entries: FilesystemEntry[],
    children: FilesystemEntry[],
    capabilities: FilesystemRootCapabilities,
): Record<string, string> {
    const key = (name: string) => (capabilities.namePolicy === 'FAT_8_3_UPPERCASE' ? name.toUpperCase() : name);
    const occupied = new Set(children.map((entry) => key(entry.name)));
    const counts = new Map<string, number>();
    for (const entry of entries) counts.set(key(entry.name), (counts.get(key(entry.name)) ?? 0) + 1);
    return Object.fromEntries(
        entries.flatMap((entry) => {
            const name = key(entry.name);
            const issue = occupied.has(name)
                ? 'This name already exists in the destination.'
                : counts.get(name)! > 1
                  ? 'Selected entries have the same destination name.'
                  : '';
            return issue ? [[entry.id, issue]] : [];
        }),
    );
}
