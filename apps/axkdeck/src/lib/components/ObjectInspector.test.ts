import { render, screen } from '@testing-library/svelte';
import { describe, expect, it } from 'vitest';
import type { SamplerObject } from '../transport';
import type {
    InspectorSelection,
    LinkedWaveDataItem,
    SampleStructureItem,
    SampleWaveformPreview,
    WaveDataItem,
} from '../types';
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
        objectEncoding: 'current',
        directoryEntryName: `${name}.001`,
        sfsId: 0,
        storedSizeBytes: 128,
        sizeWithDependenciesBytes: 1024,
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

function samplePreview(
    item: SampleStructureItem,
    waveData: LinkedWaveDataItem[],
    previewState: SampleWaveformPreview['previewState'] = 'ready',
    frameCount = Math.max(0, ...waveData.map((entry) => entry.waveData.object.frameCount)),
    laneFrameCounts = waveData.map((entry) => entry.waveData.object.frameCount),
): SampleWaveformPreview {
    return {
        item,
        waveData,
        previewState,
        preview:
            previewState === 'ready'
                ? {
                      frameCount,
                      lanes: waveData.map((entry, index) => ({
                          role: entry.role === 'left' ? 'LEFT' : 'RIGHT',
                          sourceObjectId: entry.waveData.objectKey,
                          frameCount: laneFrameCounts[index] ?? frameCount,
                          bins: entry.waveData.waveform,
                      })),
                  }
                : null,
    };
}

function headingOutline(container: HTMLElement): string[] {
    return Array.from(
        container.querySelectorAll('h2, h3, h4, h5'),
        (heading) => heading.textContent?.trim().replace(/\s+/g, ' ') ?? '',
    );
}

function programSelection(): Extract<InspectorSelection, { kind: 'program' }> {
    const programObject = object('PROG', '001');
    return {
        kind: 'program',
        program: {
            id: programObject.key,
            objectId: programObject.key,
            slot: '001',
            programNumber: 1,
            name: 'REZO LD',
            object: programObject,
        },
        assignments: [],
        sampleSelect: { assigned: [], all: [] },
    };
}

