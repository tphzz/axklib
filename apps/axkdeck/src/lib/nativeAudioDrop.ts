import { getCurrentWebview, type DragDropEvent } from '@tauri-apps/api/webview';
import type { UnlistenFn } from '@tauri-apps/api/event';
import { lstat, open, SeekMode } from '@tauri-apps/plugin-fs';
import { audioExtensions } from './audioImport';
import type { ClientUploadSource } from './clientUploadSource';

export interface NativeDropPosition {
    x: number;
    y: number;
}

interface NativeAudioDropCallbacks {
    onHover: (active: boolean, position?: NativeDropPosition) => void;
    onDrop: (
        files: ClientUploadSource[],
        position: NativeDropPosition,
        droppedPathCount: number,
    ) => void | Promise<void>;
    onError: (reason: unknown) => void;
}

const supportedExtensions = new Set<string>(audioExtensions);
const maximumNativeDropFileBytes = 4 * 1024 * 1024 * 1024;
const maximumNativeDropTotalBytes = 8 * 1024 * 1024 * 1024;

function basename(path: string): string {
    return path.split(/[/\\]/).pop() ?? path;
}

function extension(path: string): string {
    return basename(path).split('.').pop()?.toLocaleLowerCase() ?? '';
}

function mediaType(path: string): string {
    switch (extension(path)) {
        case 'wav':
        case 'wave':
            return 'audio/wav';
        case 'flac':
            return 'audio/flac';
        case 'aif':
        case 'aiff':
            return 'audio/aiff';
        default:
            return 'application/octet-stream';
    }
}

function supported(path: string): boolean {
    return supportedExtensions.has(extension(path));
}

async function droppedFile(path: string): Promise<ClientUploadSource> {
    const info = await lstat(path);
    if (!info.isFile || info.isSymlink) throw new Error(`Dropped path is not a regular file: ${basename(path)}`);
    if (!Number.isSafeInteger(info.size) || info.size < 0 || info.size > maximumNativeDropFileBytes) {
        throw new Error(`Dropped file exceeds the native admission limit: ${basename(path)}`);
    }
    return {
        name: basename(path),
        type: mediaType(path),
        size: info.size,
        readChunk: async (start, end) => {
            if (!Number.isSafeInteger(start) || !Number.isSafeInteger(end) || start < 0 || end <= start) {
                throw new Error('Invalid native upload range');
            }
            const handle = await open(path, { read: true });
            try {
                const current = await handle.stat();
                if (!current.isFile || current.size !== info.size) {
                    throw new Error(`Dropped file changed before upload: ${basename(path)}`);
                }
                await handle.seek(start, SeekMode.Start);
                const bytes = new Uint8Array(Math.min(end, info.size) - start);
                let offset = 0;
                while (offset < bytes.length) {
                    const count = await handle.read(bytes.subarray(offset));
                    if (count === null || count <= 0) {
                        throw new Error(`Dropped file ended during upload: ${basename(path)}`);
                    }
                    offset += count;
                }
                return new Blob([bytes], { type: mediaType(path) });
            } finally {
                await handle.close();
            }
        },
    };
}

async function admittedFiles(paths: readonly string[]): Promise<ClientUploadSource[]> {
    const result: ClientUploadSource[] = [];
    let aggregateBytes = 0;
    for (const path of paths.filter(supported)) {
        const file = await droppedFile(path);
        aggregateBytes += file.size;
        if (!Number.isSafeInteger(aggregateBytes) || aggregateBytes > maximumNativeDropTotalBytes) {
            throw new Error('Dropped files exceed the aggregate native admission limit');
        }
        result.push(file);
    }
    return result;
}

export async function listenForNativeAudioDrops(callbacks: NativeAudioDropCallbacks): Promise<UnlistenFn> {
    let hoveringSupportedFiles = false;
    return getCurrentWebview().onDragDropEvent((event) => {
        const payload: DragDropEvent = event.payload;
        if (payload.type === 'enter') {
            hoveringSupportedFiles = payload.paths.some(supported);
            callbacks.onHover(hoveringSupportedFiles, payload.position);
            return;
        }
        if (payload.type === 'over') {
            callbacks.onHover(hoveringSupportedFiles, payload.position);
            return;
        }
        if (payload.type === 'leave') {
            hoveringSupportedFiles = false;
            callbacks.onHover(false);
            return;
        }

        hoveringSupportedFiles = false;
        callbacks.onHover(false, payload.position);
        void admittedFiles(payload.paths)
            .then((files) => callbacks.onDrop(files, payload.position, payload.paths.length))
            .catch(callbacks.onError);
    });
}
