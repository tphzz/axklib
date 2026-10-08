import { describe, expect, it } from 'vitest';
import { mappingLabels, velocityGrid } from './mappingPresentation';
import type { KeyboardZone } from './keyboardMapping';

const range = { low: 0, high: 127, velocityLow: 0, velocityHigh: 127 };
const zone = (id: string, values = {}): KeyboardZone => ({ ...range, id, label: id, ...values });

describe('mapping labels and velocity grid', () => {
    it('marks every fifth velocity and labels every twenty-fifth at inclusive-cell centers', () => {
        const grid = velocityGrid();
        expect(grid.major.map((tick) => tick.value)).toEqual([0, 25, 50, 75, 100, 125]);
        expect([...grid.major, ...grid.minor].map((tick) => tick.value).sort((a, b) => a - b)).toEqual(
            Array.from({ length: 26 }, (_, index) => index * 5),
        );
        expect(grid.major.find((tick) => tick.value === 50)?.position).toBe(60.546875);
        expect(grid.major[0]?.position).toBe(99.609375);
        expect(grid.major.at(-1)?.position).toBe(1.953125);
    });
    it('keeps the raster fixed through resizing without a competing 127 label', () => {
        expect(velocityGrid()).toBe(velocityGrid());
        expect(velocityGrid().major.some((tick) => tick.value === 127)).toBe(false);
    });
    it('labels unselected mappings as well as selected ones without collisions', () => {
        const labels = mappingLabels(
            [zone('left', { low: 0, high: 23 }), zone('right', { low: 24, high: 47, selected: true })],
            0,
            128,
            1024,
            400,
        );
        expect(labels.map((label) => label.id).sort()).toEqual(['left', 'right']);
        expect(labels.find((label) => label.id === 'right')).toMatchObject({
            selected: true,
            left: 195,
            top: 4,
            height: 392,
        });
    });
    it('prioritizes selected names over colliding labels, deterministically', () => {
        const zones = [zone('a'), zone('b', { selected: true }), zone('c')];
        expect(mappingLabels(zones, 0, 128, 1024, 400).map((label) => label.id)).toEqual(['b']);
        expect(mappingLabels(zones.toReversed(), 0, 128, 1024, 400)).toEqual(mappingLabels(zones, 0, 128, 1024, 400));
    });
    it('clips labels to the visible plot and omits empty, narrow and thin mappings', () => {
        const labels = mappingLabels(
            [
                zone('clipped', { low: 0, high: 60 }),
                zone('offscreen', { high: 24 }),
                zone('single-note', { low: 64, high: 64 }),
                zone('single-velocity', { velocityLow: 64, velocityHigh: 64 }),
                zone('empty', { empty: true }),
            ],
            48,
            80,
            800,
            400,
        );
        expect(labels.map((label) => label.id)).toEqual(['clipped']);
        expect(labels[0]?.left).toBe(3);
        expect(mappingLabels([zone('single-note', { low: 64, high: 64 })], 60, 12, 800, 400)).toHaveLength(1);
    });
});
