import { invoke } from '@tauri-apps/api/core';

export interface LocalDirectoryExportDestination {
    candidateId: string;
    directoryName: string;
}

export function selectLocalDirectoryExportDestination(
    suggestedName: string,
    exportLabel: 'SFZ' | 'WAV' | 'MIDI' | 'Packages' | 'Floppies' | 'Files',
): Promise<LocalDirectoryExportDestination | null> {
    return invoke('select_local_directory_export_destination', { suggestedName, exportLabel });
}

export function saveRetainedDirectoryExport(
    candidateId: string,
    contentPath: string,
    expectedSize: number,
): Promise<void> {
    return invoke('save_retained_directory_export', { candidateId, contentPath, expectedSize });
}

export function cancelRetainedDirectoryExport(candidateId: string): Promise<void> {
    return invoke('cancel_retained_directory_export', { candidateId });
}