describe('ObjectInspector', () => {
    it('uses the common identity, Properties, and Relationships hierarchy for Programs', () => {
        const { container } = render(ObjectInspector, { props: { selection: programSelection() } });

        expect(headingOutline(container)).toEqual(['Program details', 'REZO LD', 'Properties', 'Relationships']);
        expect(screen.queryByRole('heading', { name: 'Preview' })).toBeNull();
    });

    it('shows decoded timing and storage metadata for a selected Sequence', () => {
        const sequenceObject = {
            ...object('SEQU', 'DJ TSUYOSHI DEMO'),
            storedSizeBytes: 41_204,
            sequence: {
                formatVersion: 1,
                ticksPerQuarterNote: 96,
                firstTick: 0,
                endTick: 40_408,
                eventCount: 6_735,
                headerTempoBpm: 130,
                effectiveInitialTempoMicrosecondsPerQuarterNote: 460_122,
                tempoEvents: [
                    { tick: 0, microsecondsPerQuarterNote: 460_122 },
                    { tick: 9_600, microsecondsPerQuarterNote: 400_000 },
                ],
            },
        };
        const { container } = render(ObjectInspector, {
            props: {
                selection: {
                    kind: 'sequence',
                    sequence: {
                        id: sequenceObject.key,
                        objectId: sequenceObject.key,
                        name: sequenceObject.name,
                        object: sequenceObject,
                    },
                },
            },
        });

        expect(screen.getByRole('heading', { name: 'Sequence details' })).toBeTruthy();
        expect(screen.getByText('DJ TSUYOSHI DEMO')).toBeTruthy();
        expect(screen.getByText('6,735')).toBeTruthy();
        expect(screen.getByText('96')).toBeTruthy();
        expect(screen.getByText('130.4 BPM')).toBeTruthy();
        expect(screen.getByText('130 BPM')).toBeTruthy();
        expect(screen.getByText('1 change')).toBeTruthy();
        expect(screen.getByText('40,408')).toBeTruthy();
        expect(headingOutline(container)).toEqual([
            'Sequence details',
            'DJ TSUYOSHI DEMO',
            'Properties',
            'Relationships',
        ]);
    });

    it('shows structural metadata for a selected SBAC', () => {
        const bankObject = object('SBAC', 'STRINGS');
        const sampleObject = object('SBNK', 'Strings C3');
        const sampleItem: SampleStructureItem = {
            id: sampleObject.key,
            objectId: sampleObject.key,
            name: 'Strings C3',
            objectType: 'SBNK',
            object: sampleObject,
        };
        const { container } = render(ObjectInspector, {
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
                    members: [sampleItem],
                    memberPreviews: [
                        samplePreview(
                            sampleItem,
                            [member('left', waveData('Strings C3 Wave', 44_100, 'ready'))],
                            'ready',
                        ),
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
        expect(screen.getByText('128 B')).toBeTruthy();
        expect(screen.getByText('1 KiB')).toBeTruthy();
        expect(headingOutline(container)).toEqual([
            'Sample Bank details',
            'STRINGS',
            'Preview',
            'Properties',
            'Relationships',
        ]);
    });

    it('marks an incomplete dependency closure as unavailable', () => {
        const sampleObject = { ...object('SBNK', 'Incomplete'), sizeWithDependenciesBytes: null };
        const item = {
            id: sampleObject.key,
            objectId: sampleObject.key,
            name: sampleObject.name,
            objectType: 'SBNK' as const,
            object: sampleObject,
        };
        render(ObjectInspector, {
            props: {
                selection: {
                    kind: 'sample',
                    item,
                    memberships: [],
                    preview: samplePreview(item, [], 'failed'),
                },
            },
        });

        expect(screen.getByText('Object size with deps.')).toBeTruthy();
        expect(screen.getByText('Unavailable')).toBeTruthy();
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
            samplePreview(first, [member('left', waveData('Kick Wave', 44_100, 'ready'))]),
            samplePreview(second, [member('left', waveData('Snare Wave', 22_050, 'ready'))]),
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
                memberPreviews: [samplePreview(sample, [], 'idle')],
                displayedMemberId: sample.objectId,
            },
        });

        expect(screen.getByText('No resolved Wave Data')).toBeTruthy();
    });

    it('keeps the inspector visible without a selection', () => {
        render(ObjectInspector, { props: { selection: null } });
        const inspector = screen.getByRole('complementary', { name: 'Object inspector' });
        const heading = screen.getByRole('heading', { name: 'Object details' });
        expect(inspector).toBeTruthy();
        expect(heading).toBeTruthy();
        const emptyState = screen.getByText('No object selected');
        expect(emptyState.classList.contains('empty-copy')).toBe(true);
        expect(emptyState.closest('.inspector-empty')?.querySelector('svg')).toBeNull();
        const body = inspector.querySelector('.inspector-body');
        expect(body).toBeTruthy();
        expect(body?.contains(emptyState)).toBe(true);
        expect(body?.contains(heading)).toBe(false);
    });

    it('keeps playback controls out of the inspector', () => {
        const sampleObject = object('SBNK', 'Stereo Pad');
        const item: SampleStructureItem = {
            id: sampleObject.key,
            objectId: sampleObject.key,
            name: sampleObject.name,
            objectType: 'SBNK',
            object: sampleObject,
        };
        const { container } = render(ObjectInspector, {
            props: {
                selection: {
                    kind: 'sample',
                    item,
                    memberships: [],
                    preview: samplePreview(item, [], 'idle'),
                },
            },
        });

        expect(screen.queryByRole('button', { name: 'Play Stereo Pad' })).toBeNull();
        expect(screen.getByText('Sample')).toBeTruthy();
        expect(headingOutline(container)).toEqual([
            'Sample details',
            'Stereo Pad',
            'Preview',
            'Properties',
            'Relationships',
        ]);
    });

    it('shows linked stereo Wave Data in separate role-labelled lanes on one timeline', () => {
        const sampleObject = object('SBNK', 'Stereo Pad');
        const item: SampleStructureItem = {
            id: sampleObject.key,
            objectId: sampleObject.key,
            name: sampleObject.name,
            objectType: 'SBNK',
            object: sampleObject,
        };
        const linked = [
            member('left', waveData('Stereo Pad L', 44_100, 'ready')),
            member('right', waveData('Stereo Pad R', 22_050, 'ready')),
        ];
        render(ObjectInspector, {
            props: {
                selection: {
                    kind: 'sample',
                    item,
                    memberships: [],
                    preview: samplePreview(item, linked),
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

    it('uses the Sample preview timeline instead of the full physical Wave Data length', () => {
        const sampleObject = object('SBNK', 'Loop Divide 10');
        const item: SampleStructureItem = {
            id: sampleObject.key,
            objectId: sampleObject.key,
            name: sampleObject.name,
            objectType: 'SBNK',
            object: sampleObject,
        };
        const linked = [member('left', waveData('Shared Loop Wave', 81_419, 'ready'))];
        render(ObjectInspector, {
            props: {
                selection: {
                    kind: 'sample',
                    item,
                    memberships: [],
                    preview: samplePreview(item, linked, 'ready', 20_352, [20_352]),
                },
                playingObjectId: sampleObject.key,
                playheadFrame: 10_176,
            },
        });

        expect(document.querySelector('[data-playhead-ratio="0.5"]')).toBeTruthy();
        expect(document.querySelector('[data-content-ratio="1"]')).toBeTruthy();
    });

    it('uses a neutral lane label for mono SBNK Wave Data', () => {
        const sampleObject = object('SBNK', 'Mono Bass');
        const item: SampleStructureItem = {
            id: sampleObject.key,
            objectId: sampleObject.key,
            name: sampleObject.name,
            objectType: 'SBNK',
            object: sampleObject,
        };
        render(ObjectInspector, {
            props: {
                selection: {
                    kind: 'sample',
                    item,
                    memberships: [],
                    preview: samplePreview(
                        item,
                        [member('left', waveData('Mono Bass Wave', 44_100, 'loading'))],
                        'loading',
                    ),
                },
            },
        });

        expect(screen.getByRole('group', { name: 'Wave Data Mono Bass Wave' })).toBeTruthy();
        expect(screen.getByText('Loading waveform')).toBeTruthy();
    });

    it('reports an unavailable SBNK waveform without hiding the member identity', () => {
        const sampleObject = object('SBNK', 'Broken Sample');
        const item: SampleStructureItem = {
            id: sampleObject.key,
            objectId: sampleObject.key,
            name: sampleObject.name,
            objectType: 'SBNK',
            object: sampleObject,
        };
        render(ObjectInspector, {
            props: {
                selection: {
                    kind: 'sample',
                    item,
                    memberships: [],
                    preview: samplePreview(
                        item,
                        [member('left', waveData('Known Wave Name', 44_100, 'failed'))],
                        'failed',
                    ),
                },
            },
        });

        expect(screen.getByRole('group', { name: 'Wave Data Known Wave Name' })).toBeTruthy();
        expect(screen.getByText('Waveform unavailable')).toBeTruthy();
    });

    it('shows decoded Wave Data object and loop metadata', () => {
        const item = waveData('Displayed Wave', 88_200, 'ready');
        Object.assign(item.object, {
            objectEncoding: 'current',
            directoryEntryName: 'WAVE0001.001',
            sourceWaveName: 'Sampler Source',
            rootKey: 60,
            fineTuneCents: -7,
            loopMode: 2,
            loopModeLabel: '->0->',
            loopStartFrame: 1_234,
            loopLengthFrames: 400,
        });

        const { container } = render(ObjectInspector, {
            props: { selection: { kind: 'wave-data', waveData: item } },
        });

        expect(screen.getByText('Object encoding')).toBeTruthy();
        expect(screen.getByText('Current A-series')).toBeTruthy();
        expect(screen.getByText('Directory entry')).toBeTruthy();
        expect(screen.getByText('WAVE0001.001')).toBeTruthy();
        expect(screen.getByText('Source Wave Data name')).toBeTruthy();
        expect(screen.getByText('Sampler Source')).toBeTruthy();
        expect(screen.getByText('C3')).toBeTruthy();
        expect(screen.getByText('-7 cents')).toBeTruthy();
        expect(screen.getByText('Audio format')).toBeTruthy();
        expect(screen.getByText('->0->')).toBeTruthy();
        expect(screen.getByText('1,234 frames')).toBeTruthy();
        expect(screen.getByText('1,634 frames')).toBeTruthy();
        expect(screen.getByText('400 frames')).toBeTruthy();
        expect(headingOutline(container)).toEqual([
            'Wave Data details',
            'Displayed Wave',
            'Preview',
            'Properties',
            'Relationships',
        ]);
    });

    it('does not repeat the source Wave Data name when it is the displayed name', () => {
        const item = waveData('Same Wave', 44_100, 'ready');
        Object.assign(item.object, { sourceWaveName: ' Same Wave ' });

        render(ObjectInspector, { props: { selection: { kind: 'wave-data', waveData: item } } });

        expect(screen.queryByText('Source Wave Data name')).toBeNull();
    });

    it('explains when an SBNK has no resolved Wave Data', () => {
        const sampleObject = object('SBNK', 'Unresolved Sample');
        const item: SampleStructureItem = {
            id: sampleObject.key,
            objectId: sampleObject.key,
            name: sampleObject.name,
            objectType: 'SBNK',
            object: sampleObject,
        };
        render(ObjectInspector, {
            props: {
                selection: {
                    kind: 'sample',
                    item,
                    memberships: [],
                    preview: samplePreview(item, [], 'idle'),
                },
            },
        });

        expect(screen.getByText('No resolved Wave Data')).toBeTruthy();
    });
});
