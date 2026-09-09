import type { FilesystemImportSourceEntry } from '../../../lib/filesystemImport';
import { serverFileLocation, type DirectoryListing, type DirectoryRef } from '../../../lib/storageLocations';

const maximumEntries = 10000;
const maximumPathCharacters = 4 * 1024 * 1024;
const validComponent = (name: string): boolean => !!name && name !== '.' && name !== '..' && !/[/\\\0]/.test(name);

// The sandbox lists only regular files and directories, never symlinks or special files.
export async function readFilesystemImportTree(
    directory: DirectoryRef,
    list: (directory: DirectoryRef, cursor?: string) => Promise<DirectoryListing>,
    signal: AbortSignal,
    active: () => void,
    progress: (message: string) => void,
): Promise<FilesystemImportSourceEntry[]> {
    if (!directory.rootId || (directory.relativePath && !directory.relativePath.split('/').every(validComponent)))
        throw new Error('The selected source directory path is invalid.');
    const rows: FilesystemImportSourceEntry[] = [];
    const pending = [{ reference: { ...directory }, relativePath: [] as string[] }];
    let pathCharacters = 0;
    const check = (): void => {
        signal.throwIfAborted();
        active();
    };
    // Iterative traversal bounds call-stack use and always visits a parent before its children.
    for (let index = 0; index < pending.length; index += 1) {
        const current = pending[index];
        const names = new Set<string>();
        const cursors = new Set<string>();
        let cursor: string | undefined;
        do {
            check();
            const page = await list(current.reference, cursor);
            check();
            if (
                page.directory.rootId !== directory.rootId ||
                page.directory.relativePath !== current.reference.relativePath
            )
                throw new Error('The source directory listing does not match the requested directory.');
            if (
                page.truncated !== (page.nextCursor !== null) ||
                (page.nextCursor !== null && (!page.nextCursor || !page.entries.length || cursors.has(page.nextCursor)))
            )
                throw new Error('The source directory pagination is incomplete or repeated.');
            for (const entry of page.entries) {
                const path = current.reference.relativePath
                    ? `${current.reference.relativePath}/${entry.name}`
                    : entry.name;
                if (!validComponent(entry.name) || entry.relativePath !== path)
                    throw new Error('The source directory contains an invalid child path.');
                if (names.has(entry.name))
                    throw new Error('The source listing contains a duplicate entry; select the directory again.');
                names.add(entry.name);
                if (rows.length >= maximumEntries)
                    throw new Error('Choose a directory containing at most 10000 entries.');
                const relativePath = [...current.relativePath, entry.name];
                if (relativePath.length >= 1024)
                    throw new Error('The source directory exceeds the filesystem path depth limit.');
                pathCharacters += path.length + relativePath.join('/').length;
                if (pathCharacters > maximumPathCharacters)
                    throw new Error('The source directory exceeds the 4 MiB path data limit.');
                if (entry.kind === 'DIRECTORY') {
                    rows.push({ relativePath, directory: true });
                    pending.push({ reference: { rootId: directory.rootId, relativePath: path }, relativePath });
                } else if (entry.kind === 'FILE') {
                    rows.push({
                        relativePath,
                        directory: false,
                        source: serverFileLocation({ rootId: directory.rootId, relativePath: path }, entry.name),
                    });
                } else throw new Error('The source directory contains an unsupported entry type.');
            }
            progress(`${rows.length} ${rows.length === 1 ? 'entry' : 'entries'} found`);
            cursor = page.nextCursor ?? undefined;
            if (cursor) cursors.add(cursor);
        } while (cursor);
    }
    return rows;
}
