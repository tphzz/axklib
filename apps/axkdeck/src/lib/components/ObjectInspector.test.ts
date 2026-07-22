import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it } from 'vitest';
import { vi } from 'vitest';
import type { SamplerObject } from '../transport';
import type { LinkedWaveDataItem, WaveDataItem } from '../types';
import ObjectInspector from './ObjectInspector.svelte';

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

function waveData(name: string, frameCount: number, previewState: WaveDataItem['previewState']): WaveDataItem {
    const waveObject = {
        ...object('SMPL', name),
        frameCount,
        sampleRate: 44_100,
        sampleWidthBytes: 2,
        storedSizeBytes: frameCount * 2,
    };
    return {
        id: waveObject.key,
        objectKey: waveObject.key,
        object: waveObject,
        name,
        note: 'C3',
        duration: `${(frameCount / 44_100).toFixed(2)} s`,
        sampleRate: '44.1 kHz',
        bitDepth: '16-bit',
        channels: 'Mono',
        storedSizeBytes: waveObject.storedSizeBytes,
        waveform: [{ minimum: -10, maximum: 12 }],
        previewState,
    };
}

function member(role: LinkedWaveDataItem['role'], item: WaveDataItem): LinkedWaveDataItem {
    return { role, waveData: item };
}

describe('ObjectInspector', () => {
    it('shows structural metadata for a selected SBAC', () => {
        const bankObject = object('SBAC', 'B STRINGS');
        const sampleObject = object('SBNK', 'Strings C3');
        render(ObjectInspector, {
            props: {
                selection: {
                    kind: 'sample-bank',
                    item: {
                        id: bankObject.key,
                        objectId: bankObject.key,
                        name: 'B STRINGS',
                        objectType: 'SBAC',
                        object: bankObject,
                    },
                    members: [
                        {
                            id: sampleObject.key,
                            objectId: sampleObject.key,
                            name: 'Strings C3',
                            objectType: 'SBNK',
                            object: sampleObject,
                        },
                    ],
                },
            },
        });

        expect(screen.getByRole('heading', { name: 'Sample Bank details' })).toBeTruthy();
        expect(screen.getByText('B STRINGS')).toBeTruthy();
        expect(screen.getByText('1')).toBeTruthy();
    });

    it('keeps the inspector visible without a selection', () => {
        render(ObjectInspector, { props: { selection: null } });
        expect(screen.getByRole('complementary', { name: 'Object inspector' })).toBeTruthy();
        expect(screen.getByRole('heading', { name: 'Object details' })).toBeTruthy();
        const emptyState = screen.getByText('No object selected');
        expect(emptyState.classList.contains('empty-copy')).toBe(true);
        expect(emptyState.closest('.inspector-empty')?.querySelector('svg')).toBeNull();
    });

    it('auditions a selected SBNK through the shared playback control', async () => {
        const onplay = vi.fn();
        const sampleObject = object('SBNK', 'Stereo Pad');
        render(ObjectInspector, {
            props: {
                selection: {
                    kind: 'sample',
                    item: {
                        id: sampleObject.key,
                        objectId: sampleObject.key,
                        name: sampleObject.name,
                        objectType: 'SBNK',
                        object: sampleObject,
                    },
                    memberships: [],
                    waveData: [],
                },
                onplay,
            },
        });

        await fireEvent.click(screen.getByRole('button', { name: 'Play Stereo Pad' }));
        expect(onplay).toHaveBeenCalledWith(sampleObject.key);
    });

    it('shows linked stereo Wave Data in separate role-labelled lanes on one timeline', () => {
        const sampleObject = object('SBNK', 'Stereo Pad');
        render(ObjectInspector, {
            props: {
                selection: {
                    kind: 'sample',
                    item: {
                        id: sampleObject.key,
                        objectId: sampleObject.key,
                        name: sampleObject.name,
                        objectType: 'SBNK',
                        object: sampleObject,
                    },
                    memberships: [],
                    waveData: [
                        member('left', waveData('Stereo Pad L', 44_100, 'ready')),
                        member('right', waveData('Stereo Pad R', 22_050, 'ready')),
                    ],
                },
                playingObjectId: sampleObject.key,
                playheadFrame: 11_025,
            },
        });

        expect(screen.getByRole('group', { name: 'Left Wave Data Stereo Pad L' })).toBeTruthy();
        expect(screen.getByRole('group', { name: 'Right Wave Data Stereo Pad R' })).toBeTruthy();
        expect(document.querySelectorAll('.inspector-wave-lane canvas')).toHaveLength(2);
        expect(document.querySelector('[data-content-ratio="0.5"]')).toBeTruthy();
        expect(document.querySelectorAll('[data-playhead-ratio="0.25"]')).toHaveLength(2);
    });

    it('uses a neutral lane label for mono SBNK Wave Data', () => {
        const sampleObject = object('SBNK', 'Mono Bass');
        render(ObjectInspector, {
            props: {
                selection: {
                    kind: 'sample',
                    item: {
                        id: sampleObject.key,
                        objectId: sampleObject.key,
                        name: sampleObject.name,
                        objectType: 'SBNK',
                        object: sampleObject,
                    },
                    memberships: [],
                    waveData: [member('left', waveData('Mono Bass Wave', 44_100, 'loading'))],
                },
            },
        });

        expect(screen.getByRole('group', { name: 'Wave Data Mono Bass Wave' })).toBeTruthy();
        expect(screen.getByText('Loading waveform')).toBeTruthy();
    });

    it('reports an unavailable SBNK waveform without hiding the member identity', () => {
        const sampleObject = object('SBNK', 'Broken Sample');
        render(ObjectInspector, {
            props: {
                selection: {
                    kind: 'sample',
                    item: {
                        id: sampleObject.key,
                        objectId: sampleObject.key,
                        name: sampleObject.name,
                        objectType: 'SBNK',
                        object: sampleObject,
                    },
                    memberships: [],
                    waveData: [member('left', waveData('Known Wave Name', 44_100, 'failed'))],
                },
            },
        });

        expect(screen.getByRole('group', { name: 'Wave Data Known Wave Name' })).toBeTruthy();
        expect(screen.getByText('Waveform unavailable')).toBeTruthy();
    });

    it('explains when an SBNK has no resolved Wave Data', () => {
        const sampleObject = object('SBNK', 'Unresolved Sample');
        render(ObjectInspector, {
            props: {
                selection: {
                    kind: 'sample',
                    item: {
                        id: sampleObject.key,
                        objectId: sampleObject.key,
                        name: sampleObject.name,
                        objectType: 'SBNK',
                        object: sampleObject,
                    },
                    memberships: [],
                    waveData: [],
                },
            },
        });

        expect(screen.getByText('No resolved Wave Data')).toBeTruthy();
    });
});
