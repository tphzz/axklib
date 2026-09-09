import type {
    FilesystemEdit,
    FilesystemImportInspection,
    FilesystemInputInspection,
    FilesystemInputSnapshot,
} from '../../lib/filesystem';
import type { InputFileLocation } from '../../lib/storageLocations';
import type { JobState } from '../../lib/transport';
import type { FilesystemImportSourceEntry } from '../../lib/filesystemImport';

export type FilesImportRow = {
    name: string;
    parent: number | null;
    conflict: 'SKIP' | 'REPLACE';
    decision: FilesystemImportInspection['entries'][number] | null;
} & (
    | { directory: true; source: null; snapshot: null }
    | { directory: false; source: InputFileLocation; snapshot: FilesystemInputSnapshot | null }
);

export function sourceRows(entries: FilesystemImportSourceEntry[]): FilesImportRow[] {
    const directories = new Map<string, number>();
    return entries.map((entry, index) => {
        const name = entry.relativePath.at(-1);
        const parentPath = entry.relativePath.slice(0, -1);
        const parent = parentPath.length ? directories.get(JSON.stringify(parentPath)) : null;
        if (parent === undefined || !name) throw new Error('Source directories must precede their children.');
        if (entry.directory) directories.set(JSON.stringify(entry.relativePath), index);
        const common = { name, parent, conflict: 'SKIP' as const, decision: null };
        return entry.directory
            ? { ...common, directory: true, source: null, snapshot: null }
            : { ...common, directory: false, source: entry.source, snapshot: null };
    });
}

export function importPaths(rows: FilesImportRow[]): string[][] {
    const paths: string[][] = [];
    for (const [index, row] of rows.entries()) {
        if (row.parent !== null && (row.parent < 0 || row.parent >= index || !rows[row.parent]?.directory))
            throw new Error('The import hierarchy is invalid.');
        paths.push([...(row.parent === null ? [] : paths[row.parent]), row.name]);
    }
    return paths;
}

export function fileSources(rows: FilesImportRow[]): InputFileLocation[] {
    return rows.flatMap((row) => (row.directory ? [] : [row.source]));
}

function result<T>(job: JobState): T {
    if (job.status !== 'completed' || !job.result)
        throw new Error(job.status === 'cancelled' ? 'Cancelled' : job.error || 'The inspection did not complete.');
    return job.result as T;
}

export function inspectedSources(job: JobState, rows: FilesImportRow[]): FilesImportRow[] {
    const inspected = result<FilesystemInputInspection>(job);
    if (!Array.isArray(inspected.inputs) || inspected.inputs.length !== fileSources(rows).length)
        throw new Error('The source inspection does not match the selected files.');
    let index = 0;
    return rows.map((row) => {
        if (row.directory) return row;
        const { source, snapshot } = inspected.inputs[index++];
        const matches =
            row.source.kind === 'client-upload'
                ? 'uploadRef' in source && source.uploadRef.uploadId === row.source.reference.uploadId
                : 'fileRef' in source &&
                  source.fileRef.rootId === row.source.reference.rootId &&
                  source.fileRef.relativePath === row.source.reference.relativePath;
        if (
            !matches ||
            !snapshot ||
            !snapshot.revision ||
            !Number.isSafeInteger(snapshot.sizeBytes) ||
            snapshot.sizeBytes < 0 ||
            snapshot.sizeBytes > 0xffffffff ||
            !/^[a-f0-9]{64}$/.test(snapshot.sha256)
        )
            throw new Error('The source inspection does not match the selected files.');
        return { ...row, snapshot: { ...snapshot } };
    });
}

export function inspectedDestination(
    job: JobState,
    revision: number,
    parentId: string,
    rows: FilesImportRow[],
): FilesImportRow[] {
    const inspected = result<FilesystemImportInspection>(job);
    if (
        inspected.revision !== revision ||
        inspected.parentEntryId !== parentId ||
        !Array.isArray(inspected.entries) ||
        inspected.entries.length !== rows.length
    )
        throw new Error('The destination inspection does not match this review.');
    const paths = importPaths(rows);
    return rows.map((row, index) => {
        const decision = inspected.entries[index];
        if (
            decision.directory !== row.directory ||
            decision.relativePath.length !== paths[index].length ||
            decision.relativePath.some((name, part) => name !== paths[index][part]) ||
            decision.sizeBytes !== (row.directory ? 0 : row.snapshot?.sizeBytes) ||
            decision.conflict !== row.conflict ||
            !(
                row.directory
                    ? ['CREATE_DIRECTORY', 'MERGE_DIRECTORY', 'CONFLICT']
                    : ['CREATE_FILE', 'SKIP_FILE', 'REPLACE_FILE', 'CONFLICT']
            ).includes(decision.action)
        )
            throw new Error('The destination inspection does not match this review.');
        return { ...row, decision };
    });
}

export function importEdits(parentId: string, rows: FilesImportRow[]): FilesystemEdit[] {
    const paths = importPaths(rows);
    return rows.map((row, index) => {
        if ((!row.directory && !row.snapshot) || !row.decision || row.decision.action === 'CONFLICT')
            throw new Error('Review every entry before importing.');
        if (row.directory) return { kind: 'CREATE_DIRECTORY', parentEntryId: parentId, relativePath: paths[index] };
        return {
            kind: 'PUT_FILE',
            parentEntryId: parentId,
            relativePath: paths[index],
            source: row.source,
            expectedSource: { ...row.snapshot! },
            conflict: row.conflict,
        };
    });
}
