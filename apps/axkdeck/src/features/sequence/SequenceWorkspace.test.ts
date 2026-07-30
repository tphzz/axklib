import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import type { SamplerObject } from '../../lib/transport';
import type { SequenceItem } from '../../lib/types';
import SequenceWorkspace from './SequenceWorkspace.svelte';

function sequence(name: string, eventCount: number): SequenceItem {
    const object: SamplerObject = {
        key: `SEQU-${name}`,
        objectType: 'SEQU',
        name,
        partitionIndex: 0,
        partitionName: 'Partition 0',
        volumeName: 'Songs',
        categoryName: 'SEQU',
        sfsId: 1,
        storedSizeBytes: 512,
        sampleRate: 0,
        rootKey: 0,
        frameCount: 0,
        sampleWidthBytes: 0,
        sequence: {
            formatVersion: 1,
            ticksPerQuarterNote: 96,
            firstTick: 0,
            endTick: 384,
            eventCount,
            initialBeatsPerMinute: 120,
        },
    };
    return { id: object.key, objectId: object.key, name, object };
}

const common = {
    activeObjectId: '',
    query: '',
    onquerychange: vi.fn(),
    onselect: vi.fn(),
};

describe('SequenceWorkspace', () => {
    it('naturally orders dense Sequence rows and exposes decoded timing facts', () => {
        render(SequenceWorkspace, {
            props: {
                ...common,
                sequences: [sequence('Song 10', 20), sequence('Song 2', 12)],
            },
        });

        const rows = screen.getAllByRole('button', { name: /Song/ });
        expect(rows.map((row) => row.textContent)).toEqual([
            expect.stringContaining('Song 2'),
            expect.stringContaining('Song 10'),
        ]);
        expect(screen.getByText('12 events · 96 PPQN · 120 BPM')).toBeTruthy();
    });

    it('offers package, MIDI, rename, and delete actions for one selected Sequence', async () => {
        const item = sequence('Demo Song', 4);
        const onrenameobject = vi.fn();
        const onexportobjects = vi.fn();
        const onexportmidi = vi.fn();
        const ondeleteobjects = vi.fn();
        const onselectionchange = vi.fn();
        const rendered = render(SequenceWorkspace, {
            props: {
                ...common,
                sequences: [item],
                objectRenameAvailable: true,
                packageExportAvailable: true,
                sequenceExportAvailable: true,
                objectDeletionAvailable: true,
                onrenameobject,
                onexportobjects,
                onexportmidi,
                ondeleteobjects,
                onselectionchange,
            },
        });

        const row = screen.getByRole('button', { name: /Demo Song/ });
        await fireEvent.click(row);
        const selection = onselectionchange.mock.calls[0]![0];
        await rendered.rerender({
            ...common,
            sequences: [item],
            objectRenameAvailable: true,
            packageExportAvailable: true,
            sequenceExportAvailable: true,
            objectDeletionAvailable: true,
            onrenameobject,
            onexportobjects,
            onexportmidi,
            ondeleteobjects,
            onselectionchange,
            selection,
        });

        const exported = {
            kind: 'SEQU',
            objectId: item.objectId,
            name: item.name,
            typeLabel: 'Sequence',
            partitionIndex: 0,
            partitionName: 'Partition 0',
            volumeName: 'Songs',
        };
        await fireEvent.contextMenu(row);
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export MIDI…' }));
        expect(onexportmidi).toHaveBeenCalledWith([exported]);

        await fireEvent.contextMenu(row);
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export package…' }));
        expect(onexportobjects).toHaveBeenCalledWith([exported]);

        await fireEvent.contextMenu(row);
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Rename' }));
        expect(onrenameobject).toHaveBeenCalledWith({ kind: 'sequence', object: item.object, name: item.name });

        await fireEvent.contextMenu(row);
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Delete' }));
        expect(ondeleteobjects).toHaveBeenCalledWith([exported]);
    });

    it('offers MIDI import from the collection toolbar', async () => {
        const onimportmidi = vi.fn();
        render(SequenceWorkspace, {
            props: {
                ...common,
                sequences: [],
                sequenceImportAvailable: true,
                onimportmidi,
            },
        });

        await fireEvent.click(screen.getByRole('button', { name: 'Import MIDI' }));
        expect(onimportmidi).toHaveBeenCalledOnce();
    });
});
