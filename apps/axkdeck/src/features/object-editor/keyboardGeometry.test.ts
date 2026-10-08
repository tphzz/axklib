import { describe, expect, it } from 'vitest';
import { isBlackKey, keyFace, keyCoverage, mappingRectangle, mappingOverlaps, mappingColors } from './keyboardGeometry';
import type { KeyboardZone } from './keyboardMapping';
const range = { low: 0, high: 127, velocityLow: 0, velocityHigh: 49 };
describe('keyboard face and range geometry', () => {
    it('centers labels and roots on the actual white or black key face', () => {
        expect(keyFace(60).center).toBe(60.75);
        expect(keyFace(64).center).toBe(64.25);
        expect(keyFace(61).center).toBe(61.5);
        for (let note = 0; note < 128; note++) {
            const face = keyFace(note);
            expect(face.center).toBe(face.x + face.width / 2);
            expect(face.height).toBe(isBlackKey(note) ? 18 : 28);
            expect(face.x).toBeGreaterThanOrEqual(0);
            expect(face.x + face.width).toBeLessThanOrEqual(128);
        }
    });
    it('uses inclusive MIDI cells and follows velocity resizing without insets', () => {
        expect(mappingRectangle(range, 0, 128)).toEqual({
            left: 0,
            width: 100,
            top: 60.9375,
            height: 39.0625,
            centerX: 50,
            centerY: 80.46875,
        });
        const single = mappingRectangle({ low: 60, high: 60, velocityLow: 64, velocityHigh: 64 }, 48, 24);
        expect(single).toMatchObject({ left: 50, top: 49.21875, height: 100 / 128, centerY: 49.609375 });
        expect(single.width).toBeCloseTo(100 / 24);
        expect(single.centerX).toBeCloseTo(50 + 50 / 24);
    });
    it('retains every nonempty mapping and its overlap contributors', () => {
        const zones: KeyboardZone[] = [
            { ...range, id: 'a', label: 'A' },
            { ...range, id: 'b', label: 'B', velocityLow: 20 },
            { ...range, id: 'empty', label: 'Empty', empty: true },
        ];
        expect(keyCoverage(zones, 60).map((zone) => zone.id)).toEqual(['a', 'b']);
        const overlap = mappingOverlaps(zones);
        expect(overlap).toHaveLength(1);
        expect(overlap[0]).toMatchObject({ low: 0, high: 127, velocityLow: 20, velocityHigh: 49 });
        expect(overlap[0]!.zones).toEqual(zones.slice(0, 2));
    });
    it('alternates only two green tones in source-range order, independent of selection and input order', () => {
        const zones: KeyboardZone[] = [
            { ...range, low: 60, high: 72, id: 'c', label: 'C' },
            { ...range, low: 24, high: 36, id: 'a', label: 'A' },
            { ...range, low: 40, high: 50, id: 'b', label: 'B' },
            { ...range, id: 'empty', label: 'Empty', empty: true },
        ];
        const colors = mappingColors(zones);
        expect(colors.get('a')).toBe('var(--editor-loop)');
        expect(colors.get('b')).toBe('color-mix(in srgb, var(--editor-loop) 90%, black)');
        expect(colors.get('c')).toBe(colors.get('a'));
        expect(colors.has('empty')).toBe(false);
        expect(mappingColors(zones.toReversed().map((zone) => ({ ...zone, selected: zone.id === 'b' })))).toEqual(
            colors,
        );
        expect(mappingColors([zones[1]!]).get('a')).toBe('var(--editor-loop)');
    });
    it('uses unmasked source ranges, velocity and IDs to resolve color-order ties', () => {
        const zones: KeyboardZone[] = [
            { ...range, id: 'b', label: 'B', source: { ...range, low: 24, velocityLow: 32 } },
            { ...range, id: 'a', label: 'A', source: { ...range, low: 24, velocityLow: 0 } },
            { ...range, id: 'c', label: 'C', source: { ...range, low: 24, velocityLow: 32 } },
        ];
        const colors = mappingColors(zones);
        expect(colors.get('a')).toBe('var(--editor-loop)');
        expect(colors.get('c')).toBe(colors.get('a'));
        expect(colors.get('b')).not.toBe(colors.get('a'));
        expect(mappingColors(zones.map((zone) => ({ ...zone, low: 80, velocityLow: 40 })))).toEqual(colors);
    });
});
