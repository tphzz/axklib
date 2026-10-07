import { describe, expect, it } from 'vitest';
import type { KeyboardRange, KeyboardZone } from '../object-editor/keyboardMapping';
import { createMappingPreview } from './preview';

const range: KeyboardRange = { low: 0, high: 60, velocityLow: 0, velocityHigh: 127 };
const zone = (id: string, patch: Partial<KeyboardZone> = {}): KeyboardZone => ({
    ...range,
    id,
    label: id,
    selected: true,
    selectionId: 0,
    root: 36,
    ...patch,
});
describe('local Mapping Editor projections', () => {
    it.each(['sample', 'members'] as const)('previews only the stored %s range without moving its root', (role) => {
        const selected = zone('a'),
            other = zone('b', { selectionId: 1, selected: false });
        const project = createMappingPreview({
            role,
            selectionId: 0,
            limits: range,
            zones: [selected, other],
            overrides: [],
        })!;
        const frame = project({ low: 12, high: 48, velocityLow: 10, velocityHigh: 84 });
        expect(frame.zones).toEqual([
            { ...selected, low: 12, high: 48, velocityLow: 10, velocityHigh: 84, empty: false },
        ]);
        expect(frame.ids).toEqual(new Set(['a']));
        expect(project(range).ids).toBe(frame.ids);
        expect(selected.velocityHigh).toBe(127);
        expect(other.selected).toBe(false);
    });
    it('clips every selected Program member against its unchanged source range', () => {
        const a = zone('a', { source: { low: 12, high: 48, velocityLow: 30, velocityHigh: 100 } });
        const b = zone('b', { source: { low: 50, high: 100, velocityLow: 80, velocityHigh: 127 } });
        const project = createMappingPreview({
            role: 'program',
            selectionId: 0,
            limits: range,
            zones: [a, b, zone('other', { selectionId: 1 })],
            overrides: [],
        })!;
        const frame = project({ low: 40, high: 70, velocityLow: 60, velocityHigh: 90 });
        expect(frame.zones[0]).toMatchObject({
            low: 40,
            high: 48,
            velocityLow: 60,
            velocityHigh: 90,
            empty: false,
            root: 36,
        });
        expect(frame.zones[1]).toMatchObject({ low: 50, high: 70, velocityLow: 80, velocityHigh: 90, empty: false });
        expect(frame.zones[0]!.source).toBe(a.source);
        expect(project({ ...range, high: 20 }).zones[1]!.empty).toBe(true);
        expect(a.low).toBe(0);
    });
    it('projects bank overrides across all members while preserving inherited endpoints', () => {
        const a = zone('a', { source: { ...range, velocityLow: 20, velocityHigh: 84 } });
        const b = zone('b', {
            low: 61,
            high: 127,
            selectionId: 1,
            selected: false,
            source: { low: 61, high: 127, velocityLow: 90, velocityHigh: 127 },
        });
        const original = { ...range, velocityLow: 20 };
        const project = createMappingPreview({
            role: 'bank',
            selectionId: 0,
            limits: original,
            zones: [a, b],
            overrides: [
                { boundary: 'velocityLow', label: 'Low', inherited: true },
                { boundary: 'velocityHigh', label: 'High', inherited: false },
            ],
        })!;
        const frame = project({ ...original, velocityHigh: 70 });
        expect(frame.zones[0]).toMatchObject({ velocityLow: 20, velocityHigh: 70, empty: false });
        expect(frame.zones[1]).toMatchObject({ low: 61, high: 127, velocityLow: 90, velocityHigh: 70, empty: true });
        expect(project({ ...original, velocityHigh: 110 }).zones[1]!.empty).toBe(false);
        expect(project({ ...original, velocityLow: 40 }).zones.map((item) => item.velocityLow)).toEqual([40, 40]);
        expect(frame.ids).toEqual(new Set(['a', 'b']));
    });
    it('does not manufacture a preview without a current editable range', () => {
        expect(
            createMappingPreview({ role: 'sample', selectionId: null, limits: null, zones: [], overrides: [] }),
        ).toBeNull();
    });
});
