import type { SamplerRelationship } from './transport';

export function isConfirmedRelationship(relationship: Pick<SamplerRelationship, 'quality'>): boolean {
    return relationship.quality === 'KNOWN';
}

const effectiveProgramAssignmentStates = new Set(['stored-assignment', 'source-load-assignment']);

export function isEffectiveProgramAssignment(
    relationship: Pick<SamplerRelationship, 'assignmentState' | 'quality' | 'relationshipType' | 'targetObjectId'>,
): boolean {
    return (
        (relationship.relationshipType === 'PROG_ASSIGNMENT_TO_SBAC' ||
            relationship.relationshipType === 'PROG_ASSIGNMENT_TO_SBNK') &&
        effectiveProgramAssignmentStates.has(relationship.assignmentState) &&
        isConfirmedRelationship(relationship) &&
        Boolean(relationship.targetObjectId)
    );
}
