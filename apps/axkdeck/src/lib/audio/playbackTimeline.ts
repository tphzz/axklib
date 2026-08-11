export interface PlaybackTimeline {
    frameCount: number;
    sampleRate: number;
    loopMode: number;
    loopStartFrame: number;
    loopLengthFrames: number;
}

export function isReversePlayback(timeline: PlaybackTimeline): boolean {
    return timeline.loopMode === 3 || timeline.loopMode === 5;
}

export function isForwardLoop(timeline: PlaybackTimeline): boolean {
    const loopEnd = timeline.loopStartFrame + timeline.loopLengthFrames;
    return (
        (timeline.loopMode === 1 || timeline.loopMode === 2) &&
        timeline.loopLengthFrames > 0 &&
        timeline.loopStartFrame >= 0 &&
        loopEnd <= timeline.frameCount
    );
}

export function initialPlaybackFrame(timeline: PlaybackTimeline): number {
    return isReversePlayback(timeline) ? Math.max(0, timeline.frameCount - 1) : 0;
}

export function playbackOffsetSeconds(timeline: PlaybackTimeline, sourceFrame: number): number {
    if (timeline.frameCount <= 0 || timeline.sampleRate <= 0) return 0;
    let frame = Math.max(0, Math.min(timeline.frameCount - 1, Math.floor(sourceFrame)));
    const loopEnd = timeline.loopStartFrame + timeline.loopLengthFrames;
    if (isForwardLoop(timeline) && frame >= loopEnd) {
        frame = timeline.loopStartFrame + ((frame - timeline.loopStartFrame) % timeline.loopLengthFrames);
    }
    const decodedFrame = isReversePlayback(timeline) ? timeline.frameCount - 1 - frame : frame;
    return decodedFrame / timeline.sampleRate;
}

export function playbackFrameAtTime(
    timeline: PlaybackTimeline,
    startFrame: number,
    elapsedSeconds: number,
): number | null {
    if (timeline.frameCount <= 0 || timeline.sampleRate <= 0) return null;
    const elapsedFrames = Math.floor(Math.max(0, elapsedSeconds) * timeline.sampleRate);
    const clampedStart = Math.max(0, Math.min(timeline.frameCount - 1, Math.floor(startFrame)));
    if (isReversePlayback(timeline)) {
        const frame = clampedStart - elapsedFrames;
        return frame >= 0 ? frame : null;
    }

    let frame = clampedStart + elapsedFrames;
    if (isForwardLoop(timeline)) {
        const loopEnd = timeline.loopStartFrame + timeline.loopLengthFrames;
        if (frame >= loopEnd) {
            frame = timeline.loopStartFrame + ((frame - timeline.loopStartFrame) % timeline.loopLengthFrames);
        }
        return frame;
    }
    return frame < timeline.frameCount ? frame : null;
}
