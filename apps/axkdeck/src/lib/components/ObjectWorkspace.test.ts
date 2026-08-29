import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import type { PackageExportSelectionState } from '../objectSelection';
import type { SamplerObject } from '../transport';
import type { PackageExportObject } from '../types';
import ObjectWorkspace from './ObjectWorkspace.svelte';

const appStyles = readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8');
const objectSizeIdentitySource = readFileSync(
    resolve(process.cwd(), 'src/lib/components/ObjectSizeIdentity.svelte'),
    'utf8',
);

function object(objectType: string, name: string): SamplerObject {
    return {
        key: `${objectType}-${name}`,
        objectType,
        name,
        partitionIndex: 0,
        partitionName: 'Partition 0',
        volumeName: 'Volume',
        categoryName: objectType,
        sfsId: 0,
        storedSizeBytes: 128,
        sizeWithDependenciesBytes: null,
        sampleRate: 0,
        rootKey: 0,
        frameCount: 0,
        sampleWidthBytes: 0,
    };
}

const common = {
    programs: [],
    waveData: [],
    activeObjectId: '',
    query: '',
    onquerychange: vi.fn(),
    onprogramselect: vi.fn(),
    onwavedataselect: vi.fn(),
};

describe('ObjectWorkspace', () => {
    it('moves through Programs with arrows and extends selection with Shift', async () => {
        const programObjects = ['001', '002', '003'].map((slot) => object('PROG', slot));
        const programs = programObjects.map((programObject, index) => ({
            id: `program-${index + 1}`,
            objectId: programObject.key,
            slot: programObject.name,
            programNumber: index + 1,
            name: `Program ${index + 1}`,
            object: programObject,
        }));
        const onprogramselect = vi.fn();
        const onselectionchange = vi.fn();
        const props = {
            ...common,
            programs,
            view: 'programs' as const,
            onprogramselect,
            onselectionchange,
        };
        const rendered = render(ObjectWorkspace, { props });
        const rows = () => screen.getAllByRole('button', { name: /Program \d/ });

        rows()[0]!.focus();
        await fireEvent.keyDown(rows()[0]!, { key: 'ArrowDown' });
        let selection = onselectionchange.mock.calls.at(-1)?.[0] as PackageExportSelectionState;
        expect(onprogramselect).toHaveBeenLastCalledWith(programs[1]);
        expect(selection.items.map((item) => item.objectId)).toEqual([programObjects[1]!.key]);
        expect(document.activeElement).toBe(rows()[1]);

        await rendered.rerender({ ...props, activeObjectId: programObjects[1]!.key, selection });
        await fireEvent.keyDown(rows()[1]!, { key: 'ArrowDown', shiftKey: true });
        selection = onselectionchange.mock.calls.at(-1)?.[0] as PackageExportSelectionState;
        expect(selection.items.map((item) => item.objectId)).toEqual([programObjects[1]!.key, programObjects[2]!.key]);
        expect(onprogramselect).toHaveBeenCalledTimes(1);
        expect(document.activeElement).toBe(rows()[2]);
    });

    it('moves to the end of a virtualized Wave Data list and scrolls it into view', async () => {
        const waveData = Array.from({ length: 80 }, (_, index) => {
            const name = `SMP ${String(index + 1).padStart(3, '0')}`;
            const waveObject = {
                ...object('SMPL', name),
                sampleRate: 44_100,
                sampleWidthBytes: 2,
                frameCount: 1,
            };
            return {
                id: waveObject.key,
                objectKey: waveObject.key,
                name,
                note: 'C3',
                duration: '0.00 s',
                sampleRate: '44.1 kHz',
                bitDepth: '16-bit',
                channels: 'Mono' as const,
                storedSizeBytes: 2,
                sizeWithDependenciesBytes: null,
                waveform: [],
                previewState: 'idle' as const,
                object: waveObject,
            };
        });
        const onwavedataselect = vi.fn();
        const onselectionchange = vi.fn();
        render(ObjectWorkspace, {
            props: {
                ...common,
                waveData,
                view: 'wave-data',
                onwavedataselect,
                onselectionchange,
            },
        });

        const first = screen.getByRole('button', { name: 'Inspect SMP 001' });
        const list = document.querySelector('.collection-body') as HTMLElement;
        Object.defineProperty(list, 'clientHeight', { configurable: true, value: 210 });
        first.focus();
        await fireEvent.keyDown(first, { key: 'End' });

        expect(onwavedataselect).toHaveBeenLastCalledWith(waveData[79]);
        expect(screen.getByRole('button', { name: 'Inspect SMP 080' })).toBe(document.activeElement);
        expect(list.scrollTop).toBeGreaterThan(0);
    });

    it('retains selection and focus across repeated paging in virtualized Wave Data', async () => {
        const waveData = Array.from({ length: 80 }, (_, index) => {
            const name = `SMP ${String(index + 1).padStart(3, '0')}`;
            const waveObject = {
                ...object('SMPL', name),
                sampleRate: 44_100,
                sampleWidthBytes: 2,
                frameCount: 1,
            };
            return {
                id: waveObject.key,
                objectKey: waveObject.key,
                name,
                note: 'C3',
                duration: '0.00 s',
                sampleRate: '44.1 kHz',
                bitDepth: '16-bit',
                channels: 'Mono' as const,
                storedSizeBytes: 2,
                sizeWithDependenciesBytes: null,
                waveform: [],
                previewState: 'idle' as const,
                object: waveObject,
            };
        });
        const onwavedataselect = vi.fn();
        render(ObjectWorkspace, {
            props: {
                ...common,
                waveData,
                view: 'wave-data',
                onwavedataselect,
                onselectionchange: vi.fn(),
            },
        });

        const list = document.querySelector('.collection-body') as HTMLElement;
        Object.defineProperty(list, 'clientHeight', { configurable: true, value: 210 });
        screen.getByRole('button', { name: 'Inspect SMP 001' }).focus();

        await fireEvent.keyDown(document.activeElement as HTMLElement, { key: 'PageDown' });
        expect(onwavedataselect).toHaveBeenLastCalledWith(waveData[4]);
        expect(document.activeElement).toBe(screen.getByRole('button', { name: 'Inspect SMP 005' }));

        await fireEvent.keyDown(document.activeElement as HTMLElement, { key: 'PageDown' });
        expect(onwavedataselect).toHaveBeenLastCalledWith(waveData[8]);
        expect(document.activeElement).toBe(screen.getByRole('button', { name: 'Inspect SMP 009' }));

        await fireEvent.keyDown(document.activeElement as HTMLElement, { key: 'PageUp' });
        expect(onwavedataselect).toHaveBeenLastCalledWith(waveData[4]);
        expect(document.activeElement).toBe(screen.getByRole('button', { name: 'Inspect SMP 005' }));

        await fireEvent.keyDown(document.activeElement as HTMLElement, { key: 'End' });
        expect(onwavedataselect).toHaveBeenLastCalledWith(waveData[79]);
        expect(document.activeElement).toBe(screen.getByRole('button', { name: 'Inspect SMP 080' }));
    });

    it('mounts and scrolls a bounded window of Wave Data rows and canvases', async () => {
        const waveData = Array.from({ length: 200 }, (_, index) => {
            const name = `SMP ${String(index + 1).padStart(3, '0')}`;
            const waveObject = {
                ...object('SMPL', name),
                sampleRate: 44_100,
                sampleWidthBytes: 2,
                frameCount: 1,
            };
            return {
                id: waveObject.key,
                objectKey: waveObject.key,
                name,
                note: 'C3',
                duration: '0.00 s',
                sampleRate: '44.1 kHz',
                bitDepth: '16-bit',
                channels: 'Mono' as const,
                storedSizeBytes: 2,
                sizeWithDependenciesBytes: null,
                waveform: [],
                previewState: 'idle' as const,
                object: waveObject,
            };
        });

        render(ObjectWorkspace, { props: { ...common, waveData, view: 'wave-data' } });

        expect(screen.getByText('200 items')).toBeTruthy();
        expect(document.querySelectorAll('.wave-data-row').length).toBeLessThan(60);
        expect(document.querySelectorAll('.wave-data-row canvas').length).toBeLessThan(60);
        expect(screen.getByRole('group', { name: 'SMP 001 Wave Data' })).toBeTruthy();
        expect(screen.queryByRole('group', { name: 'SMP 200 Wave Data' })).toBeNull();

        const list = document.querySelector('.collection-body') as HTMLElement;
        Object.defineProperty(list, 'clientHeight', { configurable: true, value: 420 });
        list.scrollTop = 8_400;
        await fireEvent.scroll(list);

        expect(screen.queryByRole('group', { name: 'SMP 001 Wave Data' })).toBeNull();
        expect(screen.getByRole('group', { name: 'SMP 200 Wave Data' })).toBeTruthy();
    });

    it('naturally orders the standalone Wave Data lane', () => {
        const wave2 = {
            ...object('SMPL', 'SMP 2'),
            sampleRate: 44_100,
            sampleWidthBytes: 2,
            frameCount: 1,
        };
        const wave10 = {
            ...object('SMPL', 'SMP 10'),
            sampleRate: 44_100,
            sampleWidthBytes: 2,
            frameCount: 1,
        };
        render(ObjectWorkspace, {
            props: {
                ...common,
                waveData: [wave10, wave2].map((waveObject) => ({
                    id: waveObject.key,
                    objectKey: waveObject.key,
                    name: waveObject.name,
                    note: 'C3',
                    duration: '0.00 s',
                    sampleRate: '44.1 kHz',
                    bitDepth: '16-bit',
                    channels: 'Mono' as const,
                    storedSizeBytes: 2,
                    sizeWithDependenciesBytes: null,
                    waveform: [],
                    previewState: 'idle' as const,
                    object: waveObject,
                })),
                view: 'wave-data',
            },
        });

        const rows = [...document.querySelectorAll('.wave-data-row')];
        expect(rows.map((row) => row.textContent)).toEqual([
            expect.stringContaining('SMP 2'),
            expect.stringContaining('SMP 10'),
        ]);
    });

    it.each([
        ['programs', 'No matching Programs'],
        ['wave-data', 'No matching Wave Data'],
    ] as const)('uses the shared centered empty state for %s', (view, message) => {
        render(ObjectWorkspace, { props: { ...common, view } });

        const emptyState = screen.getByText(message);
        expect(emptyState.classList.contains('empty-copy')).toBe(true);
        expect(emptyState.parentElement?.classList.contains('empty-collection')).toBe(true);
    });

    it('offers volume-scoped orphan cleanup only in the Wave Data view when available', async () => {
        const oncleanupwavedata = vi.fn();
        const rendered = render(ObjectWorkspace, {
            props: {
                ...common,
                view: 'wave-data',
                waveDataCleanupAvailable: true,
                oncleanupwavedata,
            },
        });

        const action = screen.getByRole('button', { name: 'Clean up unreferenced Wave Data' });
        await fireEvent.click(action);
        expect(oncleanupwavedata).toHaveBeenCalledOnce();

        await rendered.rerender({
            ...common,
            view: 'programs',
            waveDataCleanupAvailable: true,
            oncleanupwavedata,
        });
        expect(screen.queryByRole('button', { name: 'Clean up unreferenced Wave Data' })).toBeNull();
    });

    it('offers Program generation only in the Programs view when available', async () => {
        const onGeneratePrograms = vi.fn();
        const rendered = render(ObjectWorkspace, {
            props: {
                ...common,
                view: 'programs',
                programGenerationAvailable: true,
                onprogramgeneration: onGeneratePrograms,
            },
        });

        const action = screen.getByRole('button', { name: 'Generate Programs' });
        await fireEvent.click(action);
        expect(onGeneratePrograms).toHaveBeenCalledOnce();

        await rendered.rerender({
            ...common,
            view: 'wave-data',
            programGenerationAvailable: true,
            onprogramgeneration: onGeneratePrograms,
        });
        expect(screen.queryByRole('button', { name: 'Generate Programs' })).toBeNull();
    });

    it('renders Programs as factual list rows without fabricated keyboard metadata', () => {
        const programObject = { ...object('PROG', '001'), sizeWithDependenciesBytes: 4096 };
        render(ObjectWorkspace, {
            props: {
                ...common,
                programs: [
                    {
                        id: 'program-1',
                        objectId: programObject.key,
                        slot: '001',
                        programNumber: 1,
                        name: 'Grand Piano',
                        object: programObject,
                    },
                ],
                view: 'programs',
            },
        });

        expect(screen.getByText('Grand Piano')).toBeTruthy();
        expect(screen.getByText('001')).toBeTruthy();
        expect(screen.queryByText('PROG')).toBeNull();
        expect(document.querySelector('.object-code')).toBeNull();
        expect(document.querySelector('.program-keyboard')).toBeNull();
        expect(document.querySelector('.program-list')).toBeTruthy();
        const row = document.querySelector('.program-row');
        expect(row).toBeTruthy();
        expect(row?.querySelector('.object-slot')?.textContent).toBe('001');
        expect(row?.querySelector('.program-identity .object-size-primary')?.textContent).toContain('Grand Piano');
        expect(row?.querySelector('.program-identity .object-size-secondary')?.textContent).toBe(
            '128 B · 4 KiB incl. deps.',
        );
        expect(row?.querySelector('.object-size-summary')).toBeNull();
        expect(screen.getByText('128 B · 4 KiB incl. deps.')).toBeTruthy();
        expect(document.querySelector('[title*="Object size with deps.: 4 KiB"]')).toBeTruthy();
        expect(document.querySelector('.object-card')).toBeNull();
        expect(screen.getByRole('searchbox', { name: 'Search Programs' })).toBeTruthy();

        const listRule = appStyles.match(/\.program-list\s*\{[^}]+\}/)?.[0];
        const rowRule = appStyles.match(/\.program-row\s*\{[^}]+\}/)?.[0];
        const slotRule = appStyles.match(/\.program-row \.object-slot\s*\{[^}]+\}/)?.[0];
        expect(listRule).toContain('gap: 0');
        expect(listRule).toContain('padding: 2px 6px 5px');
        expect(rowRule).toContain('height: var(--density-row)');
        expect(rowRule).toContain('grid-template-columns: 30px minmax(0, 1fr)');
        expect(rowRule).toContain('gap: 4px');
        expect(rowRule).toContain('align-items: center');
        expect(rowRule).toContain('padding: 0');
        expect(rowRule).toContain('font: inherit');
        expect(slotRule).toContain('padding: 3px 6px 1px');

        const identityRule = appStyles.match(/\.program-identity\s*\{[^}]+\}/)?.[0];
        const primaryRule = objectSizeIdentitySource.match(/\.object-size-primary strong\s*\{[^}]+\}/)?.[0];
        const secondaryRule = objectSizeIdentitySource.match(/\.object-size-secondary\s*\{[^}]+\}/)?.[0];
        expect(identityRule).toContain('font-size: 10px');
        expect(identityRule).toContain('line-height: 10px');
        expect(identityRule).toContain('padding: 2px 6px 2px 0');
        expect(primaryRule).toContain('font-size: 10px');
        expect(primaryRule).toContain('line-height: 10px');
        expect(secondaryRule).toContain('font-size: 8.5px');
        expect(secondaryRule).toContain('line-height: 9px');
    });

    it('keeps Program inspection fixed while modifier gestures update the exact selection', async () => {
        const programObjects = ['001', '002', '003', '004'].map((slot) => object('PROG', slot));
        const programs = programObjects.map((programObject, index) => ({
            id: `program-${index + 1}`,
            objectId: programObject.key,
            slot: programObject.name,
            programNumber: index + 1,
            name: `Program ${index + 1}`,
            object: programObject,
        }));
        const onprogramselect = vi.fn();
        const onselectionchange = vi.fn();
        const baseProps = {
            ...common,
            programs,
            view: 'programs' as const,
            onprogramselect,
            onselectionchange,
        };
        const rendered = render(ObjectWorkspace, { props: baseProps });
        const rows = () => screen.getAllByRole('button', { name: /Program \d/ });

        await fireEvent.click(rows()[0]!);
        expect(onprogramselect).toHaveBeenCalledOnce();
        let selection = onselectionchange.mock.calls.at(-1)?.[0] as PackageExportSelectionState;
        await rendered.rerender({ ...baseProps, activeObjectId: programObjects[0]!.key, selection });

        await fireEvent.click(rows()[3]!, { shiftKey: true });
        selection = onselectionchange.mock.calls.at(-1)?.[0] as PackageExportSelectionState;
        expect(selection.items.map((item) => item.objectId)).toEqual(programObjects.map((item) => item.key));
        expect(onprogramselect).toHaveBeenCalledOnce();
        await rendered.rerender({ ...baseProps, activeObjectId: programObjects[0]!.key, selection });
        expect(rows()[0]!.classList.contains('active')).toBe(true);
        expect(rows()[3]!.classList.contains('active')).toBe(false);
        expect(rows()[3]!.getAttribute('aria-pressed')).toBe('true');

        await fireEvent.click(rows()[1]!, { ctrlKey: true });
        selection = onselectionchange.mock.calls.at(-1)?.[0] as PackageExportSelectionState;
        expect(selection.items.map((item) => item.objectId)).toEqual([
            programObjects[0]!.key,
            programObjects[2]!.key,
            programObjects[3]!.key,
        ]);
        expect(onprogramselect).toHaveBeenCalledOnce();
        await rendered.rerender({ ...baseProps, activeObjectId: programObjects[0]!.key, selection });
        expect(rows()[1]!.classList.contains('selected')).toBe(false);
        expect(rows()[1]!.getAttribute('aria-pressed')).toBe('false');

        await fireEvent.click(rows()[2]!, { ctrlKey: true });
        selection = onselectionchange.mock.calls.at(-1)?.[0] as PackageExportSelectionState;
        expect(selection.items.map((item) => item.objectId)).toEqual([programObjects[0]!.key, programObjects[3]!.key]);
        expect(onprogramselect).toHaveBeenCalledOnce();
    });

    it('offers Program rename by display name while retaining its numeric slot target', async () => {
        const programObject = object('PROG', '001');
        const onrenameobject = vi.fn();
        render(ObjectWorkspace, {
            props: {
                ...common,
                programs: [
                    {
                        id: 'program-1',
                        objectId: programObject.key,
                        slot: '001',
                        programNumber: 1,
                        name: 'Grand',
                        object: programObject,
                    },
                ],
                view: 'programs',
                objectRenameAvailable: true,
                onrenameobject,
            },
        });

        await fireEvent.contextMenu(screen.getByRole('button', { name: /Grand/ }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Rename…' }));
        expect(onrenameobject).toHaveBeenCalledWith({
            kind: 'program',
            object: programObject,
            name: 'Grand',
            programNumber: 1,
        });
    });

    it('uses one externally controlled mixed selection for export and deletion', async () => {
        const firstObject = object('PROG', '001');
        const bank: PackageExportObject = {
            kind: 'SBAC',
            objectId: 'bank',
            name: 'Strings',
            typeLabel: 'Sample Bank',
            partitionIndex: 0,
            partitionName: 'Partition 0',
            volumeName: 'Volume',
        };
        const onexportobjects = vi.fn();
        const ondeleteobjects = vi.fn();
        const onselectionchange = vi.fn();
        const rendered = render(ObjectWorkspace, {
            props: {
                ...common,
                programs: [
                    {
                        id: 'program-1',
                        objectId: firstObject.key,
                        slot: '001',
                        programNumber: 1,
                        name: 'Piano',
                        object: firstObject,
                    },
                ],
                view: 'programs',
                packageExportAvailable: true,
                objectDeletionAvailable: true,
                onexportobjects,
                ondeleteobjects,
                selection: { items: [bank], anchors: {} },
                onselectionchange,
            },
        });

        const piano = screen.getByRole('button', { name: /Piano/ });
        await fireEvent.click(piano, { ctrlKey: true });
        const selection = onselectionchange.mock.calls[0]![0];
        expect(selection.items.map((item: PackageExportObject) => item.objectId)).toEqual([firstObject.key, 'bank']);

        await rendered.rerender({
            ...common,
            programs: [
                {
                    id: 'program-1',
                    objectId: firstObject.key,
                    slot: '001',
                    programNumber: 1,
                    name: 'Piano',
                    object: firstObject,
                },
            ],
            view: 'programs',
            packageExportAvailable: true,
            objectDeletionAvailable: true,
            onexportobjects,
            ondeleteobjects,
            selection,
            onselectionchange,
        });
        await fireEvent.contextMenu(screen.getByRole('button', { name: /Piano/ }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export package…' }));

        expect(onexportobjects).toHaveBeenCalledWith([
            {
                kind: 'PROGRAM',
                objectId: firstObject.key,
                name: 'Piano',
                typeLabel: 'Program',
                partitionIndex: 0,
                partitionName: 'Partition 0',
                volumeName: 'Volume',
            },
            bank,
        ]);

        await fireEvent.contextMenu(screen.getByRole('button', { name: /Piano/ }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Delete 2 objects…' }));
        expect(ondeleteobjects).toHaveBeenCalledWith(onexportobjects.mock.calls[0]![0]);
    });

    it('renders Wave Data as dense rows with one full-row selection target', async () => {
        const waveObject = {
            ...object('SMPL', 'SMP 001'),
            sampleRate: 44_100,
            sampleWidthBytes: 2,
            frameCount: 44_100,
        };
        const onwavedataselect = vi.fn();
        render(ObjectWorkspace, {
            props: {
                ...common,
                waveData: [
                    {
                        id: waveObject.key,
                        objectKey: waveObject.key,
                        name: waveObject.name,
                        note: 'C3',
                        duration: '1.00 s',
                        sampleRate: '44.1 kHz',
                        bitDepth: '16-bit',
                        channels: 'Mono',
                        storedSizeBytes: 88_200,
                        waveform: [
                            { minimum: -10, maximum: 8 },
                            { minimum: -20, maximum: 18 },
                        ],
                        previewState: 'ready',
                        object: waveObject,
                    },
                ],
                view: 'wave-data',
                onwavedataselect,
            },
        });

        expect(document.querySelector('.wave-data-list')).toBeTruthy();
        expect(document.querySelector('.wave-data-row canvas')).toBeTruthy();
        expect(screen.queryByText('SMPL')).toBeNull();
        expect(document.querySelector('.object-code')).toBeNull();
        expect(document.querySelector('.waveform span')).toBeNull();

        const selectionTarget = screen.getByRole('button', { name: 'Inspect SMP 001' });
        expect(selectionTarget.parentElement?.classList.contains('wave-data-row')).toBe(true);
        expect(screen.getByRole('button', { name: 'Seek SMP 001' }).parentElement).toBe(selectionTarget.parentElement);
        expect(screen.getByRole('button', { name: 'Play SMP 001' }).parentElement).toBe(selectionTarget.parentElement);

        await fireEvent.click(selectionTarget);
        expect(onwavedataselect).toHaveBeenCalledOnce();
    });

    it('offers Wave Data deletion from the waveform without changing playback behavior', async () => {
        const waveObject = {
            ...object('SMPL', 'SMP 001'),
            sampleRate: 44_100,
            sampleWidthBytes: 2,
            frameCount: 1,
        };
        const waveData = {
            id: waveObject.key,
            objectKey: waveObject.key,
            name: waveObject.name,
            note: 'C3',
            duration: '0.00 s',
            sampleRate: '44.1 kHz',
            bitDepth: '16-bit',
            channels: 'Mono' as const,
            storedSizeBytes: 2,
            sizeWithDependenciesBytes: null,
            waveform: [],
            previewState: 'idle' as const,
            object: waveObject,
        };
        const ondeleteobject = vi.fn();
        render(ObjectWorkspace, {
            props: {
                ...common,
                waveData: [waveData],
                view: 'wave-data',
                objectDeletionAvailable: true,
                ondeleteobjects: ondeleteobject,
            },
        });

        const waveform = document.querySelector('.wave-data-row canvas');
        expect(waveform).toBeTruthy();
        await fireEvent.contextMenu(waveform!);
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Delete…' }));
        expect(ondeleteobject).toHaveBeenCalledWith([
            {
                kind: 'SMPL',
                objectId: waveObject.key,
                name: waveObject.name,
                typeLabel: 'Wave Data',
                partitionIndex: waveObject.partitionIndex,
                partitionName: waveObject.partitionName,
                volumeName: waveObject.volumeName,
            },
        ]);
    });

    it('uses the waveform for selection gestures and clears selection from empty list space', async () => {
        const waveObject = {
            ...object('SMPL', 'SMP 001'),
            sampleRate: 44_100,
            sampleWidthBytes: 2,
            frameCount: 100,
        };
        const waveData = {
            id: waveObject.key,
            objectKey: waveObject.key,
            name: waveObject.name,
            note: 'C3',
            duration: '1.00 s',
            sampleRate: '44.1 kHz',
            bitDepth: '16-bit',
            channels: 'Mono' as const,
            storedSizeBytes: 200,
            sizeWithDependenciesBytes: null,
            waveform: [{ minimum: -10, maximum: 10 }],
            previewState: 'ready' as const,
            object: waveObject,
        };
        const secondWaveObject = {
            ...object('SMPL', 'SMP 002'),
            sampleRate: 44_100,
            sampleWidthBytes: 2,
            frameCount: 100,
        };
        const secondWaveData = {
            ...waveData,
            id: secondWaveObject.key,
            objectKey: secondWaveObject.key,
            name: secondWaveObject.name,
            object: secondWaveObject,
        };
        const selected: PackageExportSelectionState = {
            items: [
                {
                    kind: 'SMPL' as const,
                    objectId: waveObject.key,
                    name: waveObject.name,
                    typeLabel: 'Wave Data',
                    partitionIndex: waveObject.partitionIndex,
                    partitionName: waveObject.partitionName,
                    volumeName: waveObject.volumeName,
                },
            ],
            anchors: { 'wave-data\u00000\u0000Volume': waveObject.key },
        };
        const onselectionchange = vi.fn();
        const onwavedataselect = vi.fn();
        const onseek = vi.fn();
        const rendered = render(ObjectWorkspace, {
            props: {
                ...common,
                waveData: [waveData, secondWaveData],
                view: 'wave-data',
                activeObjectId: waveObject.key,
                selection: selected,
                onselectionchange,
                onwavedataselect,
                onseek,
            },
        });

        const rows = [...document.querySelectorAll('.wave-data-row')];
        const row = rows[0]!;
        const waveform = row.querySelector('canvas')!;
        const secondWaveform = rows[1]!.querySelector('canvas')!;
        const selectionTarget = screen.getByRole('button', { name: 'Inspect SMP 001' });
        expect(row.classList.contains('active')).toBe(true);
        expect(row.classList.contains('selected')).toBe(true);
        expect(selectionTarget.getAttribute('aria-pressed')).toBe('true');

        await fireEvent.click(waveform, { ctrlKey: true, clientX: 50 });
        expect(onselectionchange.mock.calls.at(-1)?.[0].items).toEqual([]);
        expect(onwavedataselect).not.toHaveBeenCalled();
        expect(onseek).not.toHaveBeenCalled();

        await rendered.rerender({
            ...common,
            waveData: [waveData, secondWaveData],
            view: 'wave-data',
            activeObjectId: waveObject.key,
            selection: onselectionchange.mock.calls.at(-1)?.[0],
            onselectionchange,
            onwavedataselect,
            onseek,
        });
        expect(row.classList.contains('active')).toBe(true);
        expect(row.classList.contains('selected')).toBe(false);
        expect(selectionTarget.getAttribute('aria-pressed')).toBe('false');

        await fireEvent.click(secondWaveform, { shiftKey: true, clientX: 50 });
        expect(
            onselectionchange.mock.calls.at(-1)?.[0].items.map((item: PackageExportObject) => item.objectId),
        ).toEqual([waveObject.key, secondWaveObject.key]);
        expect(onwavedataselect).not.toHaveBeenCalled();
        expect(onseek).not.toHaveBeenCalled();

        await rendered.rerender({
            ...common,
            waveData: [waveData, secondWaveData],
            view: 'wave-data',
            activeObjectId: waveObject.key,
            selection: { items: [], anchors: {} },
            onselectionchange,
            onwavedataselect,
            onseek,
        });
        vi.spyOn(secondWaveform.closest('button')!, 'getBoundingClientRect').mockReturnValue({
            left: 0,
            width: 100,
        } as DOMRect);
        await fireEvent.click(secondWaveform, { clientX: 50 });
        expect(
            onselectionchange.mock.calls.at(-1)?.[0].items.map((item: PackageExportObject) => item.objectId),
        ).toEqual([secondWaveObject.key]);
        expect(onwavedataselect).toHaveBeenCalledWith(secondWaveData);
        expect(onseek).toHaveBeenCalledWith(secondWaveData, 0.5);

        await rendered.rerender({
            ...common,
            waveData: [waveData, secondWaveData],
            view: 'wave-data',
            activeObjectId: waveObject.key,
            selection: selected,
            onselectionchange,
            onwavedataselect,
            onseek,
        });
        await fireEvent.click(document.querySelector('.collection-body')!);
        expect(onselectionchange.mock.calls.at(-1)?.[0]).toEqual({ items: [], anchors: {} });
    });

    it('dismisses the Wave Data context menu on external pointer interactions that stop bubbling', async () => {
        const waveObject = {
            ...object('SMPL', 'SMP 001'),
            sampleRate: 44_100,
            sampleWidthBytes: 2,
            frameCount: 1,
        };
        const waveData = {
            id: waveObject.key,
            objectKey: waveObject.key,
            name: waveObject.name,
            note: 'C3',
            duration: '0.00 s',
            sampleRate: '44.1 kHz',
            bitDepth: '16-bit',
            channels: 'Mono' as const,
            storedSizeBytes: 2,
            sizeWithDependenciesBytes: null,
            waveform: [],
            previewState: 'idle' as const,
            object: waveObject,
        };
        render(ObjectWorkspace, {
            props: {
                ...common,
                waveData: [waveData],
                view: 'wave-data',
                objectDeletionAvailable: true,
            },
        });

        const waveform = document.querySelector('.wave-data-row canvas')!;
        await fireEvent.contextMenu(waveform);
        expect(screen.getByRole('menu')).toBeTruthy();
        await fireEvent.pointerDown(waveform);
        expect(screen.queryByRole('menu')).toBeNull();

        await fireEvent.contextMenu(waveform);
        expect(screen.getByRole('menu')).toBeTruthy();
        await fireEvent.pointerDown(screen.getByRole('button', { name: 'Play SMP 001' }));
        expect(screen.queryByRole('menu')).toBeNull();
    });

    it('offers direct WAV export for Wave Data', async () => {
        const waveObject = {
            ...object('SMPL', 'SMP 001'),
            sampleRate: 44_100,
            sampleWidthBytes: 2,
            frameCount: 1,
        };
        const waveData = {
            id: waveObject.key,
            objectKey: waveObject.key,
            name: waveObject.name,
            note: 'C3',
            duration: '0.00 s',
            sampleRate: '44.1 kHz',
            bitDepth: '16-bit',
            channels: 'Mono' as const,
            storedSizeBytes: 2,
            sizeWithDependenciesBytes: null,
            waveform: [],
            previewState: 'idle' as const,
            object: waveObject,
        };
        const onexportwav = vi.fn();
        render(ObjectWorkspace, {
            props: {
                ...common,
                waveData: [waveData],
                view: 'wave-data',
                audioExportAvailable: true,
                onexportwav,
            },
        });

        await fireEvent.contextMenu(document.querySelector('.wave-data-row')!);
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export WAV…' }));
        expect(onexportwav).toHaveBeenCalledWith([
            {
                kind: 'SMPL',
                objectId: waveObject.key,
                name: waveObject.name,
                typeLabel: 'Wave Data',
                partitionIndex: 0,
                partitionName: 'Partition 0',
                volumeName: 'Volume',
            },
        ]);
    });

    it('delegates play and selection as one coordinated action', async () => {
        const waveObject = {
            ...object('SMPL', 'SMP 001'),
            sampleRate: 44_100,
            sampleWidthBytes: 2,
            frameCount: 1,
        };
        const waveData = {
            id: waveObject.key,
            objectKey: waveObject.key,
            name: waveObject.name,
            note: 'C3',
            duration: '0.00 s',
            sampleRate: '44.1 kHz',
            bitDepth: '16-bit',
            channels: 'Mono' as const,
            storedSizeBytes: 2,
            sizeWithDependenciesBytes: null,
            waveform: [],
            previewState: 'idle' as const,
            object: waveObject,
        };
        const onplay = vi.fn();
        const onwavedataselect = vi.fn();
        render(ObjectWorkspace, {
            props: {
                ...common,
                waveData: [waveData],
                view: 'wave-data',
                onplay,
                onwavedataselect,
            },
        });

        await fireEvent.click(screen.getByRole('button', { name: 'Play SMP 001' }));

        expect(onplay).toHaveBeenCalledWith(waveData);
        expect(onwavedataselect).not.toHaveBeenCalled();
    });

    it('keeps preparing Wave Data cancellable from its row', async () => {
        const waveObject = {
            ...object('SMPL', 'SMP 001'),
            sampleRate: 44_100,
            sampleWidthBytes: 2,
            frameCount: 1,
        };
        const waveData = {
            id: waveObject.key,
            objectKey: waveObject.key,
            name: waveObject.name,
            note: 'C3',
            duration: '0.00 s',
            sampleRate: '44.1 kHz',
            bitDepth: '16-bit',
            channels: 'Mono' as const,
            storedSizeBytes: 2,
            sizeWithDependenciesBytes: null,
            waveform: [],
            previewState: 'idle' as const,
            object: waveObject,
        };
        const onplay = vi.fn();
        const onstop = vi.fn();
        render(ObjectWorkspace, {
            props: {
                ...common,
                waveData: [waveData],
                view: 'wave-data',
                onplay,
                onstop,
                preparingObjectId: waveData.objectKey,
            },
        });

        const stop = screen.getByRole('button', { name: 'Stop SMP 001' });
        expect(stop.hasAttribute('disabled')).toBe(false);
        await fireEvent.click(stop);

        expect(onstop).toHaveBeenCalledOnce();
        expect(onplay).not.toHaveBeenCalled();
    });

    it('prefetches Wave Data only after a deliberate playback-button hover', async () => {
        vi.useFakeTimers();
        const waveObject = {
            ...object('SMPL', 'SMP 001'),
            sampleRate: 44_100,
            sampleWidthBytes: 2,
            frameCount: 1,
        };
        const waveData = {
            id: waveObject.key,
            objectKey: waveObject.key,
            name: waveObject.name,
            note: 'C3',
            duration: '0.00 s',
            sampleRate: '44.1 kHz',
            bitDepth: '16-bit',
            channels: 'Mono' as const,
            storedSizeBytes: 2,
            sizeWithDependenciesBytes: null,
            waveform: [],
            previewState: 'idle' as const,
            object: waveObject,
        };
        const onprefetch = vi.fn();
        render(ObjectWorkspace, {
            props: { ...common, waveData: [waveData], view: 'wave-data', onprefetch },
        });
        const play = screen.getByRole('button', { name: 'Play SMP 001' });

        await fireEvent.pointerEnter(play);
        await vi.advanceTimersByTimeAsync(149);
        expect(onprefetch).not.toHaveBeenCalled();
        await vi.advanceTimersByTimeAsync(1);
        expect(onprefetch).toHaveBeenCalledWith(waveData);

        vi.useRealTimers();
    });
});
