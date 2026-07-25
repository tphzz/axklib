import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import type { SamplerObject } from '../transport';
import type { PackageExportSelectionState } from '../objectSelection';
import type { PackageExportObject, SampleStructureItem, WaveDataItem } from '../types';
import ContainedObjectWorkspace from './ContainedObjectWorkspace.svelte';

const appStyles = readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8');

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
        sampleRate: 44_100,
        rootKey: 60,
        frameCount: 44_100,
        sampleWidthBytes: 2,
    };
}

function structure(objectType: 'SBAC' | 'SBNK', name: string): SampleStructureItem {
    const value = object(objectType, name);
    return {
        id: value.key,
        objectId: value.key,
        objectType,
        object: value,
        name,
        memberCount: objectType === 'SBAC' ? 1 : undefined,
        membershipLabel: objectType === 'SBNK' ? 'Standalone' : undefined,
    };
}

function waveform(name: string): WaveDataItem {
    const value = object('SMPL', name);
    return {
        id: value.key,
        objectKey: value.key,
        object: value,
        name,
        note: 'C3',
        duration: '1.00 s',
        sampleRate: '44.1 kHz',
        bitDepth: '16-bit',
        channels: 'Mono',
        storedSizeBytes: 88_200,
        waveform: [],
        previewState: 'idle',
    };
}

const callbacks = {
    onquerychange: vi.fn(),
    onsamplebankselect: vi.fn(),
    onsampleselect: vi.fn(),
    onwavedataselect: vi.fn(),
};

const noAuditionableSamples = {
    auditionableSampleIds: new Set<string>(),
    auditionableSampleBankIds: new Set<string>(),
};

