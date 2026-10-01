import { describe, expect, it } from 'vitest';
import type { DiskTreeItem } from '../../lib/types';
import { findRefreshedVolume, refreshVolumeSelection } from './sourceTree';

function volume(id: string, partitionIndex = 0, volumeDirectoryId: number | undefined = 7): DiskTreeItem {
    return { id, kind: 'volume', name: 'Volume', childCount: 0, partitionIndex, volumeDirectoryId };
}

describe('refreshed volume identity', () => {
    it('uses partition and directory identity despite regenerated IDs and changed labels', () => {
        const current = volume('old');
        const refreshed = { ...volume('new'), name: 'Updated label' };
        expect(findRefreshedVolume([volume('other-partition', 1), refreshed], current)).toBe(refreshed);
    });

    it('never substitutes a same-named volume with a different stored identity', () => {
        expect(findRefreshedVolume([volume('replacement', 0, 8)], volume('old'))).toBeNull();
        expect(findRefreshedVolume([volume('other-partition', 1)], volume('old'))).toBeNull();
    });

    it('requires a unique name within the same partition when storage identity is unavailable', () => {
        const current = { ...volume('old'), volumeDirectoryId: undefined };
        const fresh = { ...volume('new'), volumeDirectoryId: undefined };
        expect(findRefreshedVolume([fresh], current)).toBe(fresh);
        expect(findRefreshedVolume([fresh, { ...fresh, id: 'ambiguous' }], current)).toBeNull();
        expect(findRefreshedVolume([volume('other-partition', 1)], current)).toBeNull();
    });

    it('remaps every selected volume and its anchor without selecting replacements', () => {
        const selected = [volume('old-a'), volume('old-b', 0, 8), volume('removed', 0, 9)];
        const updated = [volume('new-a'), volume('new-b', 0, 8), volume('replacement', 0, 10)];
        const tree: DiskTreeItem[] = [{ id: 'disk', name: 'Disk', kind: 'disk', childCount: 3, children: updated }];
        expect(refreshVolumeSelection({ items: selected, anchorId: 'old-b' }, tree)).toEqual({
            items: updated.slice(0, 2),
            anchorId: 'new-b',
        });
        expect(refreshVolumeSelection({ items: selected, anchorId: 'removed' }, tree).anchorId).toBe('');
    });
});
