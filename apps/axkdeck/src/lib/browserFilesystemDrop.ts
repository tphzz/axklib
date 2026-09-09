import { browserUploadSource } from './clientUploadSource';
import type { ClientFilesystemImportEntry, FilesystemDropReader } from './filesystemImport';

const maximumEntries = 10000;

function callback<T>(
    signal: AbortSignal,
    start: (done: (value: T) => void, fail: (reason: unknown) => void) => void,
): Promise<T> {
    signal.throwIfAborted();
    return new Promise<T>((resolve, reject) => {
        const aborted = () => finish(() => reject(signal.reason));
        const finish = (action: () => void) => {
            signal.removeEventListener('abort', aborted);
            action();
        };
        signal.addEventListener('abort', aborted, { once: true });
        try {
            start(
                (value) => finish(() => resolve(value)),
                (error) => finish(() => reject(error)),
            );
        } catch (error) {
            finish(() => reject(error));
        }
    });
}

// Drop-store access must finish in the event handler, before any asynchronous work.
export function captureBrowserFilesystemDrop(data: DataTransfer): FilesystemDropReader {
    const roots: (FileSystemEntry | File)[] = [];
    if (data.items.length > maximumEntries || data.files.length > maximumEntries)
        throw new Error('Choose at most 10000 entries.');
    const items = Array.from(data.items);
    if (items.length) {
        for (const item of items) {
            if (item.kind !== 'file') continue;
            const entry = item.webkitGetAsEntry?.() ?? item.getAsFile();
            if (!entry) throw new Error('A dropped entry could not be read.');
            roots.push(entry);
        }
    } else roots.push(...Array.from(data.files));
    return async (signal, progress) => {
        const result: ClientFilesystemImportEntry[] = [];
        const pending = roots.map((entry) => ({ entry, parent: [] as string[] })).reverse();
        const seen = new Set<string>();
        const handles = new Set<FileSystemEntry | File>();
        let encountered = roots.length,
            characters = 0,
            totalBytes = 0;
        while (pending.length) {
            signal.throwIfAborted();
            const { entry, parent } = pending.pop()!;
            const name = entry.name;
            if (
                !name ||
                name.length > 255 ||
                /[/\\\0]/.test(name) ||
                name === '.' ||
                name === '..' ||
                parent.length >= 63
            )
                throw new Error('A dropped path is invalid or too deep.');
            const relativePath = [...parent, name],
                identity = JSON.stringify(relativePath);
            characters += identity.length;
            if (characters > 4194304) throw new Error('Dropped paths exceed the metadata limit.');
            if (seen.has(identity) || handles.has(entry)) throw new Error('Duplicate or cyclic dropped entries.');
            seen.add(identity);
            handles.add(entry);
            if (!(entry instanceof File) && entry.isDirectory && !entry.isFile) {
                result.push({ relativePath, directory: true });
                const reader = (entry as FileSystemDirectoryEntry).createReader();
                const children: FileSystemEntry[] = [];
                for (;;) {
                    const batch = await callback<FileSystemEntry[]>(signal, (done, fail) =>
                        reader.readEntries(done, fail),
                    );
                    if (!batch.length) break;
                    encountered += batch.length;
                    if (encountered > maximumEntries) throw new Error('Choose at most 10000 entries.');
                    children.push(...batch);
                }
                for (const child of children.reverse()) pending.push({ entry: child, parent: relativePath });
            } else {
                if (!(entry instanceof File) && (!entry.isFile || entry.isDirectory))
                    throw new Error('Unsupported dropped entry.');
                const file =
                    entry instanceof File
                        ? entry
                        : await callback<File>(signal, (done, fail) => (entry as FileSystemFileEntry).file(done, fail));
                if (file.name !== name || !Number.isSafeInteger(file.size) || file.size < 0 || file.size > 0xffffffff)
                    throw new Error('A dropped file changed or exceeds the file size limit.');
                totalBytes += file.size;
                if (totalBytes > 8 * 1024 * 1024 * 1024)
                    throw new Error('Dropped files exceed the aggregate size limit.');
                result.push({ relativePath, directory: false, source: browserUploadSource(file) });
            }
            progress(`Reading dropped entries (${result.length})`);
        }
        signal.throwIfAborted();
        return result;
    };
}
