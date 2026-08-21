import { invoke } from '@tauri-apps/api/core';

export interface LocalPackageDestination {
    candidateId: string;
    filename: string;
}

export function selectLocalPackage(preferredPath: string | null): Promise<string | null> {
    return invoke('select_local_package', { preferredPath });
}

export function selectLocalPackages(preferredPath: string | null): Promise<string[]> {
    return invoke('select_local_packages', { preferredPath });
}

export function selectLocalPackageDestination(suggestedName: string): Promise<LocalPackageDestination | null> {
    return invoke('select_local_package_destination', { suggestedName });
}

export function saveRetainedPackage(candidateId: string, contentPath: string, expectedSize: number): Promise<void> {
    return invoke('save_retained_package', { candidateId, contentPath, expectedSize });
}
