import type { SamplerRelationship } from './transport';
import type { LinkedWaveDataItem, SampleStructureItem, WaveDataItem } from './types';
import { compareNaturalNames } from './naturalSort';
import { isConfirmedRelationship } from './relationshipResolution';

const memberRelationships = [
    { relationshipType: 'SBNK_LEFT_MEMBER_TO_SMPL', role: 'left' },
    { relationshipType: 'SBNK_RIGHT_MEMBER_TO_SMPL', role: 'right' },
] as const;

export function isStandaloneSample(sample: SampleStructureItem): boolean {
    return (sample.sampleBankObjectIds?.length ?? 0) === 0;
}

export function auditionableSampleIds(
    relationships: readonly SamplerRelationship[],
    waveData: readonly WaveDataItem[],
): Set<string> {
    const waveDataIds = new Set(waveData.map((item) => item.objectKey));
    const confirmedTargets = new Map<string, Set<string>>();
    for (const relationship of relationships) {
        if (
            !isConfirmedRelationship(relationship) ||
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

export function stereoSampleIds(
    relationships: readonly SamplerRelationship[],
    waveData: readonly WaveDataItem[],
): Set<string> {
    const waveDataIds = new Set(waveData.map((item) => item.objectKey));
    const membersBySample = new Map<string, { left: Set<string>; right: Set<string> }>();
    for (const relationship of relationships) {
        const member = memberRelationships.find(
            (candidate) => candidate.relationshipType === relationship.relationshipType,
        );
        if (
            !member ||
            !isConfirmedRelationship(relationship) ||
            !relationship.targetObjectId ||
            !waveDataIds.has(relationship.targetObjectId)
        ) {
            continue;
        }
        const members = membersBySample.get(relationship.sourceObjectId) ?? {
            left: new Set<string>(),
            right: new Set<string>(),
        };
        members[member.role].add(relationship.targetObjectId);
        membersBySample.set(relationship.sourceObjectId, members);
    }
    return new Set(
        [...membersBySample]
            .filter(([, members]) => {
                if (members.left.size !== 1 || members.right.size !== 1) return false;
                return new Set([...members.left, ...members.right]).size === 2;
            })
            .map(([sampleId]) => sampleId),
    );
}

export function auditionableSampleBankIds(
    relationships: readonly SamplerRelationship[],
    sampleBanks: readonly SampleStructureItem[],
    samples: readonly SampleStructureItem[],
    auditionableSamples: ReadonlySet<string>,
): Set<string> {
    const loadedBankIds = new Set(sampleBanks.map((bank) => bank.objectId));
    const loadedSampleIds = new Set(samples.map((sample) => sample.objectId));
    const result = new Set<string>();
    for (const relationship of relationships) {
        if (
            relationship.relationshipType === 'SBAC_SLOT_TO_SBNK' &&
            isConfirmedRelationship(relationship) &&
            loadedBankIds.has(relationship.sourceObjectId) &&
            relationship.targetObjectId &&
            loadedSampleIds.has(relationship.targetObjectId) &&
            auditionableSamples.has(relationship.targetObjectId)
        ) {
            result.add(relationship.sourceObjectId);
        }
    }
    return result;
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
                !isConfirmedRelationship(relationship) ||
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
                isConfirmedRelationship(relationship) &&
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
        .map((objectId, relationshipOrder) => ({ item: samplesById.get(objectId), relationshipOrder }))
        .filter(
            (member): member is { item: SampleStructureItem; relationshipOrder: number } => member.item !== undefined,
        )
        .toSorted(
            (left, right) =>
                compareNaturalNames(left.item.name, right.item.name) ||
                left.relationshipOrder - right.relationshipOrder ||
                (left.item.objectId < right.item.objectId ? -1 : left.item.objectId > right.item.objectId ? 1 : 0),
        )
        .map((member) => member.item);
}
