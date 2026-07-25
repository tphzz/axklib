import { getCurrentWebview, type DragDropEvent } from '@tauri-apps/api/webview';
import type { UnlistenFn } from '@tauri-apps/api/event';
import { audioExtensions } from './audioImport';
import type { ClientUploadSource } from './clientUploadSource';
import { nativeExtension, nativeFileSource } from './nativeFileSource';

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

function mediaType(path: string): string {
    switch (nativeExtension(path)) {
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
    return supportedExtensions.has(nativeExtension(path));
}

async function admittedFiles(paths: readonly string[]): Promise<ClientUploadSource[]> {
    const result: ClientUploadSource[] = [];
    let aggregateBytes = 0;
    for (const path of paths.filter(supported)) {
        const file = await nativeFileSource(path, supportedExtensions, mediaType(path));
        if (file.size > maximumNativeDropFileBytes) {
            throw new Error(`Dropped file exceeds the native admission limit: ${file.name}`);
        }
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
