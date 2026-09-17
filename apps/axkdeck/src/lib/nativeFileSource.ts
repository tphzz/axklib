import { lstat, open, SeekMode, type FileInfo } from '@tauri-apps/plugin-fs';
import type { ClientUploadSource } from './clientUploadSource';

const maximumNativeFileBytes = 4 * 1024 * 1024 * 1024;
const maximumChunkBytes = 8 * 1024 * 1024;

export function sameNativeFileIdentity(left: FileInfo, right: FileInfo): boolean {
    return (
        left.isFile === right.isFile &&
        left.isDirectory === right.isDirectory &&
        !right.isSymlink &&
        !((right.fileAttributes ?? 0) & 0x400) &&
        left.size === right.size &&
        left.dev === right.dev &&
        left.ino === right.ino &&
        left.mtime?.getTime() === right.mtime?.getTime() &&
        left.birthtime?.getTime() === right.birthtime?.getTime()
    );
}

export function nativeBasename(path: string): string {
    return path.split(/[/\\]/).pop() ?? path;
}

export function nativeExtension(path: string): string {
    return nativeBasename(path).split('.').pop()?.toLocaleLowerCase() ?? '';
}

export async function nativeFileSource(
    path: string,
    extensions: ReadonlySet<string>,
    mediaType: string,
): Promise<ClientUploadSource> {
    if (!extensions.has(nativeExtension(path))) throw new Error(`Unsupported file type: ${nativeBasename(path)}`);
    return createNativeFileSource(path, mediaType, maximumNativeFileBytes);
}

export async function nativeRawFileSource(
    path: string,
    mediaType = 'application/octet-stream',
): Promise<ClientUploadSource> {
    return createNativeFileSource(path, mediaType, maximumChunkBytes);
}

async function createNativeFileSource(
    path: string,
    mediaType: string,
    chunkLimit: number,
): Promise<ClientUploadSource> {
    const info = await lstat(path);
    if (!info.isFile || info.isSymlink || (info.fileAttributes ?? 0) & 0x400)
        throw new Error(`Selected path is not a regular file: ${nativeBasename(path)}`);
    if (!Number.isSafeInteger(info.size) || info.size < 0 || info.size > maximumNativeFileBytes) {
        throw new Error(`Selected file exceeds the native admission limit: ${nativeBasename(path)}`);
    }
    return {
        name: nativeBasename(path),
        type: mediaType,
        size: info.size,
        readChunk: async (start, end) => {
            if (
                !Number.isSafeInteger(start) ||
                !Number.isSafeInteger(end) ||
                start < 0 ||
                end <= start ||
                start >= info.size
            ) {
                throw new Error('Invalid native upload range');
            }
            const changed = () => new Error(`Selected file changed before or during upload: ${nativeBasename(path)}`);
            if (!sameNativeFileIdentity(info, await lstat(path))) throw changed();
            const handle = await open(path, { read: true });
            try {
                const current = await handle.stat();
                if (!sameNativeFileIdentity(info, current)) throw changed();
                await handle.seek(start, SeekMode.Start);
                const bytes = new Uint8Array(Math.min(end, info.size, start + chunkLimit) - start);
                let offset = 0;
                while (offset < bytes.length) {
                    const count = await handle.read(bytes.subarray(offset));
                    if (count === null || count <= 0) {
                        throw new Error(`Selected file ended during upload: ${nativeBasename(path)}`);
                    }
                    offset += count;
                }
                if (
                    !sameNativeFileIdentity(info, await handle.stat()) ||
                    !sameNativeFileIdentity(info, await lstat(path))
                )
                    throw changed();
                return new Blob([bytes], { type: mediaType });
            } finally {
                await handle.close();
            }
        },
    };
}
