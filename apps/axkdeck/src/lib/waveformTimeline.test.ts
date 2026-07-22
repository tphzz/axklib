import { describe, expect, it } from 'vitest';
import { waveformContentRatio } from './waveformTimeline';

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
