import { describe, expect, it } from 'vitest';
import type { DiskTreeItem } from './types';
import { orderSamplerTreeItems } from './samplerTreeOrder';

function item(id: string, name: string, kind: DiskTreeItem['kind'], partitionIndex?: number): DiskTreeItem {
    return { id, name, kind, partitionIndex, childCount: 0 };
}

describe('sampler tree ordering', () => {
    it('orders partitions by their sampler-visible ASCII names', () => {
        const partitions = [
            item('p0', '001_PARTITION 1', 'partition', 0),
            item('p1', 'PARTITION 2', 'partition', 1),
            item('p2', 'A_PARTITION 3', 'partition', 2),
            item('p3', '_PARTITION 4', 'partition', 3),
            item('p4', 'PARTITION 5', 'partition', 4),
            item('p5', 'B_PARTITION 6', 'partition', 5),
            item('p6', 'PARTITION 7', 'partition', 6),
            item('p7', '$PARTITION 8', 'partition', 7),
        ];

        expect(orderSamplerTreeItems(partitions, 'partition').map(({ name }) => name)).toEqual([
            '$PARTITION 8',
            '001_PARTITION 1',
            'A_PARTITION 3',
            'B_PARTITION 6',
            'PARTITION 2',
            'PARTITION 5',
            'PARTITION 7',
            '_PARTITION 4',
        ]);
    });

    it('orders volumes by their sampler-visible ASCII names without moving other node kinds', () => {
        const volumes = [
            item('v0', '$foo', 'volume'),
            item('category', 'Programs', 'category'),
            item('v1', 'a_foo', 'volume'),
            item('v2', '001_foo', 'volume'),
            item('v3', 'c_foo', 'volume'),
            item('v4', '!foo', 'volume'),
        ];

        expect(orderSamplerTreeItems(volumes, 'volume').map(({ name }) => name)).toEqual([
            '!foo',
            'Programs',
            '$foo',
            '001_foo',
            'a_foo',
            'c_foo',
        ]);
    });

    it('uses physical identity as the tie-breaker without mutating the source', () => {
        const partitions = [item('p7', 'SAME', 'partition', 7), item('p0', 'SAME', 'partition', 0)];

        expect(orderSamplerTreeItems(partitions, 'partition').map(({ id }) => id)).toEqual(['p0', 'p7']);
        expect(partitions.map(({ id }) => id)).toEqual(['p7', 'p0']);
    });
});
