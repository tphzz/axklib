import type { SamplerRelationship } from './transport';
import type { LinkedWaveDataItem, WaveDataItem } from './types';

const memberRelationships = [
    { relationshipType: 'SBNK_LEFT_MEMBER_TO_SMPL', role: 'left' },
    { relationshipType: 'SBNK_RIGHT_MEMBER_TO_SMPL', role: 'right' },
] as const;

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
