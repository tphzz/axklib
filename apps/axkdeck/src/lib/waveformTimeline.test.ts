import { describe, expect, it } from 'vitest';
import { waveformContentRatio, waveformFrameWindow } from './waveformTimeline';

describe('waveformContentRatio', () => {
    it('reserves a blank tail for a shorter member on a shared timeline', () => {
        expect(waveformContentRatio(22_050, 44_100)).toBe(0.5);
    });

    it('clamps invalid and oversized frame ranges', () => {
        expect(waveformContentRatio(44_100, 22_050)).toBe(1);
        expect(waveformContentRatio(0, 44_100)).toBe(0);
        expect(waveformContentRatio(44_100, 0)).toBe(1);
    });
});

describe('waveformFrameWindow', () => {
    it('maps and clamps a frame window onto the stored timeline', () => {
        expect(waveformFrameWindow(100, 800, 1_000)).toEqual({ startRatio: 0.1, endRatio: 0.9 });
        expect(waveformFrameWindow(-100, 300, 1_000)).toEqual({ startRatio: 0, endRatio: 0.2 });
        expect(waveformFrameWindow(900, 300, 1_000)).toEqual({ startRatio: 0.9, endRatio: 1 });
        expect(waveformFrameWindow(0, 0, 1_000)).toBeNull();
    });
});
