import type { FilesystemImageInspection, FilesystemRootCapabilities } from '../../lib/filesystem';
import { importPaths, type FilesImportRow } from './importReview';
import { normalizeFilesystemName } from './nameValidation';

export interface ImageImportGroup {
    name: string;
    rows: FilesImportRow[];
    selected: number[];
    error: string;
}

export function imageContentRows(
    inspection: FilesystemImageInspection,
    capabilities: FilesystemRootCapabilities,
): FilesImportRow[] {
    if (
        !/^[a-f0-9]{64}$/.test(inspection.inspectionToken) ||
        !Array.isArray(inspection.entries) ||
        inspection.entries.length > 8192
    )
        throw new Error('Invalid floppy inspection result.');
    const directories = new Map<string, number>();
    const paths = new Set<string>();
    return inspection.entries.map((entry, index) => {
        const path = entry.relativePath;
        if (
            !Array.isArray(path) ||
            !path.length ||
            path.some((name) => !name || name === '.' || name === '..' || /[\\/\0]/.test(name))
        )
            throw new Error('Invalid floppy entry path.');
        const key = JSON.stringify(path);
        if (paths.has(key)) throw new Error('Duplicate floppy entry path.');
        paths.add(key);
        const parent = path.length > 1 ? directories.get(JSON.stringify(path.slice(0, -1))) : null;
        if (parent === undefined) throw new Error('Missing floppy parent directory.');
        const common = {
            name: normalizeFilesystemName(path.at(-1)!, capabilities),
            parent,
            conflict: 'SKIP' as const,
            decision: null,
        };
        if (entry.directory) {
            directories.set(key, index);
            return { ...common, directory: true, source: null, snapshot: null };
        }
        if (
            !entry.snapshot ||
            !entry.snapshot.revision ||
            !Number.isSafeInteger(entry.snapshot.sizeBytes) ||
            entry.snapshot.sizeBytes < 0 ||
            entry.snapshot.sizeBytes > 4194304 ||
            !/^[a-f0-9]{64}$/.test(entry.snapshot.sha256)
        )
            throw new Error('Invalid floppy file snapshot.');
        return {
            ...common,
            directory: false,
            snapshot: entry.snapshot,
            source: {
                kind: 'image-entry',
                displayName: path.join('/'),
                reference: { inspectionToken: inspection.inspectionToken, entryId: entry.entryId },
            },
        };
    });
}

export function descendants(rows: FilesImportRow[], index: number): number[] {
    if (!rows[index]?.directory) return [index];
    const included = new Set([index]);
    for (let child = index + 1; child < rows.length; child++) {
        const parent = rows[child].parent;
        if (parent !== null && included.has(parent)) included.add(child);
    }
    return [...included];
}

export function selectedImageRows(groups: ImageImportGroup[]): {
    rows: FilesImportRow[];
    mapping: Map<string, number>;
    errors: Map<string, string>;
} {
    const rows: FilesImportRow[] = [];
    const mapping = new Map<string, number>();
    const errors = new Map<string, string>();
    const destinations = new Map<string, { index: number; keys: string[] }>();
    for (const [groupIndex, group] of groups.entries()) {
        const selected = new Set(group.selected);
        for (const index of group.selected) {
            let parent = group.rows[index]?.parent;
            while (parent !== null && parent !== undefined) {
                selected.add(parent);
                parent = group.rows[parent].parent;
            }
        }
        const paths = importPaths(group.rows);
        for (const [index, row] of group.rows.entries()) {
            if (!selected.has(index)) continue;
            const key = `${groupIndex}:${index}`;
            const path = JSON.stringify(paths[index]);
            const existing = destinations.get(path);
            if (existing) {
                existing.keys.push(key);
                if (!(row.directory && rows[existing.index].directory)) {
                    for (const duplicate of existing.keys)
                        errors.set(
                            duplicate,
                            'Multiple selected entries have the same destination. Rename or deselect an entry.',
                        );
                }
                mapping.set(key, existing.index);
                continue;
            }
            const parent = row.parent === null ? null : mapping.get(`${groupIndex}:${row.parent}`)!;
            mapping.set(key, rows.length);
            destinations.set(path, { index: rows.length, keys: [key] });
            rows.push({ ...row, parent, decision: null });
        }
    }
    return { rows, mapping, errors };
}
