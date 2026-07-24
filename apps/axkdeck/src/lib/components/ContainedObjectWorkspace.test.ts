import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import type { SamplerObject } from '../transport';
import type { SampleStructureItem, WaveDataItem } from '../types';
import ContainedObjectWorkspace from './ContainedObjectWorkspace.svelte';

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

describe('ContainedObjectWorkspace', () => {
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

    it('renders the SBNK hierarchy as two lanes and filters each lane independently', async () => {
        const piano = structure('SBNK', 'Piano C3');
        const brass = structure('SBNK', 'Brass C3');
        const pianoWave = waveform('Piano C3 L');
        render(ContainedObjectWorkspace, {
            props: {
                ...callbacks,
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
            },
        });

        const stop = screen.getByRole('button', { name: 'Stop Violin C3' });
        expect(stop.hasAttribute('disabled')).toBe(false);
        await fireEvent.click(stop);

        expect(onstop).toHaveBeenCalledOnce();
        expect(onplaysample).not.toHaveBeenCalled();
    });
});
