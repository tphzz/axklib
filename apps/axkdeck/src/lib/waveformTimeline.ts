export interface WaveformTimeline {
    sampleRate: number;
    storedFrameCount: number;
    playbackStartFrame: number;
    playbackLengthFrames: number;
    loopStartFrame: number;
    loopLengthFrames: number;
    displayDurationSeconds: number;
}

export function waveformContentRatio(timeline: WaveformTimeline): number {
    if (timeline.storedFrameCount <= 0) return 0;
    if (timeline.sampleRate <= 0 || timeline.displayDurationSeconds <= 0) return 1;
    return Math.max(0, Math.min(1, timeline.storedFrameCount / timeline.sampleRate / timeline.displayDurationSeconds));
}

export interface WaveformFrameWindow {
    startRatio: number;
    endRatio: number;
}

export function waveformFrameWindow(
    timeline: WaveformTimeline,
    startFrame: number,
    lengthFrames: number,
): WaveformFrameWindow | null {
    if (timeline.sampleRate <= 0 || timeline.displayDurationSeconds <= 0 || lengthFrames <= 0) return null;
    const start = Math.max(0, Math.min(timeline.storedFrameCount, startFrame));
    const end = Math.max(start, Math.min(timeline.storedFrameCount, startFrame + lengthFrames));
    if (end <= start) return null;
    return {
        startRatio: Math.min(1, start / timeline.sampleRate / timeline.displayDurationSeconds),
        endRatio: Math.min(1, end / timeline.sampleRate / timeline.displayDurationSeconds),
    };
}

export function waveformPlayheadRatio(
    timeline: WaveformTimeline,
    playbackFrame: number,
    playbackSampleRate: number,
): number {
    if (timeline.sampleRate <= 0 || playbackSampleRate <= 0 || timeline.displayDurationSeconds <= 0) return 0;
    const playbackEndFrame = timeline.playbackStartFrame + timeline.playbackLengthFrames;
    const laneFrame = Math.max(
        timeline.playbackStartFrame,
        Math.min(
            playbackEndFrame,
            timeline.playbackStartFrame + (Math.max(0, playbackFrame) * timeline.sampleRate) / playbackSampleRate,
        ),
    );
    return Math.max(0, Math.min(1, laneFrame / timeline.sampleRate / timeline.displayDurationSeconds));
}
