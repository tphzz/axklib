import { describe, expect, it } from 'vitest';
import type { SampleEditingSnapshot } from '../../../../lib/objectEditing';
import { sampleFormatFixture } from '../../../../test/sampleFormatFixture';
import { sampleFields } from './fields';
import { formatField } from './formatCapabilities';

function field(key: string, native = true) {
    const snapshot = sampleFormatFixture(native ? 'A3000_188' : 'A4000_A5000_224') as SampleEditingSnapshot;
    return formatField(
        sampleFields.find((item) => item.key === key)!,
        snapshot,
    );
}

describe('format-specific output routing', () => {
    it('names the native routing groups without marking the entire fields as extensions', () => {
        expect(field('output1_destination').label).toBe('Main output');
        expect(field('output1_level').label).toBe('Main output level');
        expect(field('output2_destination').label).toBe('Assignable output');
        expect(field('output2_level').label).toBe('Assignable output level');
        expect(field('output1_destination').extended).toBe(false);
        expect(field('output2_destination').extended).toBe(false);
        expect(field('output1_destination', false).label).toBe('Output 1');
        expect(field('output2_destination', false).label).toBe('Output 2');
    });

    it.each([
        ['output1_destination', 4],
        ['output2_destination', 5],
    ] as const)('preserves the native destination domain for %s', (key, maximum) => {
        const options = field(key).options!;
        expect(options.filter((option) => !option.disabled).map((option) => option.value)).toEqual(
            Array.from({ length: maximum + 1 }, (_, i) => i),
        );
        expect(options.filter((option) => option.extended).map((option) => option.value)).toEqual(
            Array.from({ length: 12 - maximum }, (_, i) => maximum + i + 1),
        );
        expect(field(key, false).options!.every((option) => !option.disabled)).toBe(true);
    });

    it('points to the other native slot only for destinations actually available there', () => {
        expect(field('output1_destination').options![5]!.reason).toContain('Assignable output');
        expect(field('output2_destination').options![7]!.reason).toContain('Main output');
        for (const key of ['output1_destination', 'output2_destination']) {
            for (const option of field(key).options!.filter((option) => option.value >= 10)) {
                expect(option.reason).toContain('A5000');
                expect(option.reason).not.toMatch(/select.*under/i);
                expect(option.label.match(/A5000/g)).toHaveLength(1);
            }
        }
    });

    it('explains optional hardware without disabling native assignable routes', () => {
        const options = field('output2_destination').options!;
        expect(options[1]!.reason).not.toContain('AIEB1');
        for (const option of options.slice(2, 6)) {
            expect(option.reason).toContain('AIEB1');
            expect(option.disabled).toBe(false);
            expect(option.extended).toBe(false);
        }
    });
});
