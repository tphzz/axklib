import { invoke } from '@tauri-apps/api/core';

export interface LocalMediaDestination {
    candidateId: string;
    filename: string;
}

export function selectLocalMediaDestination(suggestedName: string): Promise<LocalMediaDestination | null> {
    return invoke('select_local_media_destination', { suggestedName });
}

export function saveRetainedMedia(candidateId: string, contentPath: string, expectedSize: number): Promise<void> {
    return invoke('save_retained_media', { candidateId, contentPath, expectedSize });
}
