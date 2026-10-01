import { describe, expect, it } from 'vitest';
import { calculatedLoopTempo, endDisplay, endFrame } from './sampleInfoGeometry';

describe('Sample Info End Type geometry', () => {
    it('keeps Address absolute regardless of the corresponding start', () => {
        expect(endDisplay(132300, 44100, 44100, 12000, 0)).toBe(132300);
        expect(endDisplay(132300, 88200, 44100, 12000, 0)).toBe(132300);
        expect(endFrame(132300, 44100, 44100, 12000, 0)).toBe(132300);
        expect(endFrame(132300, 88200, 44100, 12000, 0)).toBe(132300);
    });

    it.each([
        { type: 1, wave: 88200, loop: 44100 },
        { type: 2, wave: 2, loop: 1 },
        { type: 3, wave: 4, loop: 2 },
    ])('uses independent wave and loop starts for End Type $type', ({ type, wave, loop }) => {
        expect(endDisplay(132300, 44100, 44100, 12000, type)).toBe(wave);
        expect(endDisplay(132300, 88200, 44100, 12000, type)).toBe(loop);
        expect(endFrame(wave, 44100, 44100, 12000, type)).toBe(132300);
        expect(endFrame(loop, 88200, 44100, 12000, type)).toBe(132300);
    });

    it.each([0, 1, 2, 3])('round-trips large native addresses for End Type %i', (type) => {
        for (const rate of [22050, 44100, 48000]) {
            for (const tempo of [8000, 12345, 15999]) {
                for (const [start, frame] of [
                    [0, 16777216],
                    [16700001, 16777215],
                    [1234567, 1234568],
                    [7654321, 7654321],
                ]) {
                    const display = endDisplay(frame!, start!, rate, tempo, type);
                    expect(display).toBeDefined();
                    expect(endFrame(display!, start!, rate, tempo, type)).toBe(frame);
                }
            }
        }
    });

    it('rounds edited values to the nearest source frame', () => {
        expect(endFrame(100.49, 20, 44100, 12000, 0)).toBe(100);
        expect(endFrame(100.5, 20, 44100, 12000, 0)).toBe(101);
        expect(endFrame(10.49, 20, 44100, 12000, 1)).toBe(30);
        expect(endFrame(10.5, 20, 44100, 12000, 1)).toBe(31);
        expect(endFrame(1.5 / 44100, 20, 44100, 12000, 2)).toBe(22);
        expect(endFrame(3 / 44100, 20, 44100, 12000, 3)).toBe(22);
    });

    it.each([0, -1, NaN, Infinity])('rejects invalid sample rate %s only where it is needed', (rate) => {
        expect(endDisplay(120, 20, rate, 12000, 0)).toBe(120);
        expect(endDisplay(120, 20, rate, 12000, 1)).toBe(100);
        expect(endFrame(120, 20, rate, 12000, 0)).toBe(120);
        expect(endFrame(100, 20, rate, 12000, 1)).toBe(120);
        for (const type of [2, 3]) {
            expect(endDisplay(120, 20, rate, 12000, type)).toBeUndefined();
            expect(endFrame(1, 20, rate, 12000, type)).toBeUndefined();
        }
    });

    it.each([0, -1, NaN, Infinity])('requires a valid tempo %s only for Beat', (tempo) => {
        expect(endDisplay(120, 20, 100, tempo, 0)).toBe(120);
        expect(endDisplay(120, 20, 100, tempo, 1)).toBe(100);
        expect(endDisplay(120, 20, 100, tempo, 2)).toBe(1);
        expect(endFrame(120, 20, 100, tempo, 0)).toBe(120);
        expect(endFrame(100, 20, 100, tempo, 1)).toBe(120);
        expect(endFrame(1, 20, 100, tempo, 2)).toBe(120);
        expect(endDisplay(120, 20, 100, tempo, 3)).toBeUndefined();
        expect(endFrame(1, 20, 100, tempo, 3)).toBeUndefined();
    });

    it('changes only the representation, never stored boundaries or tempo', () => {
        const values = Object.freeze({
            start: 1000,
            end: 45100,
            loopStart: 23050,
            loopEnd: 45100,
            rate: 44100,
            tempo: 12000,
        });
        const before = { ...values };
        for (const type of [1, 2, 3, 0]) {
            const wave = endDisplay(values.end, values.start, values.rate, values.tempo, type);
            const loop = endDisplay(values.loopEnd, values.loopStart, values.rate, values.tempo, type);
            expect(endFrame(wave!, values.start, values.rate, values.tempo, type)).toBe(values.end);
            expect(endFrame(loop!, values.loopStart, values.rate, values.tempo, type)).toBe(values.loopEnd);
        }
        expect(values).toEqual(before);
    });
});

describe('Sample Info tempo calculation', () => {
    it.each([0.25, 1, 2, 4, 8])('normalizes the default four-beat calculation for %s seconds', (seconds) => {
        expect(calculatedLoopTempo(44100, 44100 * seconds)).toBe(12000);
    });

    it('normalizes the octave boundary into the native tempo range', () => {
        expect(calculatedLoopTempo(44100, 44100 * 3)).toBe(8000);
        expect(calculatedLoopTempo(44100, 44100 * 1.5)).toBe(8000);
        expect(calculatedLoopTempo(44100, 44100 * 6)).toBe(8000);
    });

    it('respects an explicit beat count without normalizing it', () => {
        expect(calculatedLoopTempo(44100, 44100, 2, false)).toBe(12000);
        expect(calculatedLoopTempo(44100, 44100, 1, false)).toBeUndefined();
        expect(calculatedLoopTempo(44100, 44100, 4, false)).toBeUndefined();
        expect(calculatedLoopTempo(44100, 44100, 8, false)).toBeUndefined();
    });

    it('rounds to native hundredths and admits both native endpoints', () => {
        expect(calculatedLoopTempo(44100, (60 * 44100 * 4) / 123.456, 4, false)).toBe(12346);
        expect(calculatedLoopTempo(44100, (60 * 44100 * 4) / 80, 4, false)).toBe(8000);
        expect(calculatedLoopTempo(44100, (60 * 44100 * 4) / 159.99, 4, false)).toBe(15999);
        expect(calculatedLoopTempo(44100, (60 * 44100 * 4) / 160, 4, false)).toBeUndefined();
    });

    it.each([0, -1, NaN, Infinity])('rejects invalid calculation inputs %s', (invalid) => {
        expect(calculatedLoopTempo(invalid, 44100)).toBeUndefined();
        expect(calculatedLoopTempo(44100, invalid)).toBeUndefined();
        expect(calculatedLoopTempo(44100, 44100, invalid)).toBeUndefined();
        expect(calculatedLoopTempo(44100, 44100, invalid, false)).toBeUndefined();
    });
});
