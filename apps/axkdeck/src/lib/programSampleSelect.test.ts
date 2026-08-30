import { describe, expect, it } from 'vitest';
import type { SamplerObject, SamplerRelationship } from './transport';
import type { ProgramAssignmentRow, SampleStructureItem } from './types';
import { programSampleSelectRows } from './programSampleSelect';

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
        sizeWithDependenciesBytes: null,
        sampleRate: 44_100,
        rootKey: 60,
        frameCount: 44_100,
        sampleWidthBytes: 2,
    };
}

function item(objectType: 'SBAC' | 'SBNK', name: string): SampleStructureItem {
    const samplerObject = object(objectType, name);
    return {
        id: samplerObject.key,
        objectId: samplerObject.key,
        objectType,
        object: samplerObject,
        name,
    };
}

function assignment(
    target: SampleStructureItem | null,
    assignmentState: string,
    receiveChannelDisplay: string,
    assignmentIndex: number,
    overrides: Partial<SamplerRelationship> = {},
): ProgramAssignmentRow {
    const relationship: SamplerRelationship = {
        id: `assignment-${assignmentIndex}-${target?.objectId ?? 'unresolved'}`,
        sourceObjectId: 'PROG-001',
        targetObjectId: target?.objectId,
        candidateObjectIds: [],
        relationshipType: `PROG_ASSIGNMENT_TO_${target?.objectType ?? 'SBNK'}`,
        quality: target ? 'KNOWN' : 'UNKNOWN',
        basis: 'test',
        notes: [],
        assignmentIndex,
        assignmentName: target?.name ?? 'Missing Sample',
        assignmentState,
        receiveChannelDisplay,
        ...overrides,
    };
    return {
        relationship,
        targetObjectId: relationship.targetObjectId,
        targetType: target?.objectType ?? 'SBNK',
        targetName: target?.name ?? relationship.assignmentName,
        confirmed: relationship.quality === 'KNOWN',
    };
}

