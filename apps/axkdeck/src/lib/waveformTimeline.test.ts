import { describe, expect, it } from 'vitest';
import {
    waveformContentRatio,
    waveformFrameWindow,
    waveformPlayheadRatio,
    type WaveformTimeline,
} from './waveformTimeline';

const timeline: WaveformTimeline = {
    sampleRate: 1_000,
    storedFrameCount: 1_000,
    playbackStartFrame: 100,
    playbackLengthFrames: 800,
    loopStartFrame: 250,
    loopLengthFrames: 500,
    displayDurationSeconds: 1,
};

describe('waveformContentRatio', () => {
    it('reserves a blank tail for a shorter member on a shared timeline', () => {
        expect(waveformContentRatio({ ...timeline, storedFrameCount: 500 })).toBe(0.5);
    });

    it('aligns different sample rates by duration and clamps invalid ranges', () => {
        expect(waveformContentRatio({ ...timeline, sampleRate: 500, storedFrameCount: 500 })).toBe(1);
        expect(waveformContentRatio({ ...timeline, storedFrameCount: 2_000 })).toBe(1);
        expect(waveformContentRatio({ ...timeline, storedFrameCount: 0 })).toBe(0);
        expect(waveformContentRatio({ ...timeline, displayDurationSeconds: 0 })).toBe(1);
    });
});

describe('waveformFrameWindow', () => {
    it('maps and clamps a frame window onto the stored timeline', () => {
        expect(waveformFrameWindow(timeline, 100, 800)).toEqual({ startRatio: 0.1, endRatio: 0.9 });
        expect(waveformFrameWindow(timeline, -100, 300)).toEqual({ startRatio: 0, endRatio: 0.2 });
        expect(waveformFrameWindow(timeline, 900, 300)).toEqual({ startRatio: 0.9, endRatio: 1 });
        expect(waveformFrameWindow(timeline, 0, 0)).toBeNull();
    });
});

describe('waveformPlayheadRatio', () => {
    it('maps a playback-relative cursor onto a lane with a different sample rate', () => {
        expect(
            waveformPlayheadRatio(
                {
                    ...timeline,
                    sampleRate: 500,
                    storedFrameCount: 500,
                    playbackStartFrame: 50,
                    playbackLengthFrames: 400,
                },
                250,
                1_000,
            ),
        ).toBe(0.35);
    });
});
