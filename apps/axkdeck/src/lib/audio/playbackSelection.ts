export function inspectorSelectionStopsPlayback(
    activePlaybackObjectId: string | null,
    nextInspectorObjectId: string,
): boolean {
    return activePlaybackObjectId !== null && activePlaybackObjectId !== nextInspectorObjectId;
}