describe('ContainedObjectWorkspace', () => {
    it('extends row borders past playback controls while keeping them clear of overlay scrollbars', () => {
        const listRule = appStyles.match(/\.contained-list\s*\{[^}]+\}/)?.[0];
        const rowRule = appStyles.match(/\.contained-row\s*\{[^}]+\}/)?.[0];

        expect(listRule).toContain('padding: 2px 6px 5px');
        expect(listRule).not.toContain('padding-right: calc(6px + var(--overlay-scrollbar-clearance))');
        expect(listRule).toContain('scrollbar-gutter: stable');
        expect(appStyles).toContain('--contained-playback-clearance: 10px');
        expect(rowRule).toContain('grid-template-columns: minmax(0, 1fr) 26px var(--contained-playback-clearance)');
        expect(rowRule).toContain('border-bottom-color: rgb(61 68 72 / 72%)');
    });

    it('naturally orders every displayed object lane before filtering', () => {
        const bank2 = structure('SBAC', 'Bank 2');
        const bank10 = structure('SBAC', 'Bank 10');
        const sample2 = structure('SBNK', 'LoopDiv2');
        const sample10 = structure('SBNK', 'LoopDiv10');
        const wave2 = waveform('Slice 2');
        const wave10 = waveform('Slice 10');
        render(ContainedObjectWorkspace, {
            props: {
                ...callbacks,
                ...noAuditionableSamples,
                view: 'sample-banks',
                sampleBanks: [bank10, bank2],
                samples: [sample10, sample2],
                waveData: [wave10, wave2],
                activeSampleBankId: '',
                activeSampleId: '',
                activeWaveDataId: '',
                queries: { primary: 'bank', secondary: 'loop', tertiary: 'slice' },
            },
        });

        const lanes = [...document.querySelectorAll('.contained-lane')];
        expect(lanes).toHaveLength(3);
        expect(lanes[0]?.textContent?.indexOf('Bank 2')).toBeLessThan(lanes[0]?.textContent?.indexOf('Bank 10') ?? -1);
        expect(lanes[1]?.textContent?.indexOf('LoopDiv2')).toBeLessThan(
            lanes[1]?.textContent?.indexOf('LoopDiv10') ?? -1,
        );
        expect(lanes[2]?.textContent?.indexOf('Slice 2')).toBeLessThan(
            lanes[2]?.textContent?.indexOf('Slice 10') ?? -1,
        );
    });

    it('uses displayed natural order for range selection', async () => {
        const sample2 = structure('SBNK', 'LoopDiv2');
        const sample3 = structure('SBNK', 'LoopDiv3');
        const sample10 = structure('SBNK', 'LoopDiv10');
        const onselectionchange = vi.fn();
        const props = {
            ...callbacks,
            ...noAuditionableSamples,
            view: 'samples' as const,
            sampleBanks: [],
            samples: [sample10, sample2, sample3],
            waveData: [],
            activeSampleBankId: '',
            activeSampleId: '',
            activeWaveDataId: '',
            queries: { primary: '', secondary: '', tertiary: '' },
            packageExportAvailable: true,
            onselectionchange,
        };
        const rendered = render(ContainedObjectWorkspace, { props });

        await fireEvent.click(screen.getByRole('button', { name: 'Inspect LoopDiv2' }));
        const anchoredSelection = onselectionchange.mock.calls[0]![0];
        await rendered.rerender({ ...props, selection: anchoredSelection });
        await fireEvent.click(screen.getByRole('button', { name: 'Inspect LoopDiv10' }), { shiftKey: true });

        const rangeSelection = onselectionchange.mock.calls[1]![0] as PackageExportSelectionState;
        expect(new Set(rangeSelection.items.map((item) => item.objectId))).toEqual(
            new Set([sample2.objectId, sample3.objectId, sample10.objectId]),
        );
    });

    it('renders the SBAC hierarchy as three simultaneous list lanes', async () => {
        const bank = structure('SBAC', 'Strings');
        const sample = structure('SBNK', 'Violin C3');
        const waveData = waveform('Violin C3 L');
        const onsamplebankselect = vi.fn();
        const onsampleselect = vi.fn();
        const onwavedataselect = vi.fn();
        render(ContainedObjectWorkspace, {
            props: {
                ...callbacks,
                ...noAuditionableSamples,
                view: 'sample-banks',
                sampleBanks: [bank],
                samples: [sample],
                waveData: [waveData],
                activeSampleBankId: bank.objectId,
                activeSampleId: sample.objectId,
                activeWaveDataId: waveData.objectKey,
                queries: { primary: '', secondary: '', tertiary: '' },
                onsamplebankselect,
                onsampleselect,
                onwavedataselect,
            },
        });

        expect(screen.getByRole('heading', { name: 'Sample Banks' })).toBeTruthy();
        expect(screen.getByRole('heading', { name: 'Samples' })).toBeTruthy();
        expect(screen.getByRole('heading', { name: 'Wave Data' })).toBeTruthy();
        expect(document.querySelectorAll('.contained-lane')).toHaveLength(3);

        await fireEvent.click(screen.getByRole('button', { name: 'Inspect Strings' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Inspect Violin C3' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Inspect Violin C3 L' }));
        expect(onsamplebankselect).toHaveBeenCalledWith(bank);
        expect(onsampleselect).toHaveBeenCalledWith(sample);
        expect(onwavedataselect).toHaveBeenCalledWith(waveData);
    });

    it('offers deletion from mouse and keyboard context menus only when the session supports it', async () => {
        const bank = structure('SBAC', 'Strings');
        const ondeleteobject = vi.fn();
        render(ContainedObjectWorkspace, {
            props: {
                ...callbacks,
                ...noAuditionableSamples,
                view: 'sample-banks',
                sampleBanks: [bank],
                samples: [],
                waveData: [],
                activeSampleBankId: '',
                activeSampleId: '',
                activeWaveDataId: '',
                queries: { primary: '', secondary: '', tertiary: '' },
                objectDeletionAvailable: true,
                ondeleteobject,
            },
        });

        const row = screen.getByRole('button', { name: 'Inspect Strings' });
        await fireEvent.contextMenu(row, { clientX: 80, clientY: 120 });
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Delete' }));
        expect(ondeleteobject).toHaveBeenCalledWith(bank.object);

        await fireEvent.keyDown(row, { key: 'F10', shiftKey: true });
        expect(screen.getByRole('menuitem', { name: 'Delete' })).toBeTruthy();
    });

    it('offers type-safe rename targets for contained objects', async () => {
        const bank = structure('SBAC', 'Strings');
        const onrenameobject = vi.fn();
        render(ContainedObjectWorkspace, {
            props: {
                ...callbacks,
                ...noAuditionableSamples,
                view: 'sample-banks',
                sampleBanks: [bank],
                samples: [],
                waveData: [],
                activeSampleBankId: '',
                activeSampleId: '',
                activeWaveDataId: '',
                queries: { primary: '', secondary: '', tertiary: '' },
                objectRenameAvailable: true,
                onrenameobject,
            },
        });

        await fireEvent.contextMenu(screen.getByRole('button', { name: 'Inspect Strings' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Rename' }));
        expect(onrenameobject).toHaveBeenCalledWith({
            kind: 'sample-bank',
            object: bank.object,
            name: 'Strings',
        });
    });

    it('adds a Sample Bank to an externally controlled mixed selection', async () => {
        const strings = structure('SBAC', 'Strings');
        const program: PackageExportObject = {
            kind: 'PROGRAM',
            objectId: 'program',
            name: 'Program',
            typeLabel: 'Program',
            partitionIndex: 0,
            partitionName: 'Partition 0',
            volumeName: 'Volume',
        };
        const onexportobjects = vi.fn();
        const onselectionchange = vi.fn();
        const rendered = render(ContainedObjectWorkspace, {
            props: {
                ...callbacks,
                ...noAuditionableSamples,
                view: 'sample-banks',
                sampleBanks: [strings],
                samples: [],
                waveData: [],
                activeSampleBankId: '',
                activeSampleId: '',
                activeWaveDataId: '',
                queries: { primary: '', secondary: '', tertiary: '' },
                packageExportAvailable: true,
                onexportobjects,
                selection: { items: [program], anchors: {} },
                onselectionchange,
            },
        });

        const stringsRow = screen.getByRole('button', { name: 'Inspect Strings' });
        await fireEvent.click(stringsRow, { metaKey: true });
        const selection = onselectionchange.mock.calls[0]![0];
        expect(selection.items.map((item: PackageExportObject) => item.objectId)).toEqual([
            'program',
            strings.objectId,
        ]);

        await rendered.rerender({
            ...callbacks,
            ...noAuditionableSamples,
            view: 'sample-banks',
            sampleBanks: [strings],
            samples: [],
            waveData: [],
            activeSampleBankId: '',
            activeSampleId: '',
            activeWaveDataId: '',
            queries: { primary: '', secondary: '', tertiary: '' },
            packageExportAvailable: true,
            onexportobjects,
            selection,
            onselectionchange,
        });
        await fireEvent.contextMenu(screen.getByRole('button', { name: 'Inspect Strings' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Export 2 objects' }));

        expect(onexportobjects).toHaveBeenCalledWith([
            program,
            {
                kind: 'SBAC',
                objectId: strings.objectId,
                name: 'Strings',
                typeLabel: 'Sample Bank',
                partitionIndex: 0,
                partitionName: 'Partition 0',
                volumeName: 'Volume',
            },
        ]);
    });

    it('renders the SBNK hierarchy as two lanes and filters each lane independently', async () => {
        const piano = structure('SBNK', 'Piano C3');
        const brass = structure('SBNK', 'Brass C3');
        const pianoWave = waveform('Piano C3 L');
        render(ContainedObjectWorkspace, {
            props: {
                ...callbacks,
                ...noAuditionableSamples,
                view: 'samples',
                sampleBanks: [],
                samples: [piano, brass],
                waveData: [pianoWave],
                activeSampleBankId: '',
                activeSampleId: piano.objectId,
                activeWaveDataId: '',
                queries: { primary: 'piano', secondary: 'missing', tertiary: '' },
            },
        });

        expect(screen.queryByRole('heading', { name: 'Sample Banks' })).toBeNull();
        expect(document.querySelectorAll('.contained-lane')).toHaveLength(2);
        expect(screen.getByText('Piano C3')).toBeTruthy();
        expect(screen.queryByText('Brass C3')).toBeNull();
        expect(screen.queryByText('Piano C3 L')).toBeNull();
        expect(screen.getByText('No matching Wave Data')).toBeTruthy();

        const searches = screen.getAllByRole('searchbox');
        await fireEvent.input(searches[0]!, { target: { value: 'brass' } });
        await fireEvent.input(searches[1]!, { target: { value: 'piano' } });
        expect(callbacks.onquerychange).toHaveBeenCalledWith('primary', 'brass');
        expect(callbacks.onquerychange).toHaveBeenCalledWith('secondary', 'piano');
    });

    it('places playback beside each playable row and exposes active stop states', async () => {
        const bank = structure('SBAC', 'Strings');
        const sample = structure('SBNK', 'Violin C3');
        const waveData = waveform('Violin C3 L');
        const onplaysamplebank = vi.fn();
        const onplaysample = vi.fn();
        const onplaywavedata = vi.fn();
        const onstop = vi.fn();
        render(ContainedObjectWorkspace, {
            props: {
                ...callbacks,
                view: 'sample-banks',
                sampleBanks: [bank],
                samples: [sample],
                waveData: [waveData],
                activeSampleBankId: bank.objectId,
                activeSampleId: sample.objectId,
                activeWaveDataId: waveData.objectKey,
                queries: { primary: '', secondary: '', tertiary: '' },
                onplaysamplebank,
                onplaysample,
                onplaywavedata,
                onstop,
                playingSampleBankId: bank.objectId,
                playingObjectId: sample.objectId,
                auditionableSampleIds: new Set([sample.objectId]),
                auditionableSampleBankIds: new Set([bank.objectId]),
            },
        });

        expect(document.querySelector('button button')).toBeNull();
        await fireEvent.click(screen.getByRole('button', { name: 'Stop Strings' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Stop Violin C3' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Play Violin C3 L' }));

        expect(onstop).toHaveBeenCalledTimes(2);
        expect(onplaysamplebank).not.toHaveBeenCalled();
        expect(onplaysample).not.toHaveBeenCalled();
        expect(onplaywavedata).toHaveBeenCalledWith(waveData);
    });

    it('keeps a preparing row cancellable from its playback control', async () => {
        const sample = structure('SBNK', 'Violin C3');
        const onplaysample = vi.fn();
        const onstop = vi.fn();
        render(ContainedObjectWorkspace, {
            props: {
                ...callbacks,
                view: 'samples',
                sampleBanks: [],
                samples: [sample],
                waveData: [],
                activeSampleBankId: '',
                activeSampleId: sample.objectId,
                activeWaveDataId: '',
                queries: { primary: '', secondary: '', tertiary: '' },
                onplaysample,
                onstop,
                preparingObjectId: sample.objectId,
                auditionableSampleIds: new Set([sample.objectId]),
                auditionableSampleBankIds: new Set<string>(),
            },
        });

        const stop = screen.getByRole('button', { name: 'Stop Violin C3' });
        expect(stop.hasAttribute('disabled')).toBe(false);
        await fireEvent.click(stop);

        expect(onstop).toHaveBeenCalledOnce();
        expect(onplaysample).not.toHaveBeenCalled();
    });

    it('disables Sample and Sample Bank audition without confirmed Wave Data while retaining direct playback', async () => {
        const bank = structure('SBAC', 'Broken Bank');
        const sample = structure('SBNK', 'Broken Sample');
        const candidate = waveform('Candidate Wave Data');
        const onplaysamplebank = vi.fn();
        const onplaysample = vi.fn();
        const onplaywavedata = vi.fn();
        render(ContainedObjectWorkspace, {
            props: {
                ...callbacks,
                ...noAuditionableSamples,
                view: 'sample-banks',
                sampleBanks: [bank],
                samples: [sample],
                waveData: [candidate],
                activeSampleBankId: bank.objectId,
                activeSampleId: sample.objectId,
                activeWaveDataId: '',
                queries: { primary: '', secondary: '', tertiary: '' },
                onplaysamplebank,
                onplaysample,
                onplaywavedata,
            },
        });

        const bankPlayback = screen.getByRole('button', { name: 'Broken Bank cannot be auditioned' });
        const samplePlayback = screen.getByRole('button', { name: 'Broken Sample cannot be auditioned' });
        expect(bankPlayback.hasAttribute('disabled')).toBe(true);
        expect(samplePlayback.hasAttribute('disabled')).toBe(true);
        await fireEvent.click(bankPlayback);
        await fireEvent.click(samplePlayback);
        await fireEvent.click(screen.getByRole('button', { name: 'Play Candidate Wave Data' }));

        expect(onplaysamplebank).not.toHaveBeenCalled();
        expect(onplaysample).not.toHaveBeenCalled();
        expect(onplaywavedata).toHaveBeenCalledWith(candidate);
    });
});
