import { describe, expect, it } from 'vitest';
import type { SamplerRelationship } from './transport';
import { inspectorRelationshipGroups, type InspectorRelationshipObject } from './inspectorRelationships';

function relationship(overrides: Partial<SamplerRelationship>): SamplerRelationship {
    return {
        id: 'relationship-1',
        sourceObjectId: 'program-1',
        targetObjectId: 'bank-1',
        candidateObjectIds: [],
        relationshipType: 'PROG_ASSIGNMENT_TO_SBAC',
        quality: 'KNOWN',
        basis: 'test',
        notes: [],
        assignmentName: '',
        assignmentState: '',
        receiveChannelDisplay: '=Smp',
        ...overrides,
    };
}

const objects: InspectorRelationshipObject[] = [
    { objectId: 'program-1', objectType: 'PROG', name: '001: Piano', sortIndex: 1 },
    { objectId: 'program-2', objectType: 'PROG', name: '002: Strings', sortIndex: 2 },
    { objectId: 'bank-1', objectType: 'SBAC', name: 'Piano Bank' },
    { objectId: 'sample-1', objectType: 'SBNK', name: 'Piano C3' },
    { objectId: 'wave-1', objectType: 'SMPL', name: 'Piano C3 L' },
];

describe('inspectorRelationshipGroups', () => {
    it('groups direct outgoing and incoming relationships by canonical object type', () => {
        const groups = inspectorRelationshipGroups(
            'sample-1',
            [
                relationship({
                    id: 'bank-member',
                    sourceObjectId: 'bank-1',
                    targetObjectId: 'sample-1',
                    relationshipType: 'SBAC_SLOT_TO_SBNK',
                    receiveChannelDisplay: '',
                }),
                relationship({
                    id: 'sample-wave',
                    sourceObjectId: 'sample-1',
                    targetObjectId: 'wave-1',
                    relationshipType: 'SBNK_LEFT_MEMBER_TO_SMPL',
                    receiveChannelDisplay: '',
                }),
            ],
            objects,
        );

        expect(groups).toEqual([
            {
                objectType: 'SBAC',
                label: 'Sample Banks',
                items: [
                    {
                        id: 'bank-member:bank-1',
                        objectId: 'bank-1',
                        name: 'Piano Bank',
                        detail: 'Member of',
                        navigable: true,
                    },
                ],
            },
            {
                objectType: 'SMPL',
                label: 'Wave Data',
                items: [
                    {
                        id: 'sample-wave:wave-1',
                        objectId: 'wave-1',
                        name: 'Piano C3 L',
                        detail: 'Left Wave Data',
                        navigable: true,
                    },
                ],
            },
        ]);
    });

    it('keeps uncertain candidates visible and non-navigable without diagnostic terminology', () => {
        const groups = inspectorRelationshipGroups(
            'program-1',
            [
                relationship({
                    targetObjectId: undefined,
                    candidateObjectIds: ['bank-1'],
                    quality: 'TENTATIVE',
                    assignmentName: 'Piano Bank',
                }),
            ],
            objects,
        );

        expect(groups[0]?.items).toEqual([
            {
                id: 'relationship-1:bank-1',
                objectId: undefined,
                name: 'Piano Bank',
                detail: 'Assignment · =Smp',
                navigable: false,
            },
        ]);
    });

    it('categorizes generic Program assignments by the resolved target object type', () => {
        const groups = inspectorRelationshipGroups(
            'program-1',
            [
                relationship({
                    id: 'generic-assignment',
                    targetObjectId: 'sample-1',
                    relationshipType: 'PROG_ASSIGNMENT_TO_OBJECT',
                    receiveChannelDisplay: '',
                }),
            ],
            objects,
        );

        expect(groups).toEqual([
            {
                objectType: 'SBNK',
                label: 'Samples',
                items: [
                    {
                        id: 'generic-assignment:sample-1',
                        objectId: 'sample-1',
                        name: 'Piano C3',
                        detail: 'Assignment',
                        navigable: true,
                    },
                ],
            },
        ]);
    });

    it('aggregates repeated assignments to one concrete object', () => {
        const groups = inspectorRelationshipGroups(
            'program-1',
            [
                relationship({ id: 'assignment-1', receiveChannelDisplay: 'A01' }),
                relationship({ id: 'assignment-2', receiveChannelDisplay: '=Smp' }),
            ],
            objects,
        );

        expect(groups[0]?.items).toHaveLength(1);
        expect(groups[0]?.items[0]).toMatchObject({
            objectId: 'bank-1',
            detail: 'Assignment · A01 / Assignment · =Smp',
            navigable: true,
        });
    });
});
