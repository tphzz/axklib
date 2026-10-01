import { describe, expect, it } from 'vitest';
import { sampleFilterStages } from './filterModel';
import { filterResponse } from '../../../object-editor/filterResponse';
describe('schematic filter families', () => {
    it('supports all seventeen native filter types and all parameter extremes', () => {
        for (let type = 0; type <= 16; type++)
            for (const cutoff of [0, 64, 127])
                for (const q of [0, 31])
                    for (const distance of [-63, 0, 63]) {
                        const stages = sampleFilterStages(type, cutoff, q, distance);
                        expect(stages).toHaveLength(type === 0 ? 0 : type >= 10 ? 2 : 1);
                        expect(filterResponse(stages, 1).every((point) => Number.isFinite(point.y))).toBe(true);
                    }
    });
    it('distinguishes pass, rejection, resonance and compound cutoff behavior', () => {
        const low = filterResponse(sampleFilterStages(1, 64, 0, 0), 0);
        const high = filterResponse(sampleFilterStages(3, 64, 0, 0), 0);
        expect(low[0]!.y).toBeGreaterThan(low.at(-1)!.y);
        expect(high[0]!.y).toBeLessThan(high.at(-1)!.y);
        expect(sampleFilterStages(5, 64, 31, 0)[0]!.width).toBeGreaterThan(sampleFilterStages(5, 64, 0, 0)[0]!.width);
        expect(sampleFilterStages(6, 64, 31, 0)[0]!.width).toBeLessThan(sampleFilterStages(6, 64, 0, 0)[0]!.width);
        expect(sampleFilterStages(16, 64, 0, 32)[1]!.cutoff).toBe(96 / 127);
        expect(filterResponse([], 1)).toEqual(filterResponse([], -1));
    });
});
