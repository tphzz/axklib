import { invoke } from '@tauri-apps/api/core';
import { lstat, type FileInfo } from '@tauri-apps/plugin-fs';
import type { ClientFilesystemImportEntry, FilesystemDropReader } from './filesystemImport';
import { nativeBasename, nativeRawFileSource, sameNativeFileIdentity } from './nativeFileSource';

export function nativeFilesystemDrop(paths: readonly string[]): FilesystemDropReader {
    const roots = [...paths];
    return async (signal, progress) => {
        if (roots.length > 10000) throw new Error('Choose at most 10000 entries.');
        const result: ClientFilesystemImportEntry[] = [];
        const pending = roots.map((path) => ({ path, parent: [] as string[] })).reverse();
        const directories: { path: string; info: FileInfo }[] = [];
        const seen = new Set<string>();
        let encountered = roots.length,
            characters = 0,
            totalBytes = 0;
        while (pending.length) {
            signal.throwIfAborted();
            const { path, parent } = pending.pop()!;
            const name = nativeBasename(path);
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
            if (seen.has(identity)) throw new Error('Duplicate dropped entries.');
            seen.add(identity);
            const info = await lstat(path);
            signal.throwIfAborted();
            if (info.isSymlink || (info.fileAttributes ?? 0) & 0x400 || (!info.isDirectory && !info.isFile))
                throw new Error('Dropped path is not a regular file or directory.');
            if (info.isDirectory) {
                directories.push({ path, info });
                result.push({ relativePath, directory: true });
                const children = await invoke<string[]>('read_native_drop_directory', {
                    path,
                    limit: 10000 - encountered,
                });
                signal.throwIfAborted();
                encountered += children.length;
                if (encountered > 10000) throw new Error('Choose at most 10000 entries.');
                for (const child of children.reverse()) {
                    if (!child || /[/\\\0]/.test(child) || child === '.' || child === '..')
                        throw new Error('A dropped path is invalid.');
                    pending.push({ path: `${path}/${child}`, parent: relativePath });
                }
            } else {
                if (!Number.isSafeInteger(info.size) || info.size < 0 || info.size > 0xffffffff)
                    throw new Error('A dropped file exceeds the file size limit.');
                totalBytes += info.size;
                if (totalBytes > 8 * 1024 * 1024 * 1024)
                    throw new Error('Dropped files exceed the aggregate size limit.');
                const source = await nativeRawFileSource(path);
                if (!sameNativeFileIdentity(info, await lstat(path)) || source.size !== info.size)
                    throw new Error('A dropped file changed while reading.');
                result.push({ relativePath, directory: false, source });
            }
            progress(`Reading dropped entries (${result.length})`);
        }
        for (const { path, info } of directories) {
            signal.throwIfAborted();
            if (!sameNativeFileIdentity(info, await lstat(path)))
                throw new Error('A dropped directory changed while reading.');
        }
        signal.throwIfAborted();
        return result;
    };
}
