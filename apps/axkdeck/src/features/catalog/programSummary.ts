import type { SamplerObject, SamplerRelationship } from '../../lib/transport';
import { isConfirmedRelationship, isEffectiveProgramAssignment } from '../../lib/relationshipResolution';

export function programSummaries(objects: SamplerObject[], relationships: SamplerRelationship[]): Map<string, string> {
    const types = new Map(objects.map((object) => [object.key, object.objectType]));
    const assignments = new Map<string, SamplerRelationship[]>();
    const members = new Map<string, SamplerRelationship[]>();
    for (const relationship of relationships) {
        const group = relationship.relationshipType.startsWith('PROG_ASSIGNMENT_')
            ? assignments
            : relationship.relationshipType === 'SBAC_SLOT_TO_SBNK'
              ? members
              : null;
        if (!group) continue;
        const entries = group.get(relationship.sourceObjectId) ?? [];
        entries.push(relationship);
        group.set(relationship.sourceObjectId, entries);
    }
    const count = (value: number, noun: string) => `${value} ${noun}${value === 1 ? '' : 's'}`;
    return new Map(
        objects
            .filter((object) => object.objectType === 'PROG')
            .map((program) => {
                const banks = new Set<string>();
                const direct = new Set<string>();
                const samples = new Set<string>();
                let active = 0,
                    unresolved = 0,
                    unresolvedMembers = 0;
                for (const row of assignments.get(program.key) ?? []) {
                    const type = types.get(row.targetObjectId ?? '');
                    if (isEffectiveProgramAssignment(row) && (type === 'SBAC' || type === 'SBNK')) {
                        active++;
                        (type === 'SBAC' ? banks : direct).add(row.targetObjectId!);
                    } else {
                        unresolved++;
                    }
                }
                for (const bank of banks)
                    for (const row of members.get(bank) ?? []) {
                        if (isConfirmedRelationship(row) && types.get(row.targetObjectId ?? '') === 'SBNK')
                            samples.add(row.targetObjectId!);
                        else unresolvedMembers++;
                    }
                return [
                    program.key,
                    `${count(active, 'assignment')} · ${count(banks.size, 'bank')} (${count(samples.size, 'sample')}) · ${count(direct.size, 'direct sample')}${unresolved ? ` · ${count(unresolved, 'unresolved assignment')}` : ''}${unresolvedMembers ? ` · ${count(unresolvedMembers, 'unresolved bank member')}` : ''}`,
                ];
            }),
    );
}
