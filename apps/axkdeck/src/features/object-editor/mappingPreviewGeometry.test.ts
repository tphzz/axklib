import { describe, expect, it, vi } from 'vitest';
import { MappingGeometryCache } from './keyboardGeometry';
import type { KeyboardRange, KeyboardZone } from './keyboardMapping';
import { MappingPreviewGeometryCache } from './mappingPreviewGeometry';

const zone = (id: string, patch: Partial<KeyboardZone> = {}): KeyboardZone => ({
    id,
    label: id,
    low: 12,
    high: 36,
    velocityLow: 0,
    velocityHigh: 127,
    ...patch,
});
function areas(path: string): KeyboardRange[] {
    return [...path.matchAll(/M(\d+) (\d+)h(\d+)v(\d+)h-\d+Z/g)].map((match) => {
        const [low, top, width, height] = match.slice(1).map(Number) as [number, number, number, number];
        return { low, high: low + width - 1, velocityLow: 128 - top - height, velocityHigh: 127 - top };
    });
}
const contains = (range: KeyboardRange, note: number, velocity: number) =>
    note >= range.low && note <= range.high && velocity >= range.velocityLow && velocity <= range.velocityHigh;
describe('incremental mapping preview geometry', () => {
    it('retains canonical geometry and caches the unaffected background for an entire gesture', () => {
        const a = zone('a'),
            b = zone('b', { low: 48, high: 72 });
        const original = new MappingGeometryCache().read([a, b]);
        const cache = new MappingPreviewGeometryCache();
        expect(cache.read(original).geometry).toBe(original);
        const ids = new Set(['a']);
        const read = vi.spyOn(MappingGeometryCache.prototype, 'read');
        try {
            const first = cache.read(original, { ids, zones: [{ ...a, velocityHigh: 84 }] });
            const next = cache.read(original, { ids, zones: [{ ...a, low: 0, high: 20, velocityHigh: 60 }] });
            expect(read).toHaveBeenCalledTimes(1);
            expect(next.geometry.colors).toBe(first.geometry.colors);
            expect(next.geometry.zones[1]).toBe(original.zones[1]);
            expect(next.geometry.coverage[24]!.map((item) => item.id)).toEqual([]);
            expect(next.geometry.coverage[12]![0]!.velocityHigh).toBe(60);
            expect(original.zones[0]!.velocityHigh).toBe(127);
            expect(cache.read(original).geometry).toBe(original);
        } finally {
            read.mockRestore();
        }
    });
    it('keeps overlap shading exact when a selected block moves or shrinks', () => {
        const a = zone('a'),
            b = zone('b', { low: 24, high: 72, velocityLow: 32, velocityHigh: 95 }),
            c = zone('c', { low: 48, high: 96, velocityLow: 64 });
        const original = new MappingGeometryCache().read([a, b, c]);
        const cache = new MappingPreviewGeometryCache();
        const ids = new Set(['a']);
        for (const patch of [{ velocityHigh: 84 }, { low: 80, high: 100, velocityLow: 90 }, { empty: true }]) {
            const frame = cache.read(original, { ids, zones: [{ ...a, ...patch }] });
            const shaded = areas(frame.overlapPath!);
            for (let note = 0; note < 128; note++)
                for (let velocity = 0; velocity < 128; velocity++) {
                    const mapped = frame.geometry.zones.filter(
                        (item) => !item.empty && contains(item, note, velocity),
                    ).length;
                    expect(shaded.some((area) => contains(area, note, velocity))).toBe(mapped > 1);
                }
        }
    });
    it('includes a previously empty member when a bank preview makes its range valid', () => {
        const a = zone('a', { empty: true });
        const original = new MappingGeometryCache().read([a]);
        const frame = new MappingPreviewGeometryCache().read(original, {
            ids: new Set(['a']),
            zones: [{ ...a, empty: false, velocityHigh: 84 }],
        });
        expect(frame.geometry.ordered[0]!.velocityHigh).toBe(84);
        expect(frame.geometry.colors.has('a')).toBe(true);
        expect(frame.geometry.coverage[24]).toHaveLength(1);
    });
});