describe('programSampleSelectRows', () => {
    it('orders assigned targets by receive selector before type and name', () => {
        const bassBank = item('SBAC', 'Bass');
        const stringBank = item('SBAC', 'Strings');
        const cello = item('SBNK', 'Cello');
        const violin = item('SBNK', 'Violin');
        const unresolvedSample = item('SBNK', 'Flute');

        const rows = programSampleSelectRows(
            [
                assignment(unresolvedSample, 'unknown', 'unknown', 0),
                assignment(violin, 'stored-assignment', 'A03', 1),
                assignment(stringBank, 'source-load-assignment', '=Smp', 2),
                assignment(cello, 'stored-assignment', 'A02', 3),
                assignment(bassBank, 'stored-assignment', 'A01', 4),
            ],
            [stringBank, bassBank],
            [violin, cello, unresolvedSample],
        );

        expect(rows.assigned.map((row) => row.targetName)).toEqual(['Bass', 'Cello', 'Violin', 'Strings']);
        expect(rows.assigned.map((row) => row.receiveChannelDisplays)).toEqual([['A01'], ['A02'], ['A03'], ['=Smp']]);
        expect(rows.assigned[3]?.sourceLoad).toBe(true);
    });

    it('orders every target by receive selector and places inactive objects last', () => {
        const bassBank = item('SBAC', 'Bass');
        const stringBank = item('SBAC', 'Strings');
        const cello = item('SBNK', 'Cello');
        const violin = item('SBNK', 'Violin');

        const rows = programSampleSelectRows(
            [assignment(violin, 'stored-assignment', '=Smp', 0)],
            [stringBank, bassBank],
            [violin, cello],
        );

        expect(rows.all.map((row) => row.targetName)).toEqual(['Violin', 'Bass', 'Strings', 'Cello']);
        expect(rows.all.map((row) => row.receiveChannelDisplays)).toEqual([['=Smp'], ['off'], ['off'], ['off']]);
        expect(rows.all.map((row) => row.assigned)).toEqual([true, false, false, false]);
    });

    it('matches the complete sampler receive selector order', () => {
        const targets = [
            item('SBNK', 'Target 1'),
            item('SBNK', 'Target 2'),
            item('SBNK', 'Target 3'),
            item('SBNK', 'Target 4'),
            item('SBNK', 'Target 5'),
            item('SBNK', 'Target 6'),
            item('SBNK', 'Alpha unknown'),
            item('SBNK', 'Zulu unknown'),
        ];
        const displays = ['B16', '=Smp', 'Bch', 'A16', 'B01', 'A01', 'A00', 'unexpected'];
        const rows = programSampleSelectRows(
            targets.map((target, index) => assignment(target, 'stored-assignment', displays[index]!, index)),
            [],
            targets,
        );

        expect(rows.assigned.map((row) => row.receiveChannelDisplays[0])).toEqual([
            'A01',
            'A16',
            'B01',
            'B16',
            'Bch',
            '=Smp',
            'A00',
            'unexpected',
        ]);
    });

    it('uses the earliest selector for a target and preserves current ordering for equal selectors', () => {
        const alphaBank = item('SBAC', 'Alpha');
        const zuluBank = item('SBAC', 'Zulu');
        const alphaSample = item('SBNK', 'Alpha');
        const layeredSample = item('SBNK', 'Layered');
        const rows = programSampleSelectRows(
            [
                assignment(alphaSample, 'stored-assignment', 'A03', 0),
                assignment(layeredSample, 'stored-assignment', '=Smp', 1),
                assignment(zuluBank, 'stored-assignment', 'A03', 2),
                assignment(layeredSample, 'stored-assignment', 'A02', 3),
                assignment(alphaBank, 'stored-assignment', 'A03', 4),
            ],
            [zuluBank, alphaBank],
            [layeredSample, alphaSample],
        );

        expect(rows.assigned.map((row) => row.targetName)).toEqual(['Layered', 'Alpha', 'Zulu', 'Alpha']);
        expect(rows.assigned.map((row) => row.targetType)).toEqual(['SBNK', 'SBAC', 'SBAC', 'SBNK']);
        expect(rows.assigned[0]?.receiveChannelDisplays).toEqual(['=Smp', 'A02']);
    });

    it('aggregates distinct active selectors for one object into one row', () => {
        const sample = item('SBNK', 'Layer');
        const rows = programSampleSelectRows(
            [
                assignment(sample, 'stored-assignment', 'A01', 0),
                assignment(sample, 'stored-assignment', 'A02', 1),
                assignment(sample, 'stored-assignment', 'A01', 2),
            ],
            [],
            [sample],
        );

        expect(rows.assigned).toHaveLength(1);
        expect(rows.assigned[0]?.receiveChannelDisplays).toEqual(['A01', 'A02']);
        expect(rows.assigned[0]?.relationships).toHaveLength(3);
    });

    it('does not merge an unresolved selector into the only resolved target', () => {
        const astro = item('SBNK', 'Astro');
        const rows = programSampleSelectRows(
            [
                assignment(astro, 'stored-assignment', '=Smp', 0),
                assignment(null, 'stored-assignment', 'A01', 1, { assignmentName: 'ASR10 MergeX   *' }),
            ],
            [],
            [astro],
        );

        expect(rows.assigned).toHaveLength(1);
        expect(rows.assigned[0]).toMatchObject({
            targetName: 'Astro',
            targetObjectId: astro.objectId,
            navigable: true,
            receiveChannelDisplays: ['=Smp'],
        });
        expect(rows.all.map((row) => row.targetName)).toEqual(['Astro']);
    });

    it('does not present a stored assignment without an exact target as sampler content', () => {
        const unresolved = assignment(null, 'stored-assignment', 'A07', 0);
        const rows = programSampleSelectRows([unresolved], [], []);

        expect(rows.assigned).toEqual([]);
        expect(rows.all).toEqual([]);
    });

    it('keeps an exact real target whose stored name ends in star', () => {
        const starred = item('SBNK', 'STAR SAMPLE    *');
        const rows = programSampleSelectRows([assignment(starred, 'stored-assignment', 'A02', 0)], [], [starred]);

        expect(rows.assigned).toHaveLength(1);
        expect(rows.assigned[0]).toMatchObject({
            targetName: 'STAR SAMPLE    *',
            targetObjectId: starred.objectId,
            navigable: true,
            receiveChannelDisplays: ['A02'],
        });
    });

    it('keeps a confirmed target navigable when it is outside the visible inventory subset', () => {
        const sample = item('SBNK', 'Electro FX');
        const rows = programSampleSelectRows([assignment(sample, 'source-load-assignment', 'A02', 0)], [], []);

        expect(rows.assigned).toHaveLength(1);
        expect(rows.assigned[0]).toMatchObject({
            targetName: 'Electro FX',
            targetObjectId: sample.objectId,
            navigable: true,
            receiveChannelDisplays: ['A02'],
        });
    });
});
