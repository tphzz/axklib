import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import {
    eqCoefficients,
    eqFrequencyPosition,
    eqFrequencySelection,
    eqResponse,
    eqEffectiveGain,
    eqParameterCoefficients,
    eqParameterResponse,
} from './eqModel';

const references = JSON.parse(readFileSync('../../tests/fixtures/sample-eq-vectors.json', 'utf8')) as {
    name: string;
    type: number;
    frequency: number;
    gain_db: number;
    width_tenths: number;
    coefficients_q13: number[];
    tolerance_q13: number;
}[];
describe('Sample EQ preview model', () => {
    it.each(references)('matches the native reference $name', (reference) => {
        const actual = eqCoefficients(reference.type, reference.frequency, reference.gain_db, reference.width_tenths);
        actual.forEach((value, index) =>
            expect(Math.abs(value - reference.coefficients_q13[index]!)).toBeLessThanOrEqual(reference.tolerance_q13),
        );
    });
    it('roundtrips all supported frequency selections', () => {
        for (let selection = 4; selection <= 58; selection++)
            expect(eqFrequencySelection(eqFrequencyPosition(selection))).toBe(selection);
    });
    it('has a neutral response, shelf-specific limits and width-independent shelves', () => {
        const neutral = eqCoefficients(0, 26, 0, 10);
        for (let x = 0; x <= 1; x += 0.01) expect(Math.abs(eqResponse(neutral, x))).toBeLessThan(0.001);
        expect(eqEffectiveGain(2, 32, 12)).toBe(6);
        expect(eqEffectiveGain(2, 49, 12)).toBe(12);
        expect(eqCoefficients(1, 37, 6, 10)).toEqual(eqCoefficients(1, 37, 6, 120));
        expect(eqCoefficients(0, 37, 6, 10)).not.toEqual(eqCoefficients(0, 37, 6, 120));
    });
    it('uses hysteresis at every adjacent native frequency boundary', () => {
        for (let selection = 4; selection < 58; selection++) {
            const low = eqFrequencyPosition(selection),
                high = eqFrequencyPosition(selection + 1);
            const middle = (low + high) / 2,
                noise = (high - low) * 0.04;
            expect(eqFrequencySelection(middle + noise, selection)).toBe(selection);
            expect(eqFrequencySelection(middle - noise, selection + 1)).toBe(selection + 1);
            expect(eqFrequencySelection(high, selection)).toBe(selection + 1);
            expect(eqFrequencySelection(low, selection + 1)).toBe(selection);
        }
    });
    it('keeps the editable peak centered on its parameters across the native range', () => {
        for (let frequency = 4; frequency <= 58; frequency++) {
            for (let gain = -12; gain <= 12; gain++) {
                for (const width of [10, 60, 120]) {
                    const coefficients = eqParameterCoefficients(0, frequency, gain, width);
                    expect(eqParameterResponse(coefficients, eqFrequencyPosition(frequency))).toBeCloseTo(gain, 4);
                }
            }
        }
    });
    it('separates low-frequency coefficient artifacts from the parameter response', () => {
        const coefficients = eqCoefficients(0, 10, -4, 10);
        expect(coefficients).toEqual([-16268, 8098, 8171, 16268, -8076]);
        expect(eqResponse(coefficients, 0)).toBeGreaterThan(4);
        const parameters = eqParameterCoefficients(0, 10, -4, 10);
        const center = eqFrequencyPosition(10);
        expect(eqParameterResponse(parameters, center)).toBeCloseTo(-4, 4);
        for (let x = 0; x <= 1; x += 0.002) {
            const response = eqParameterResponse(parameters, x);
            expect(response).toBeGreaterThanOrEqual(-4.0001);
            expect(response).toBeLessThanOrEqual(0.0001);
        }
    });
});
