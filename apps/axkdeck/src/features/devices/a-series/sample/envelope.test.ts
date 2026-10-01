import { describe, expect, it } from 'vitest';
import { sampleEnvelope, envelopeDuration, envelopeRate } from './envelope';

describe('sample envelope stage models', () => {
    it('does not pad maximum-rate transitions into visibly slow ramps', () => {
        const points = sampleEnvelope('aeg', {
            'aeg.attack_rate': 127,
            'aeg.decay_rate': 127,
            'aeg.release_rate': 127,
            'aeg.sustain_level': 127,
        });
        expect(points[1]!.x / 127).toBeLessThan(0.01);
        expect(points[3]!.x).toBe(points[4]!.x);
    });
    it.each(['aeg', 'feg', 'peg'] as const)('%s spans the full stage axis with a sustain interval', (kind) => {
        const points = sampleEnvelope(kind, {
            [`${kind}.init_level`]: -40,
            [`${kind}.attack_level`]: 100,
            [`${kind}.sustain_level`]: 64,
            [`${kind}.release_level`]: -20,
        });
        expect(points[0]!.x).toBe(0);
        expect(points.at(-1)!.x).toBe(127);
        expect(points[2]!.y).toBe(points[3]!.y);
        expect(points[3]!.label).toBe('Note off');
        expect(points[0]!.y).toBe(kind === 'aeg' ? 0 : -40);
        expect(points.at(-1)!.y).toBe(kind === 'aeg' ? 0 : -20);
    });
    it('draws an immediate maximum level and hold interval in amplitude Hold mode', () => {
        const points = sampleEnvelope('aeg', { 'aeg.attack_mode': 1, 'aeg.sustain_level': 80 });
        expect(points.slice(0, 2).map((point) => point.y)).toEqual([127, 127]);
        expect(points[1]!.label).toBe('Hold');
        expect(points.filter((point) => point.parameter).map((point) => point.parameter)).toEqual([
            'aeg.sustain_level',
        ]);
        expect(points[1]!.rateParameter).toBe('aeg.attack_rate');
    });
    it('maps every native rate to an invertible relative duration', () => {
        for (let rate = 0; rate <= 127; rate++) expect(envelopeRate(envelopeDuration(rate))).toBe(rate);
        expect(envelopeDuration(127)).toBeLessThan(envelopeDuration(0));
    });
    it.each(['aeg', 'feg', 'peg'] as const)('%s responds to all rates and still spans the full width', (kind) => {
        const baseline = sampleEnvelope(kind, { [`${kind}.sustain_level`]: 64 });
        for (const stage of ['attack', 'decay', 'release']) {
            const changed = sampleEnvelope(kind, { [`${kind}.sustain_level`]: 64, [`${kind}.${stage}_rate`]: 0 });
            expect(changed.map((point) => point.x)).not.toEqual(baseline.map((point) => point.x));
            expect(changed[0]!.x).toBe(0);
            expect(changed.at(-1)!.x).toBe(127);
        }
    });
});
