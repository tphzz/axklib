import { describe, expect, it } from 'vitest';

import { playbackFrameAtTime, playbackOffsetSeconds } from './playbackTimeline';

describe('buffered playback timeline', () => {
    it('maps forward and looped playback to original source frames', () => {
        const oneShot = { frameCount: 100, sampleRate: 10, loopMode: 0, loopStartFrame: 0, loopLengthFrames: 0 };
        expect(playbackFrameAtTime(oneShot, 20, 2.5)).toBe(45);

        const looped = { ...oneShot, loopMode: 1, loopStartFrame: 20, loopLengthFrames: 30 };
        expect(playbackFrameAtTime(looped, 0, 5.5)).toBe(25);
    });

    it('maps reversed buffers back to descending original frames', () => {
        const reversed = { frameCount: 100, sampleRate: 10, loopMode: 3, loopStartFrame: 0, loopLengthFrames: 0 };
        expect(playbackOffsetSeconds(reversed, 99)).toBe(0);
        expect(playbackOffsetSeconds(reversed, 49)).toBe(5);
        expect(playbackFrameAtTime(reversed, 99, 2.5)).toBe(74);
    });

    it('reports the end of one-shot playback', () => {
        const oneShot = { frameCount: 10, sampleRate: 10, loopMode: 0, loopStartFrame: 0, loopLengthFrames: 0 };
        expect(playbackFrameAtTime(oneShot, 0, 1)).toBeNull();
    });
});
