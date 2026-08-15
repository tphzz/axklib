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
        sfsId: 0,
        storedSizeBytes: 128,
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
        targetName: target?.name ?? 'Missing Sample',
        confirmed: relationship.quality === 'KNOWN',
    };
}

describe('programSampleSelectRows', () => {
    it('keeps only effective assignments in hardware order for the assigned view', () => {
        const bank = item('SBAC', 'Strings');
        const activeSample = item('SBNK', 'Violin');
        const offSample = item('SBNK', 'Cello');

        const rows = programSampleSelectRows(
            [
                assignment(offSample, 'confirmed-visible-off', 'off', 0),
                assignment(activeSample, 'confirmed-active', '03', 1),
                assignment(bank, 'source-load-assignment', '=SMP', 2),
            ],
            [bank],
            [activeSample, offSample],
        );

        expect(rows.assigned.map((row) => row.targetName)).toEqual(['Violin', 'Strings']);
        expect(rows.assigned.map((row) => row.receiveChannelDisplays)).toEqual([['03'], ['=SMP']]);
        expect(rows.assigned[1]?.sourceLoad).toBe(true);
    });

    it('lists every bank before every sample alphabetically and marks inactive objects off', () => {
        const bassBank = item('SBAC', 'Bass');
        const stringBank = item('SBAC', 'Strings');
        const cello = item('SBNK', 'Cello');
        const violin = item('SBNK', 'Violin');

        const rows = programSampleSelectRows(
            [assignment(violin, 'confirmed-active', '=SMP', 0)],
            [stringBank, bassBank],
            [violin, cello],
        );

        expect(rows.all.map((row) => row.targetName)).toEqual(['Bass', 'Strings', 'Cello', 'Violin']);
        expect(rows.all.map((row) => row.receiveChannelDisplays)).toEqual([['off'], ['off'], ['off'], ['=SMP']]);
        expect(rows.all.map((row) => row.assigned)).toEqual([false, false, false, true]);
    });

    it('aggregates distinct active selectors for one object into one row', () => {
        const sample = item('SBNK', 'Layer');
        const rows = programSampleSelectRows(
            [
                assignment(sample, 'confirmed-active', '01', 0),
                assignment(sample, 'confirmed-active', '02', 1),
                assignment(sample, 'confirmed-active', '01', 2),
            ],
            [],
            [sample],
        );

        expect(rows.assigned).toHaveLength(1);
        expect(rows.assigned[0]?.receiveChannelDisplays).toEqual(['01', '02']);
        expect(rows.assigned[0]?.relationships).toHaveLength(3);
    });

    it('preserves unresolved active assignments without making up an inventory object', () => {
        const unresolved = assignment(null, 'confirmed-active', '07', 0);
        const rows = programSampleSelectRows([unresolved], [], []);

        expect(rows.assigned).toHaveLength(1);
        expect(rows.all).toHaveLength(1);
        expect(rows.all[0]).toMatchObject({
            targetName: 'Missing Sample',
            targetType: 'SBNK',
            assigned: true,
            targetObjectId: undefined,
            receiveChannelDisplays: ['07'],
        });
    });
});
