import type { ObjectDetail } from '../../../../lib/transport';
import { sampleSnapshot } from '../../../../lib/objectEditing';

export function mappingMembers(detail: ObjectDetail): { id: string; name: string }[] {
    const confirmed = new Set(
        (detail.relationships ?? [])
            .filter(
                (row) =>
                    row.type === 'SBAC_SLOT_TO_SBNK' &&
                    row.quality === 'KNOWN' &&
                    row.selectedObjectRoles.includes('SOURCE') &&
                    row.sourceObject.id === detail.object.id &&
                    row.targetObject?.type === 'SBNK',
            )
            .map((row) => row.targetObject!.id),
    );
    const seen = new Set<string>();
    return (sampleSnapshot(detail)?.bankOverrides?.members ?? []).flatMap((member) => {
        if (!member.objectId || !confirmed.has(member.objectId) || seen.has(member.objectId)) return [];
        seen.add(member.objectId);
        return [{ id: member.objectId, name: member.name }];
    });
}
