import { describe, expect, it } from 'vitest';
import { lfoTimeline, lfoBuildup } from './lfoShape';

describe('schematic LFO shapes', () => {
    it.each(['square', 'saw', 'sample-hold'] as const)('keeps an exact end-cycle %s discontinuity vertical', (wave) => {
        const points = lfoTimeline(wave, 128, 0, true);
        const lastEdge = points.filter((point) => point.x === 1);
        expect(new Set(lastEdge.map((point) => point.y)).size).toBe(2);
    });
    it('keeps exact delayed triangle vertices and a separate buildup guide', () => {
        const delay = 54,
            onset = (delay / 127) * 0.35;
        const points = lfoTimeline('triangle', 40, delay, true);
        const firstPeak = points.find((point) => point.y > 0);
        expect(firstPeak?.y).toBe(1);
        expect(points.filter((point) => point.x < onset).every((point) => point.y === 0)).toBe(true);
        expect(lfoBuildup(delay)).toEqual([
            { x: 0, y: 0 },
            { x: onset, y: 0 },
            { x: onset * 2, y: 1 },
            { x: 1, y: 1 },
        ]);
    });
    it.each(['saw', 'square', 'sample-hold'] as const)('uses vertical discontinuities for %s', (wave) => {
        const points = lfoTimeline(wave, 40, 54, true);
        expect(points.some((p, i) => i > 0 && p.x === points[i - 1]!.x && p.y !== points[i - 1]!.y)).toBe(true);
        if (wave === 'square') expect(points.every((p) => [0, -1, 1].includes(p.y))).toBe(true);
    });
    it.each(['saw', 'triangle', 'square', 'sample-hold'] as const)('draws a bounded, repeatable %s shape', (wave) => {
        const points = lfoTimeline(wave, 40, 0, true);
        expect(points[0]!.x).toBe(0);
        expect(points.at(-1)!.x).toBe(1);
        expect(points.some((point) => point.y !== 0)).toBe(true);
        expect(points.every((point) => Math.abs(point.y) <= 1)).toBe(true);
        expect(lfoTimeline(wave, 40, 0, true)).toEqual(points);
    });
    it('shows faster cycles, delayed onset and buildup without changing Sample & Hold speed', () => {
        const slow = lfoTimeline('saw', 1, 0, true);
        const fast = lfoTimeline('saw', 128, 0, true);
        const crossings = (points: typeof slow) =>
            points.filter((point, index) => index && point.y * points[index - 1]!.y < 0).length;
        expect(crossings(fast)).toBeGreaterThan(crossings(slow));
        const delayed = lfoTimeline('triangle', 40, 127, true);
        expect(delayed.filter((point) => point.x < 0.35).every((point) => point.y === 0)).toBe(true);
        expect(delayed.some((point) => point.x > 0.7 && Math.abs(point.y) > 0.8)).toBe(true);
        expect(lfoTimeline('sample-hold', 1, 0, true)).toEqual(lfoTimeline('sample-hold', 128, 0, true));
    });
});
