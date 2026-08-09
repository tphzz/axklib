import { describe, expect, it } from 'vitest';
import type { SamplerObject, SamplerRelationship } from './transport';
import type { SampleStructureItem, WaveDataItem } from './types';
import {
    auditionableSampleBankIds,
    auditionableSampleIds,
    distinctWaveDataForSample,
    linkedWaveDataForSample,
    orderedSamplesForBank,
} from './sampleRelationships';

function relationship(id: string, type: string, targetObjectId?: string): SamplerRelationship {
    return {
        id,
        sourceObjectId: 'SBNK-1',
        targetObjectId,
        candidateObjectIds: [],
        relationshipType: type,
        quality: 'KNOWN',
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

    it('does not resolve Wave Data through unconfirmed relationships', () => {
        const candidate = waveData('SMPL-CANDIDATE');
        const likely = {
            ...relationship('likely', 'SBNK_LEFT_MEMBER_TO_SMPL', candidate.objectKey),
            quality: 'LIKELY' as const,
        };

        expect(linkedWaveDataForSample('SBNK-1', [likely], [candidate])).toEqual([]);
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

describe('auditionableSampleIds', () => {
    it('requires one or two distinct Known Wave Data targets', () => {
        const left = waveData('SMPL-L');
        const right = waveData('SMPL-R');
        const third = waveData('SMPL-THIRD');
        const knownMono = relationship('known-mono', 'SBNK_LEFT_MEMBER_TO_SMPL', left.objectKey);
        const likely = {
            ...relationship('likely', 'SBNK_LEFT_MEMBER_TO_SMPL', left.objectKey),
            sourceObjectId: 'SBNK-LIKELY',
            quality: 'LIKELY' as const,
        };
        const knownStereo = [
            {
                ...relationship('known-left', 'SBNK_LEFT_MEMBER_TO_SMPL', left.objectKey),
                sourceObjectId: 'SBNK-STEREO',
            },
            {
                ...relationship('known-right', 'SBNK_RIGHT_MEMBER_TO_SMPL', right.objectKey),
                sourceObjectId: 'SBNK-STEREO',
            },
        ];
        const tooMany = [left, right, third].map((item, index) => ({
            ...relationship(`known-${index}`, 'SBNK_LEFT_MEMBER_TO_SMPL', item.objectKey),
            sourceObjectId: 'SBNK-TOO-MANY',
        }));

        expect(auditionableSampleIds([knownMono, likely, ...knownStereo, ...tooMany], [left, right, third])).toEqual(
            new Set(['SBNK-1', 'SBNK-STEREO']),
        );
    });

    it('ignores Known targets that are absent from the loaded Wave Data collection', () => {
        expect(
            auditionableSampleIds([relationship('missing', 'SBNK_LEFT_MEMBER_TO_SMPL', 'SMPL-MISSING')], []),
        ).toEqual(new Set());
    });
});

describe('auditionableSampleBankIds', () => {
    it('indexes loaded Sample Banks with confirmed playable members', () => {
        const firstBank = { ...sample('SBAC-1'), objectType: 'SBAC' as const };
        const secondBank = { ...sample('SBAC-2'), objectType: 'SBAC' as const };
        const playableMember = sample('SBNK-PLAYABLE');
        const unavailableMember = sample('SBNK-UNAVAILABLE');
        const member = (
            id: string,
            bankId: string,
            targetObjectId: string,
            quality: SamplerRelationship['quality'] = 'KNOWN',
        ): SamplerRelationship => ({
            ...relationship(id, 'SBAC_SLOT_TO_SBNK', targetObjectId),
            sourceObjectId: bankId,
            quality,
        });

        expect(
            auditionableSampleBankIds(
                [
                    member('playable', firstBank.objectId, playableMember.objectId),
                    member('unavailable', secondBank.objectId, unavailableMember.objectId),
                    member('unconfirmed', secondBank.objectId, playableMember.objectId, 'LIKELY'),
                    member('unloaded', 'SBAC-UNLOADED', playableMember.objectId),
                    member('missing-sample', secondBank.objectId, 'SBNK-MISSING'),
                ],
                [firstBank, secondBank],
                [playableMember, unavailableMember],
                new Set([playableMember.objectId, 'SBNK-MISSING']),
            ),
        ).toEqual(new Set([firstBank.objectId]));
    });
});

describe('orderedSamplesForBank', () => {
    it('uses natural Sample names for slice playback and removes duplicate members', () => {
        const makeMember = (id: string, targetObjectId: string, assignmentIndex?: number): SamplerRelationship => ({
            ...relationship(id, 'SBAC_SLOT_TO_SBNK', targetObjectId),
            sourceObjectId: 'SBAC-1',
            assignmentIndex,
        });
        const slice00 = { ...sample('SBNK-00'), name: 'LoopDiv00' };
        const slice01 = { ...sample('SBNK-01'), name: 'LoopDiv01' };
        const slice02 = { ...sample('SBNK-02'), name: 'LoopDiv02' };
        const slice10 = { ...sample('SBNK-10'), name: 'LoopDiv10' };
        const slice13 = { ...sample('SBNK-13'), name: 'LoopDiv13' };

        expect(
            orderedSamplesForBank(
                'SBAC-1',
                [
                    makeMember('slice-13', slice13.objectId, 1),
                    makeMember('slice-02', slice02.objectId, 2),
                    makeMember('slice-10', slice10.objectId, 3),
                    makeMember('slice-00', slice00.objectId, 4),
                    makeMember('slice-01', slice01.objectId),
                    makeMember('duplicate', slice00.objectId, 5),
                    makeMember('missing', 'SBNK-MISSING', 4),
                ],
                [slice13, slice02, slice10, slice00, slice01],
            ),
        ).toEqual([slice00, slice01, slice02, slice10, slice13]);
    });

    it('retains assignment order when distinct Samples have the same visible name', () => {
        const first = { ...sample('SBNK-1'), name: 'Same Name' };
        const second = { ...sample('SBNK-2'), name: 'Same Name' };
        const makeMember = (id: string, targetObjectId: string, assignmentIndex: number): SamplerRelationship => ({
            ...relationship(id, 'SBAC_SLOT_TO_SBNK', targetObjectId),
            sourceObjectId: 'SBAC-1',
            assignmentIndex,
        });

        expect(
            orderedSamplesForBank(
                'SBAC-1',
                [makeMember('second', second.objectId, 2), makeMember('first', first.objectId, 1)],
                [second, first],
            ),
        ).toEqual([first, second]);
    });

    it('does not order unconfirmed Sample Bank members for playback', () => {
        const member = sample('SBNK-1');
        const likely: SamplerRelationship = {
            ...relationship('likely', 'SBAC_SLOT_TO_SBNK', member.objectId),
            sourceObjectId: 'SBAC-1',
            quality: 'LIKELY',
        };

        expect(orderedSamplesForBank('SBAC-1', [likely], [member])).toEqual([]);
    });
});
