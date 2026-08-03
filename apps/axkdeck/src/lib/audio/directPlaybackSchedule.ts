import { isForwardLoop, isReversePlayback, type PlaybackTimeline } from './playbackTimeline';

export type DirectPlaybackMode = 'interactive' | 'forced-one-shot';
export type DirectPlaybackKind = 'natural-one-shot' | 'declared-loop' | 'short-one-shot-repeat' | 'forced-one-shot';

export interface DirectPlaybackSchedule<T extends PlaybackTimeline> {
    kind: DirectPlaybackKind;
    loop: boolean;
    loopStartSeconds: number;
    loopEndSeconds: number;
    stopAfterSeconds: number | null;
    timeline: T;
}

const maximumShortOneShotSeconds = 0.05;
const shortOneShotPreviewSeconds = 0.5;

function oneShotSchedule<T extends PlaybackTimeline>(kind: DirectPlaybackKind, timeline: T): DirectPlaybackSchedule<T> {
    return {
        kind,
        loop: false,
        loopStartSeconds: 0,
        loopEndSeconds: 0,
        stopAfterSeconds: null,
        timeline,
    };
}

export function planDirectPlayback<T extends PlaybackTimeline>(
    timeline: T,
    decodedDurationSeconds: number,
    mode: DirectPlaybackMode,
): DirectPlaybackSchedule<T> {
    if (mode === 'forced-one-shot') {
        return oneShotSchedule('forced-one-shot', {
            ...timeline,
            loopMode: 0,
            loopStartFrame: 0,
            loopLengthFrames: 0,
        });
    }
    if (isForwardLoop(timeline)) {
        const loopStartSeconds = timeline.loopStartFrame / timeline.sampleRate;
        const declaredLoopEndSeconds = (timeline.loopStartFrame + timeline.loopLengthFrames) / timeline.sampleRate;
        const loopEndSeconds = Math.min(declaredLoopEndSeconds, decodedDurationSeconds);
        if (loopEndSeconds <= loopStartSeconds) return oneShotSchedule('natural-one-shot', timeline);
        return {
            kind: 'declared-loop',
            loop: true,
            loopStartSeconds,
            loopEndSeconds,
            stopAfterSeconds: null,
            timeline,
        };
    }
    const forwardOneShot = timeline.loopMode === 0 || timeline.loopMode === 4;
    if (
        forwardOneShot &&
        !isReversePlayback(timeline) &&
        Number.isFinite(decodedDurationSeconds) &&
        decodedDurationSeconds > 0 &&
        decodedDurationSeconds <= maximumShortOneShotSeconds
    ) {
        return {
            kind: 'short-one-shot-repeat',
            loop: true,
            loopStartSeconds: 0,
            loopEndSeconds: decodedDurationSeconds,
            stopAfterSeconds: shortOneShotPreviewSeconds,
            timeline: {
                ...timeline,
                loopMode: 1,
                loopStartFrame: 0,
                loopLengthFrames: timeline.frameCount,
            },
        };
    }
    return oneShotSchedule('natural-one-shot', timeline);
}
