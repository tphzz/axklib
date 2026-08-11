export function waveformContentRatio(sourceFrameCount: number, timelineFrameCount: number): number {
    if (timelineFrameCount <= 0) return 1;
    return Math.max(0, Math.min(1, sourceFrameCount / timelineFrameCount));
}
