import { describe, expect, it } from 'vitest';

import { planDirectPlayback } from './directPlaybackSchedule';

const baseTimeline = {
    frameCount: 260,
    sampleRate: 44_100,
    loopMode: 0,
    loopStartFrame: 0,
    loopLengthFrames: 0,
};

describe('direct playback scheduling', () => {
    it('repeats a short forward one-shot for a bounded audible preview', () => {
        const naturalDurationSeconds = baseTimeline.frameCount / baseTimeline.sampleRate;

        expect(planDirectPlayback(baseTimeline, naturalDurationSeconds, 'interactive')).toEqual({
            kind: 'short-one-shot-repeat',
            loop: true,
            loopStartSeconds: 0,
            loopEndSeconds: naturalDurationSeconds,
            stopAfterSeconds: 0.5,
            timeline: { ...baseTimeline, loopMode: 1, loopLengthFrames: baseTimeline.frameCount },
        });
    });

    it('includes exactly 50 ms but leaves longer one-shots natural', () => {
        const boundary = { ...baseTimeline, frameCount: 2_205 };
        const longer = { ...baseTimeline, frameCount: 2_206 };

        expect(planDirectPlayback(boundary, 0.05, 'interactive').kind).toBe('short-one-shot-repeat');
        expect(planDirectPlayback(longer, longer.frameCount / longer.sampleRate, 'interactive')).toMatchObject({
            kind: 'natural-one-shot',
            loop: false,
            stopAfterSeconds: null,
            timeline: longer,
        });
    });

    it('preserves declared loop points without imposing a stop', () => {
        const looped = { ...baseTimeline, frameCount: 4_410, loopMode: 1, loopStartFrame: 100, loopLengthFrames: 300 };

        expect(planDirectPlayback(looped, 0.1, 'interactive')).toEqual({
            kind: 'declared-loop',
            loop: true,
            loopStartSeconds: 100 / 44_100,
            loopEndSeconds: 400 / 44_100,
            stopAfterSeconds: null,
            timeline: looped,
        });
    });

    it('does not reinterpret reverse playback or forced one-shots', () => {
        const reversed = { ...baseTimeline, loopMode: 3 };
        const looped = { ...baseTimeline, loopMode: 1, loopStartFrame: 10, loopLengthFrames: 100 };

        expect(planDirectPlayback(reversed, 0.005, 'interactive')).toMatchObject({
            kind: 'natural-one-shot',
            loop: false,
            stopAfterSeconds: null,
            timeline: reversed,
        });
        expect(planDirectPlayback(looped, 0.005, 'forced-one-shot')).toMatchObject({
            kind: 'forced-one-shot',
            loop: false,
            stopAfterSeconds: null,
            timeline: { ...looped, loopMode: 0, loopStartFrame: 0, loopLengthFrames: 0 },
        });
    });
});
