import type { WorkspaceView } from './types';

export interface PlaybackRowVisibility {
    view: WorkspaceView;
    playingSampleBankId: string;
    playingObjectId: string | null;
    visibleSampleBankIds: readonly string[];
    visibleSampleIds: readonly string[];
    visibleWaveDataIds: readonly string[];
}

export function matchesSearch(name: string, query: string): boolean {
    return name.toLocaleLowerCase().includes(query.trim().toLocaleLowerCase());
}

export function playbackRowVisible(context: PlaybackRowVisibility): boolean {
    if (context.view === 'programs') return false;
    const objectId = context.playingObjectId;
    return (
        (Boolean(context.playingSampleBankId) && context.visibleSampleBankIds.includes(context.playingSampleBankId)) ||
        (objectId !== null &&
            objectId.length > 0 &&
            (context.visibleSampleIds.includes(objectId) || context.visibleWaveDataIds.includes(objectId)))
    );
}
