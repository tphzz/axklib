import type { UnlistenFn } from '@tauri-apps/api/event';
import { getCurrentWebview, type DragDropEvent } from '@tauri-apps/api/webview';
import { audioExtensions, audioMediaType } from './audioImport';
import type { ClientUploadSource } from './clientUploadSource';
import { midiExtensions, midiMediaType } from './midiImport';
import { nativeExtension, nativeFileSource } from './nativeFileSource';
import { tx16wDiskExtensions, tx16wDiskMediaType } from './tx16wImport';

export interface NativeDropPosition {
    x: number;
    y: number;
}

interface NativeMediaDropCallbacks {
    onHover: (paths: readonly string[], position?: NativeDropPosition) => void;
    onDrop: (
        files: ClientUploadSource[],
        position: NativeDropPosition,
        droppedPathCount: number,
    ) => void | Promise<void>;
    onError: (reason: unknown) => void;
}

const supportedExtensions = new Set<string>([...audioExtensions, ...midiExtensions, ...tx16wDiskExtensions]);
const maximumNativeDropFileBytes = 4 * 1024 * 1024 * 1024;
const maximumNativeDropTotalBytes = 8 * 1024 * 1024 * 1024;

function mediaType(path: string): string {
    return audioMediaType(path) ?? midiMediaType(path) ?? tx16wDiskMediaType(path) ?? 'application/octet-stream';
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

export async function listenForNativeMediaDrops(callbacks: NativeMediaDropCallbacks): Promise<UnlistenFn> {
    let hoveringPaths: readonly string[] = [];
    return getCurrentWebview().onDragDropEvent((event) => {
        const payload: DragDropEvent = event.payload;
        if (payload.type === 'enter') {
            hoveringPaths = payload.paths.filter(supported);
            callbacks.onHover(hoveringPaths, payload.position);
            return;
        }
        if (payload.type === 'over') {
            callbacks.onHover(hoveringPaths, payload.position);
            return;
        }
        if (payload.type === 'leave') {
            hoveringPaths = [];
            callbacks.onHover([]);
            return;
        }

        hoveringPaths = [];
        callbacks.onHover([], payload.position);
        void admittedFiles(payload.paths)
            .then((files) => callbacks.onDrop(files, payload.position, payload.paths.length))
            .catch(callbacks.onError);
    });
}
