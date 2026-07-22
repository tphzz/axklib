import { getCurrentWebview, type DragDropEvent } from '@tauri-apps/api/webview';
import type { UnlistenFn } from '@tauri-apps/api/event';
import { readFile } from '@tauri-apps/plugin-fs';
import { audioExtensions } from './audioImport';

export interface NativeDropPosition {
    x: number;
    y: number;
}

interface NativeAudioDropCallbacks {
    onHover: (active: boolean, position?: NativeDropPosition) => void;
    onDrop: (files: File[], position: NativeDropPosition, droppedPathCount: number) => void | Promise<void>;
    onError: (reason: unknown) => void;
}

const supportedExtensions = new Set<string>(audioExtensions);

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

async function droppedFile(path: string): Promise<File> {
    const bytes = await readFile(path);
    return new File([new Uint8Array(bytes).buffer], basename(path), { type: mediaType(path) });
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
        void Promise.all(payload.paths.filter(supported).map(droppedFile))
            .then((files) => callbacks.onDrop(files, payload.position, payload.paths.length))
            .catch(callbacks.onError);
    });
}
