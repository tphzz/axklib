import { lstat, open, SeekMode } from '@tauri-apps/plugin-fs';
import type { ClientUploadSource } from './clientUploadSource';

const maximumNativeFileBytes = 4 * 1024 * 1024 * 1024;

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
    const info = await lstat(path);
    if (!info.isFile || info.isSymlink) throw new Error(`Selected path is not a regular file: ${nativeBasename(path)}`);
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
            const handle = await open(path, { read: true });
            try {
                const current = await handle.stat();
                if (!current.isFile || current.size !== info.size) {
                    throw new Error(`Selected file changed before upload: ${nativeBasename(path)}`);
                }
                await handle.seek(start, SeekMode.Start);
                const bytes = new Uint8Array(Math.min(end, info.size) - start);
                let offset = 0;
                while (offset < bytes.length) {
                    const count = await handle.read(bytes.subarray(offset));
                    if (count === null || count <= 0) {
                        throw new Error(`Selected file ended during upload: ${nativeBasename(path)}`);
                    }
                    offset += count;
                }
                return new Blob([bytes], { type: mediaType });
            } finally {
                await handle.close();
            }
        },
    };
}
