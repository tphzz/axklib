import type { DragDropEvent } from '@tauri-apps/api/webview';

type Receiver = (event: DragDropEvent, position: { x: number; y: number } | null) => void;
let receiver: Receiver | null = null;

// The application owns one native listener; the mounted Files tree supplies its destination.
export function registerNativeFilesystemDropTarget(target: Receiver): () => void {
    receiver = target;
    return () => {
        if (receiver === target) receiver = null;
    };
}

export function dispatchNativeFilesystemDrop(event: DragDropEvent, scale: number): void {
    if (!Number.isFinite(scale) || scale <= 0) return;
    receiver?.(event, event.type === 'leave' ? null : { x: event.position.x / scale, y: event.position.y / scale });
}
