import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import type { SamplerObject } from '../transport';
import ObjectWorkspace from './ObjectWorkspace.svelte';

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
    it.each([
        ['programs', 'No matching Programs'],
        ['wave-data', 'No matching Wave Data'],
    ] as const)('uses the shared centered empty state for %s', (view, message) => {
        render(ObjectWorkspace, { props: { ...common, view } });

        const emptyState = screen.getByText(message);
        expect(emptyState.classList.contains('empty-copy')).toBe(true);
        expect(emptyState.parentElement?.classList.contains('empty-collection')).toBe(true);
    });

    it('renders Programs as factual list rows without fabricated keyboard metadata', () => {
        const programObject = object('PROG', '001');
        render(ObjectWorkspace, {
            props: {
                ...common,
                programs: [
                    {
                        id: 'program-1',
                        objectId: programObject.key,
                        slot: '001',
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
        expect(document.querySelector('.program-row')).toBeTruthy();
        expect(document.querySelector('.object-card')).toBeNull();
        expect(screen.getByRole('searchbox', { name: 'Search Programs' })).toBeTruthy();
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

    it('offers Wave Data deletion from the selection target without changing playback behavior', async () => {
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
                ondeleteobject,
            },
        });

        await fireEvent.contextMenu(screen.getByRole('button', { name: 'Inspect SMP 001' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Delete' }));
        expect(ondeleteobject).toHaveBeenCalledWith(waveObject);
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
