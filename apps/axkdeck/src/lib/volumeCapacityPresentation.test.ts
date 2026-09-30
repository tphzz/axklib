import { describe, expect, it } from 'vitest';
import { capacityMetric } from './volumeCapacityPresentation';

describe('Compact capacity metrics', () => {
    it('uses one memory unit for the amount and limit', () => {
        expect(capacityMetric(124000, 123456, 786432, 'B')).toBe('121.1 / 768 KiB');
    });
    it('never rounds a lower bound upward', () => {
        expect(capacityMetric(null, 860704, 786432, 'B')).toBe('\u2265 840.5 / 768 KiB');
        expect(capacityMetric(null, 1023, 786432, 'B')).toBe('\u2265 0.9 / 768 KiB');
    });
    it('retains slot counts and distinguishes unknown amounts', () => {
        expect(capacityMetric(null, 419, 2048)).toBe('\u2265 419 / 2,048');
        expect(capacityMetric(0, null, 2048)).toBe('0 / 2,048');
        expect(capacityMetric(null, null, 786432, 'B')).toBe('Not available / 768 KiB');
    });
});
