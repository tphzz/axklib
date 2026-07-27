import { invoke } from '@tauri-apps/api/core';

export interface LocalSfzDestination {
    candidateId: string;
    directoryName: string;
}

export function selectLocalSfzDestination(suggestedName: string): Promise<LocalSfzDestination | null> {
    return invoke('select_local_sfz_destination', { suggestedName });
}

export function saveRetainedSfzExport(candidateId: string, contentPath: string, expectedSize: number): Promise<void> {
    return invoke('save_retained_sfz_export', { candidateId, contentPath, expectedSize });
}
