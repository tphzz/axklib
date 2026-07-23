import { describe, expect, it } from 'vitest';
import type { SamplerObject, SamplerRelationship } from './transport';
import type { SampleStructureItem, WaveDataItem } from './types';
import { distinctWaveDataForSample, linkedWaveDataForSample, orderedSamplesForBank } from './sampleRelationships';

function relationship(id: string, type: string, targetObjectId?: string): SamplerRelationship {
    return {
        id,
        sourceObjectId: 'SBNK-1',
        targetObjectId,
        candidateObjectIds: [],
        relationshipType: type,
        quality: 'known',
        basis: 'test',
        notes: [],
        assignmentName: '',
        assignmentState: '',
        receiveChannelDisplay: '',
    };
}

function waveData(id: string): WaveDataItem {
    const object: SamplerObject = {
        key: id,
        objectType: 'SMPL',
        name: id,
        partitionIndex: 0,
        partitionName: 'Partition 0',
        volumeName: 'Volume',
        categoryName: 'SMPL',
        sfsId: 0,
        storedSizeBytes: 2,
        sampleRate: 44_100,
        rootKey: 60,
        frameCount: 1,
        sampleWidthBytes: 2,
    };
    return {
        id,
        objectKey: id,
        object,
        name: id,
        note: 'C3',
        duration: '0.00 s',
        sampleRate: '44.1 kHz',
        bitDepth: '16-bit',
        channels: 'Mono',
        storedSizeBytes: 2,
        waveform: [],
        previewState: 'idle',
    };
}

function sample(id: string): SampleStructureItem {
    const value = {
        ...waveData(id).object,
        key: id,
        name: id,
        objectType: 'SBNK',
    };
    return { id, objectId: id, objectType: 'SBNK', object: value, name: id };
}

describe('linkedWaveDataForSample', () => {
    it('preserves left/right roles and ignores unresolved or missing targets', () => {
        const left = waveData('SMPL-L');
        const right = waveData('SMPL-R');
        const result = linkedWaveDataForSample(
            'SBNK-1',
            [
                relationship('right', 'SBNK_RIGHT_MEMBER_TO_SMPL', right.objectKey),
                relationship('missing', 'SBNK_LEFT_MEMBER_TO_SMPL', 'SMPL-MISSING'),
                relationship('unresolved', 'SBNK_LEFT_MEMBER_TO_SMPL'),
                relationship('left', 'SBNK_LEFT_MEMBER_TO_SMPL', left.objectKey),
            ],
            [left, right],
        );

        expect(result).toEqual([
            { role: 'left', waveData: left },
            { role: 'right', waveData: right },
        ]);
    });

    it('preserves both member roles when they refer to one physical Wave Data object', () => {
        const shared = waveData('SMPL-SHARED');
        const result = linkedWaveDataForSample(
            'SBNK-1',
            [
                relationship('left', 'SBNK_LEFT_MEMBER_TO_SMPL', shared.objectKey),
                relationship('right', 'SBNK_RIGHT_MEMBER_TO_SMPL', shared.objectKey),
            ],
            [shared],
        );

        expect(result).toEqual([
            { role: 'left', waveData: shared },
            { role: 'right', waveData: shared },
        ]);
    });

    it('collapses duplicate records for the same member role and physical object', () => {
        const left = waveData('SMPL-L');
        const result = linkedWaveDataForSample(
            'SBNK-1',
            [
                relationship('left-1', 'SBNK_LEFT_MEMBER_TO_SMPL', left.objectKey),
                relationship('left-2', 'SBNK_LEFT_MEMBER_TO_SMPL', left.objectKey),
            ],
            [left],
        );

        expect(result).toEqual([{ role: 'left', waveData: left }]);
    });

    it('returns each physical Wave Data object once for keyed collection views', () => {
        const shared = waveData('SMPL-SHARED');
        const result = distinctWaveDataForSample(
            'SBNK-1',
            [
                relationship('left', 'SBNK_LEFT_MEMBER_TO_SMPL', shared.objectKey),
                relationship('right', 'SBNK_RIGHT_MEMBER_TO_SMPL', shared.objectKey),
            ],
            [shared],
        );

        expect(result).toEqual([shared]);
    });
});

describe('orderedSamplesForBank', () => {
    it('uses assignment order, preserves unindexed response order, and removes duplicate members', () => {
        const makeMember = (id: string, targetObjectId: string, assignmentIndex?: number): SamplerRelationship => ({
            ...relationship(id, 'SBAC_SLOT_TO_SBNK', targetObjectId),
            sourceObjectId: 'SBAC-1',
            assignmentIndex,
        });
        const first = sample('SBNK-1');
        const second = sample('SBNK-2');
        const unindexed = sample('SBNK-3');

        expect(
            orderedSamplesForBank(
                'SBAC-1',
                [
                    makeMember('unindexed', unindexed.objectId),
                    makeMember('second', second.objectId, 2),
                    makeMember('first', first.objectId, 1),
                    makeMember('duplicate', first.objectId, 3),
                    makeMember('missing', 'SBNK-MISSING', 4),
                ],
                [first, second, unindexed],
            ),
        ).toEqual([first, second, unindexed]);
    });
});
