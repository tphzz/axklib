import type { SamplerRelationship } from './transport';
import type { LinkedWaveDataItem, SampleStructureItem, WaveDataItem } from './types';

const memberRelationships = [
    { relationshipType: 'SBNK_LEFT_MEMBER_TO_SMPL', role: 'left' },
    { relationshipType: 'SBNK_RIGHT_MEMBER_TO_SMPL', role: 'right' },
] as const;

export function auditionableSampleIds(
    relationships: readonly SamplerRelationship[],
    waveData: readonly WaveDataItem[],
): Set<string> {
    const waveDataIds = new Set(waveData.map((item) => item.objectKey));
    const confirmedTargets = new Map<string, Set<string>>();
    for (const relationship of relationships) {
        if (
            relationship.quality !== 'Known' ||
            !relationship.targetObjectId ||
            !waveDataIds.has(relationship.targetObjectId) ||
            !memberRelationships.some((member) => member.relationshipType === relationship.relationshipType)
        ) {
            continue;
        }
        const targets = confirmedTargets.get(relationship.sourceObjectId) ?? new Set<string>();
        targets.add(relationship.targetObjectId);
        confirmedTargets.set(relationship.sourceObjectId, targets);
    }
    return new Set(
        [...confirmedTargets]
            .filter(([, targets]) => targets.size >= 1 && targets.size <= 2)
            .map(([sampleId]) => sampleId),
    );
}

export function linkedWaveDataForSample(
    sampleId: string,
    relationships: readonly SamplerRelationship[],
    waveData: readonly WaveDataItem[],
): LinkedWaveDataItem[] {
    const waveDataById = new Map(waveData.map((item) => [item.objectKey, item]));
    const result: LinkedWaveDataItem[] = [];
    const seen = new Set<string>();
    for (const memberRelationship of memberRelationships) {
        for (const relationship of relationships) {
            if (
                relationship.sourceObjectId !== sampleId ||
                relationship.relationshipType !== memberRelationship.relationshipType ||
                !relationship.targetObjectId
            ) {
                continue;
            }
            const item = waveDataById.get(relationship.targetObjectId);
            if (!item) continue;
            const memberKey = `${memberRelationship.role}\0${item.objectKey}`;
            if (seen.has(memberKey)) continue;
            seen.add(memberKey);
            result.push({ role: memberRelationship.role, waveData: item });
        }
    }
    return result;
}

export function distinctWaveDataForSample(
    sampleId: string,
    relationships: readonly SamplerRelationship[],
    waveData: readonly WaveDataItem[],
): WaveDataItem[] {
    const seen = new Set<string>();
    return linkedWaveDataForSample(sampleId, relationships, waveData)
        .map((member) => member.waveData)
        .filter((item) => {
            if (seen.has(item.objectKey)) return false;
            seen.add(item.objectKey);
            return true;
        });
}

export function orderedSamplesForBank(
    bankId: string,
    relationships: readonly SamplerRelationship[],
    samples: readonly SampleStructureItem[],
): SampleStructureItem[] {
    const samplesById = new Map(samples.map((item) => [item.objectId, item]));
    const seen = new Set<string>();
    return relationships
        .map((relationship, responseIndex) => ({ relationship, responseIndex }))
        .filter(
            ({ relationship }) =>
                relationship.sourceObjectId === bankId &&
                relationship.relationshipType === 'SBAC_SLOT_TO_SBNK' &&
                Boolean(relationship.targetObjectId),
        )
        .toSorted(
            (left, right) =>
                (left.relationship.assignmentIndex ?? Number.MAX_SAFE_INTEGER) -
                    (right.relationship.assignmentIndex ?? Number.MAX_SAFE_INTEGER) ||
                left.responseIndex - right.responseIndex,
        )
        .map(({ relationship }) => relationship.targetObjectId!)
        .filter((objectId) => {
            if (seen.has(objectId)) return false;
            seen.add(objectId);
            return true;
        })
        .map((objectId) => samplesById.get(objectId))
        .filter((item) => item !== undefined);
}
