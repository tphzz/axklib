import { describe, expect, it, vi } from 'vitest';
import type { DiskTreeItem } from '../../lib/types';
import { createImageTreeActionHandler } from './treeActions';

const volume = (id: string, partitionIndex: number): DiskTreeItem => ({
    id,
    name: id,
    kind: 'volume',
    childCount: 0,
    partitionIndex,
});

describe('image tree action routing', () => {
    it('deletes the complete selection when the context target is one of its volumes', () => {
        const first = volume('first', 0);
        const second = volume('second', 1);
        const requestVolumeDeletion = vi.fn();
        const handler = createImageTreeActionHandler({
            imageSession: { volumeSelection: { items: [first, second] } },
            mutation: { requestVolumeDeletion },
        } as never);

        handler(second, 'delete-volume');

        expect(requestVolumeDeletion).toHaveBeenCalledWith([first, second]);
    });

    it('replaces the selection when deletion is requested from an unselected volume', () => {
        const selected = volume('selected', 0);
        const target = volume('target', 0);
        const requestVolumeDeletion = vi.fn();
        const handler = createImageTreeActionHandler({
            imageSession: { volumeSelection: { items: [selected] } },
            mutation: { requestVolumeDeletion },
        } as never);

        handler(target, 'delete-volume');

        expect(requestVolumeDeletion).toHaveBeenCalledWith([target]);
    });
});
