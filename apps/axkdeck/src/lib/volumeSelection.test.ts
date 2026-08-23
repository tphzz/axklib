import { describe, expect, it } from 'vitest';
import type { DiskTreeItem } from './types';
import { emptyVolumeSelection, updateVolumeSelection } from './volumeSelection';

const volume = (id: string, partitionIndex: number): DiskTreeItem => ({
    id,
    name: id,
    kind: 'volume',
    childCount: 0,
    partitionIndex,
});

const partition = (id: string, partitionIndex: number): DiskTreeItem => ({
    id,
    name: id,
    kind: 'partition',
    childCount: 0,
    partitionIndex,
});

describe('volume selection', () => {
    const visible = [volume('a', 0), volume('b', 0), volume('c', 1), volume('d', 1)];

    it('supports toggle and cross-partition visible ranges', () => {
        let selection = updateVolumeSelection(emptyVolumeSelection(), visible, visible[0]!, 'replace').selection;
        selection = updateVolumeSelection(selection, visible, visible[2]!, 'add-range').selection;
        expect(selection.items.map((item) => item.id)).toEqual(['a', 'b', 'c']);

        selection = updateVolumeSelection(selection, visible, visible[1]!, 'toggle').selection;
        expect(selection.items.map((item) => item.id)).toEqual(['a', 'c']);
    });

    it('replaces the selected set for a plain click and range selection', () => {
        let selection = updateVolumeSelection(emptyVolumeSelection(), visible, visible[0]!, 'replace').selection;
        selection = updateVolumeSelection(selection, visible, visible[3]!, 'toggle').selection;
        selection = updateVolumeSelection(selection, visible, visible[1]!, 'replace').selection;
        expect(selection.items.map((item) => item.id)).toEqual(['b']);

        selection = updateVolumeSelection(selection, visible, visible[3]!, 'range').selection;
        expect(selection.items.map((item) => item.id)).toEqual(['b', 'c', 'd']);
    });

    it('moves the active item to the nearest remaining volume when toggled off', () => {
        let selection = updateVolumeSelection(emptyVolumeSelection(), visible, visible[0]!, 'replace').selection;
        selection = updateVolumeSelection(selection, visible, visible[1]!, 'toggle').selection;
        selection = updateVolumeSelection(selection, visible, visible[2]!, 'toggle').selection;

        const update = updateVolumeSelection(selection, visible, visible[1]!, 'toggle');
        expect(update.selection.items.map((item) => item.id)).toEqual(['a', 'c']);
        expect(update.active.id).toBe('c');
    });

    it('clears volume selection when a non-volume row is selected', () => {
        const selected = updateVolumeSelection(emptyVolumeSelection(), visible, visible[0]!, 'replace').selection;
        const update = updateVolumeSelection(selected, visible, partition('partition', 0), 'replace');
        expect(update.selection).toEqual(emptyVolumeSelection());
        expect(update.active.id).toBe('partition');
    });
});
