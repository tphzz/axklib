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
        sizeWithDependenciesBytes: null,
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
            headerTempoBpm: 130,
            effectiveInitialTempoMicrosecondsPerQuarterNote: 460_122,
            tempoEvents: [
                { tick: 0, microsecondsPerQuarterNote: 460_122 },
                { tick: 192, microsecondsPerQuarterNote: 400_000 },
            ],
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
    it('moves through Sequences with Up, Down, Home, and End', async () => {
        const sequences = [sequence('First', 1), sequence('Second', 2), sequence('Third', 3)];
        const onselect = vi.fn();
        const onselectionchange = vi.fn();
        render(SequenceWorkspace, {
            props: { ...common, sequences, onselect, onselectionchange },
        });

        const second = screen.getByRole('button', { name: /Second/ });
        second.focus();
        await fireEvent.keyDown(second, { key: 'ArrowDown' });
        expect(onselect).toHaveBeenLastCalledWith(sequences[2]);
        expect(document.activeElement).toBe(screen.getByRole('button', { name: /Third/ }));

        await fireEvent.keyDown(document.activeElement as HTMLElement, { key: 'Home' });
        expect(onselect).toHaveBeenLastCalledWith(sequences[0]);
        expect(document.activeElement).toBe(screen.getByRole('button', { name: /First/ }));

        await fireEvent.keyDown(document.activeElement as HTMLElement, { key: 'End' });
        expect(onselect).toHaveBeenLastCalledWith(sequences[2]);
        expect(document.activeElement).toBe(screen.getByRole('button', { name: /Third/ }));
    });

    it('moves selection and focus by a visible page in an ordinary list', async () => {
        const sequences = Array.from({ length: 12 }, (_, index) => sequence(`Song ${index + 1}`, index + 1));
        const onselect = vi.fn();
        render(SequenceWorkspace, {
            props: { ...common, sequences, onselect, onselectionchange: vi.fn() },
        });

        const first = screen.getByRole('button', { name: /Song 1 / });
        const list = first.closest<HTMLElement>('[data-navigation-list]')!;
        Object.defineProperty(list, 'clientHeight', { configurable: true, value: 160 });
        Object.defineProperty(first, 'offsetHeight', { configurable: true, value: 40 });
        first.focus();

        await fireEvent.keyDown(first, { key: 'PageDown' });

        expect(onselect).toHaveBeenLastCalledWith(sequences[3]);
        expect(document.activeElement).toBe(screen.getByRole('button', { name: /Song 4 / }));
    });

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
        expect(screen.getByText('12 events · 96 PPQN · 130.4 BPM')).toBeTruthy();
    });

    it('reports export selection independently from the inspected Sequence', async () => {
        const inspected = sequence('Inspected Song', 4);
        const selected = sequence('Selected Song', 8);
        const onselect = vi.fn();
        const onselectionchange = vi.fn();
        render(SequenceWorkspace, {
            props: {
                ...common,
                sequences: [inspected, selected],
                activeObjectId: inspected.objectId,
                onselect,
                selection: {
                    items: [
                        {
                            kind: 'SEQU',
                            objectId: selected.objectId,
                            name: selected.name,
                            typeLabel: 'Sequence',
                            partitionIndex: selected.object.partitionIndex,
                            partitionName: selected.object.partitionName,
                            volumeName: selected.object.volumeName,
                        },
                    ],
                    anchors: {},
                },
                onselectionchange,
            },
        });

        const inspectedButton = screen.getByRole('button', { name: /Inspected Song/ });
        const selectedButton = screen.getByRole('button', { name: /Selected Song/ });
        expect(inspectedButton.classList.contains('active')).toBe(true);
        expect(inspectedButton.classList.contains('selected')).toBe(false);
        expect(inspectedButton.getAttribute('aria-pressed')).toBe('false');
        expect(selectedButton.classList.contains('active')).toBe(false);
        expect(selectedButton.classList.contains('selected')).toBe(true);
        expect(selectedButton.getAttribute('aria-pressed')).toBe('true');

        await fireEvent.click(selectedButton, { ctrlKey: true });
        expect(onselect).not.toHaveBeenCalled();
        expect(onselectionchange.mock.calls.at(-1)?.[0].items).toEqual([]);
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
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export MIDI…' }));
        expect(onexportmidi).toHaveBeenCalledWith([exported]);

        await fireEvent.contextMenu(row);
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export package…' }));
        expect(onexportobjects).toHaveBeenCalledWith([exported]);

        await fireEvent.contextMenu(row);
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Rename…' }));
        expect(onrenameobject).toHaveBeenCalledWith({ kind: 'sequence', object: item.object, name: item.name });

        await fireEvent.contextMenu(row);
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Delete…' }));
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
