import { describe, expect, it } from 'vitest';
import { mappingVoiceValues, matchesMapping } from './playbackPlan';

describe('mapping audition resolution', () => {
    it('matches inclusive key and velocity layers without changing the selected mapping', () => {
        const mapping = { low: 36, high: 84, velocityLow: 40, velocityHigh: 90 };
        expect(matchesMapping(mapping, 36, 40)).toBe(true);
        expect(matchesMapping(mapping, 84, 90)).toBe(true);
        expect(matchesMapping(mapping, 60, 39)).toBe(false);
        expect(matchesMapping({ ...mapping, empty: true }, 60, 64)).toBe(false);
    });
    it('applies assignment root shifts and saturated preview tuning, level and pan without mutating stored values', () => {
        const source = { root_key: 60, coarse_tune: 3, fine_tune_cents: 30, level: 120, pan: 60 };
        const values = mappingVoiceValues(
            source,
            {
                'assignments.2.key_shift': 12,
                'assignments.2.level_offset': 30,
                'assignments.2.pan_offset': 15,
                'assignments.2.coarse_tune_offset': 5,
                'assignments.2.fine_tune_offset': 10,
            },
            2,
        );
        expect(values).toMatchObject({ root_key: 72, coarse_tune: 8, fine_tune_cents: 40, level: 127, pan: 63 });
        expect(source.root_key).toBe(60);
    });
    it('preserves the wider native A3000 tuning domain when applying Program offsets', () => {
        expect(
            mappingVoiceValues({ coarse_tune: 100 }, { 'assignments.0.coarse_tune_offset': 10 }, 0, {
                minimum: -127,
                maximum: 127,
            }).coarse_tune,
        ).toBe(110);
        expect(
            mappingVoiceValues({ coarse_tune: 120 }, { 'assignments.0.coarse_tune_offset': 30 }, 0, {
                minimum: -127,
                maximum: 127,
            }).coarse_tune,
        ).toBe(127);
    });
});
