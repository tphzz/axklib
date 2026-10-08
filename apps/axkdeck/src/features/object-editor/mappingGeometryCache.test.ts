import { describe, expect, it } from 'vitest';
import { MappingGeometryCache, mappingOverlaps, keyCoverage } from './keyboardGeometry';
import type { KeyboardZone } from './keyboardMapping';

const zone = (id: string, overrides = {}): KeyboardZone => ({
    id,
    label: id,
    low: 0,
    high: 127,
    velocityLow: 0,
    velocityHigh: 127,
    ...overrides,
});
describe('shared mapping geometry index', () => {
    it('deduplicates identical source outlines without dropping mapping regions or contributors', () => {
        const source = { low: 12, high: 100, velocityLow: 10, velocityHigh: 90 };
        const geometry = new MappingGeometryCache().read([
            zone('a', { source }),
            zone('b', { source: { ...source } }),
            zone('c', { source: { ...source, low: 24 } }),
        ]);
        expect(geometry.zones).toHaveLength(3);
        expect(geometry.sources).toHaveLength(2);
        expect(geometry.coverage[60]?.map((zone) => zone.id)).toEqual(['a', 'b', 'c']);
    });
    it('reuses geometry through selection and rejects mutated range/source/root/label data', () => {
        const cache = new MappingGeometryCache();
        const zones = [zone('a', { source: { low: 12, high: 100, velocityLow: 10, velocityHigh: 90 } }), zone('b')];
        const original = cache.read(zones);
        expect(cache.read(zones.map((z) => ({ ...z, selected: z.id === 'b' })))).toBe(original);
        expect(original.coverage[60]?.map((z) => z.id)).toEqual(['a', 'b']);
        zones[0]!.low = 24;
        const changed = cache.read(zones);
        expect(changed).not.toBe(original);
        expect(original.coverage[0]?.map((z) => z.id)).toEqual(['a', 'b']);
        expect(changed.coverage[0]?.map((z) => z.id)).toEqual(['b']);
        for (const patch of [
            { root: 60 },
            { label: 'Renamed' },
            { source: { low: 13, high: 100, velocityLow: 10, velocityHigh: 90 } },
            { empty: true },
        ]) {
            const before = cache.read(zones);
            Object.assign(zones[0]!, patch);
            expect(cache.read(zones)).not.toBe(before);
        }
        expect(cache.read([]).coverage.every((row) => row.length === 0)).toBe(true);
    });
    it('matches the simple per-cell reference for random, nested, duplicate and empty mappings', () => {
        let seed = 17;
        const random = () => {
            seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
            return seed % 128;
        };
        for (let run = 0; run < 24; run++) {
            const zones = Array.from({ length: run + 1 }, (_, i) => {
                const x = [random(), random()].sort((a, b) => a - b),
                    y = [random(), random()].sort((a, b) => a - b);
                return zone(String(i), {
                    low: x[0],
                    high: x[1],
                    velocityLow: y[0],
                    velocityHigh: y[1],
                    empty: i % 7 === 6,
                });
            });
            zones.push(zone('duplicate', { ...zones[0], id: 'duplicate' }));
            const geometry = new MappingGeometryCache().read(zones);
            const overlaps = mappingOverlaps(zones);
            for (let key = 0; key < 128; key++) {
                expect(geometry.coverage[key]?.map((z) => z.id)).toEqual(keyCoverage(zones, key).map((z) => z.id));
                const expectedRow: string[][][] = [],
                    foundRow: string[][][] = [];
                for (let velocity = 0; velocity < 128; velocity++) {
                    const expected = zones
                        .filter(
                            (z) =>
                                !z.empty &&
                                key >= z.low &&
                                key <= z.high &&
                                velocity >= z.velocityLow &&
                                velocity <= z.velocityHigh,
                        )
                        .map((z) => z.id);
                    const found = overlaps.filter(
                        (o) => key >= o.low && key <= o.high && velocity >= o.velocityLow && velocity <= o.velocityHigh,
                    );
                    expectedRow.push(expected.length > 1 ? [expected] : []);
                    foundRow.push(found.map((o) => o.zones.map((z) => z.id)));
                }
                expect(foundRow).toEqual(expectedRow);
            }
        }
    });
});
