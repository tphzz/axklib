import { render, screen } from '@testing-library/svelte';
import { describe, expect, it } from 'vitest';
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
        const bankObject = object('SBAC', 'STRINGS');
        const sampleObject = object('SBNK', 'Strings C3');
        render(ObjectInspector, {
            props: {
                selection: {
                    kind: 'sample-bank',
                    item: {
                        id: bankObject.key,
                        objectId: bankObject.key,
                        name: 'STRINGS',
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
                    memberPreviews: [
                        {
                            item: {
                                id: sampleObject.key,
                                objectId: sampleObject.key,
                                name: 'Strings C3',
                                objectType: 'SBNK',
                                object: sampleObject,
                            },
                            waveData: [member('left', waveData('Strings C3 Wave', 44_100, 'ready'))],
                        },
                    ],
                    displayedMemberId: sampleObject.key,
                },
            },
        });

        expect(screen.getByRole('heading', { name: 'Sample Bank details' })).toBeTruthy();
        expect(screen.getByText('STRINGS')).toBeTruthy();
        expect(screen.getByText('Sample 1 of 1')).toBeTruthy();
        expect(screen.getByRole('group', { name: 'Wave Data Strings C3 Wave' })).toBeTruthy();
        expect(document.querySelectorAll('.inspector-inline-heading')).toHaveLength(2);
        expect(screen.getByText('1')).toBeTruthy();
    });

    it('switches the Sample Bank waveform to the currently playing member', async () => {
        const bankObject = object('SBAC', 'DRUMS');
        const firstObject = object('SBNK', 'Kick');
        const secondObject = object('SBNK', 'Snare');
        const item = {
            id: bankObject.key,
            objectId: bankObject.key,
            name: bankObject.name,
            objectType: 'SBAC' as const,
            object: bankObject,
        };
        const first = {
            id: firstObject.key,
            objectId: firstObject.key,
            name: firstObject.name,
            objectType: 'SBNK' as const,
            object: firstObject,
        };
        const second = {
            id: secondObject.key,
            objectId: secondObject.key,
            name: secondObject.name,
            objectType: 'SBNK' as const,
            object: secondObject,
        };
        const memberPreviews = [
            { item: first, waveData: [member('left', waveData('Kick Wave', 44_100, 'ready'))] },
            { item: second, waveData: [member('left', waveData('Snare Wave', 22_050, 'ready'))] },
        ];
        const { rerender } = render(ObjectInspector, {
            props: {
                selection: {
                    kind: 'sample-bank',
                    item,
                    members: [first, second],
                    memberPreviews,
                    displayedMemberId: first.objectId,
                },
                playingObjectId: first.objectId,
                playheadFrame: 11_025,
            },
        });

        expect(screen.getByText('Sample 1 of 2')).toBeTruthy();
        expect(screen.getByRole('group', { name: 'Wave Data Kick Wave' })).toBeTruthy();
        expect(document.querySelector('[data-playhead-ratio="0.25"]')).toBeTruthy();

        await rerender({
            selection: {
                kind: 'sample-bank',
                item,
                members: [first, second],
                memberPreviews,
                displayedMemberId: second.objectId,
            },
            playingObjectId: second.objectId,
            playheadFrame: 5_512.5,
        });

        expect(screen.getByText('Sample 2 of 2')).toBeTruthy();
        expect(screen.getByRole('group', { name: 'Wave Data Snare Wave' })).toBeTruthy();
        expect(screen.queryByRole('group', { name: 'Wave Data Kick Wave' })).toBeNull();
        expect(document.querySelector('[data-playhead-ratio="0.25"]')).toBeTruthy();

        await rerender({
            selection: {
                kind: 'sample-bank',
                item,
                members: [first, second],
                memberPreviews,
                displayedMemberId: first.objectId,
            },
            playingObjectId: null,
            playheadFrame: 0,
        });

        expect(screen.getByText('Sample 1 of 2')).toBeTruthy();
        expect(screen.getByRole('group', { name: 'Wave Data Kick Wave' })).toBeTruthy();
        expect(document.querySelector('[data-playhead-ratio="0"]')).toBeTruthy();
    });

    it('distinguishes empty Sample Banks from members with unresolved Wave Data', async () => {
        const bankObject = object('SBAC', 'EMPTY');
        const item = {
            id: bankObject.key,
            objectId: bankObject.key,
            name: bankObject.name,
            objectType: 'SBAC' as const,
            object: bankObject,
        };
        const sampleObject = object('SBNK', 'Unresolved');
        const sample = {
            id: sampleObject.key,
            objectId: sampleObject.key,
            name: sampleObject.name,
            objectType: 'SBNK' as const,
            object: sampleObject,
        };
        const { rerender } = render(ObjectInspector, {
            props: {
                selection: {
                    kind: 'sample-bank',
                    item,
                    members: [],
                    memberPreviews: [],
                    displayedMemberId: '',
                },
            },
        });

        expect(screen.getByText('No Samples')).toBeTruthy();

        await rerender({
            selection: {
                kind: 'sample-bank',
                item,
                members: [sample],
                memberPreviews: [{ item: sample, waveData: [] }],
                displayedMemberId: sample.objectId,
            },
        });

        expect(screen.getByText('No resolved Wave Data')).toBeTruthy();
    });

    it('keeps the inspector visible without a selection', () => {
        render(ObjectInspector, { props: { selection: null } });
        expect(screen.getByRole('complementary', { name: 'Object inspector' })).toBeTruthy();
        expect(screen.getByRole('heading', { name: 'Object details' })).toBeTruthy();
        const emptyState = screen.getByText('No object selected');
        expect(emptyState.classList.contains('empty-copy')).toBe(true);
        expect(emptyState.closest('.inspector-empty')?.querySelector('svg')).toBeNull();
    });

    it('keeps playback controls out of the inspector', () => {
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
            },
        });

        expect(screen.queryByRole('button', { name: 'Play Stereo Pad' })).toBeNull();
        expect(screen.getByText('Sample')).toBeTruthy();
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
