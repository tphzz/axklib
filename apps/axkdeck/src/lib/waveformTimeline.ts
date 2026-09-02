export function waveformContentRatio(sourceFrameCount: number, timelineFrameCount: number): number {
    if (timelineFrameCount <= 0) return 1;
    return Math.max(0, Math.min(1, sourceFrameCount / timelineFrameCount));
}

export interface WaveformFrameWindow {
    startRatio: number;
    endRatio: number;
}

export function waveformFrameWindow(
    startFrame: number,
    lengthFrames: number,
    timelineFrameCount: number,
): WaveformFrameWindow | null {
    if (timelineFrameCount <= 0 || lengthFrames <= 0) return null;
    const start = Math.max(0, Math.min(timelineFrameCount, startFrame));
    const end = Math.max(start, Math.min(timelineFrameCount, startFrame + lengthFrames));
    if (end <= start) return null;
    return { startRatio: start / timelineFrameCount, endRatio: end / timelineFrameCount };
}
