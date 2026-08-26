import { compareNaturalNames } from './naturalSort';
import type { SamplerRelationship } from './transport';
import type { InspectorRelatedObjectType, InspectorRelationshipGroup, InspectorRelationshipItem } from './types';

export interface InspectorRelationshipObject {
    objectId: string;
    objectType: InspectorRelatedObjectType;
    name: string;
    sortIndex?: number;
}

interface RelationshipKind {
    sourceType: InspectorRelatedObjectType;
    targetType: InspectorRelatedObjectType;
    outgoingDetail: string;
    incomingDetail: string;
}

const groupOrder: readonly {
    objectType: InspectorRelatedObjectType;
    label: InspectorRelationshipGroup['label'];
}[] = [
    { objectType: 'PROG', label: 'Programs' },
    { objectType: 'SBAC', label: 'Sample Banks' },
    { objectType: 'SBNK', label: 'Samples' },
    { objectType: 'SMPL', label: 'Wave Data' },
    { objectType: 'SEQU', label: 'Sequences' },
];

const relationshipKinds = new Map<string, RelationshipKind>([
    [
        'PROG_ASSIGNMENT_TO_SBAC',
        {
            sourceType: 'PROG',
            targetType: 'SBAC',
            outgoingDetail: 'Assignment',
            incomingDetail: 'Assigned by',
        },
    ],
    [
        'PROG_ASSIGNMENT_TO_SBNK',
        {
            sourceType: 'PROG',
            targetType: 'SBNK',
            outgoingDetail: 'Assignment',
            incomingDetail: 'Assigned by',
        },
    ],
    [
        'SBAC_SLOT_TO_SBNK',
        {
            sourceType: 'SBAC',
            targetType: 'SBNK',
            outgoingDetail: 'Member',
            incomingDetail: 'Member of',
        },
    ],
    [
        'SBNK_LEFT_MEMBER_TO_SMPL',
        {
            sourceType: 'SBNK',
            targetType: 'SMPL',
            outgoingDetail: 'Left Wave Data',
            incomingDetail: 'Used as left member',
        },
    ],
    [
        'SBNK_RIGHT_MEMBER_TO_SMPL',
        {
            sourceType: 'SBNK',
            targetType: 'SMPL',
            outgoingDetail: 'Right Wave Data',
            incomingDetail: 'Used as right member',
        },
    ],
]);

function genericProgramAssignmentKind(target: InspectorRelationshipObject): RelationshipKind {
    return {
        sourceType: 'PROG',
        targetType: target.objectType,
        outgoingDetail: 'Assignment',
        incomingDetail: 'Assigned by',
    };
}

function assignmentDetail(base: string, relationship: SamplerRelationship): string {
    return relationship.receiveChannelDisplay ? `${base} · ${relationship.receiveChannelDisplay}` : base;
}

function unresolvedName(objectType: InspectorRelatedObjectType, relationship: SamplerRelationship): string {
    if (relationship.assignmentName) return relationship.assignmentName;
    const label = groupOrder.find((group) => group.objectType === objectType)?.label ?? 'Object';
    return `Unresolved ${label.replace(/s$/, '')}`;
}

function appendItem(
    grouped: Map<InspectorRelatedObjectType, Map<string, InspectorRelationshipItem>>,
    objectType: InspectorRelatedObjectType,
    item: InspectorRelationshipItem,
): void {
    const group = grouped.get(objectType) ?? new Map<string, InspectorRelationshipItem>();
    const key = item.objectId ?? item.id;
    const existing = group.get(key);
    if (!existing) {
        group.set(key, item);
        grouped.set(objectType, group);
        return;
    }
    const details = [...new Set([...existing.detail.split(' / '), ...item.detail.split(' / ')])];
    group.set(key, {
        ...existing,
        objectId: existing.objectId ?? item.objectId,
        detail: details.join(' / '),
        navigable: existing.navigable || item.navigable,
    });
}

function relatedItem(
    relationship: SamplerRelationship,
    object: InspectorRelationshipObject,
    detail: string,
): InspectorRelationshipItem {
    const navigable = relationship.quality === 'KNOWN';
    return {
        id: `${relationship.id}:${object.objectId}`,
        objectId: navigable ? object.objectId : undefined,
        name: object.name,
        detail: assignmentDetail(detail, relationship),
        navigable,
    };
}

export function inspectorRelationshipGroups(
    selectedObjectId: string,
    relationships: readonly SamplerRelationship[],
    objects: readonly InspectorRelationshipObject[],
): InspectorRelationshipGroup[] {
    const objectById = new Map(objects.map((object) => [object.objectId, object]));
    const grouped = new Map<InspectorRelatedObjectType, Map<string, InspectorRelationshipItem>>();

    for (const relationship of relationships) {
        const fixedKind = relationshipKinds.get(relationship.relationshipType);
        const genericProgramAssignment = relationship.relationshipType === 'PROG_ASSIGNMENT_TO_OBJECT';
        if (!fixedKind && !genericProgramAssignment) continue;
        if (relationship.sourceObjectId === selectedObjectId) {
            const targetIds = relationship.targetObjectId
                ? [relationship.targetObjectId]
                : relationship.candidateObjectIds;
            if (targetIds.length === 0 && fixedKind) {
                appendItem(grouped, fixedKind.targetType, {
                    id: relationship.id,
                    name: unresolvedName(fixedKind.targetType, relationship),
                    detail: assignmentDetail(fixedKind.outgoingDetail, relationship),
                    navigable: false,
                });
            } else {
                for (const targetId of targetIds) {
                    const target = objectById.get(targetId);
                    const kind = fixedKind ?? (target ? genericProgramAssignmentKind(target) : null);
                    if (!kind) continue;
                    appendItem(
                        grouped,
                        kind.targetType,
                        target
                            ? relatedItem(relationship, target, kind.outgoingDetail)
                            : {
                                  id: `${relationship.id}:${targetId}`,
                                  name: unresolvedName(kind.targetType, relationship),
                                  detail: assignmentDetail(kind.outgoingDetail, relationship),
                                  navigable: false,
                              },
                    );
                }
            }
        }

        const targetIds = new Set(
            relationship.targetObjectId
                ? [relationship.targetObjectId, ...relationship.candidateObjectIds]
                : relationship.candidateObjectIds,
        );
        if (targetIds.has(selectedObjectId)) {
            const source = objectById.get(relationship.sourceObjectId);
            const selected = objectById.get(selectedObjectId);
            const kind = fixedKind ?? (selected ? genericProgramAssignmentKind(selected) : null);
            if (source && kind) {
                appendItem(grouped, kind.sourceType, relatedItem(relationship, source, kind.incomingDetail));
            }
        }
    }

    return groupOrder.flatMap(({ objectType, label }) => {
        const items = [...(grouped.get(objectType)?.values() ?? [])].toSorted((left, right) => {
            const leftIndex = left.objectId ? objectById.get(left.objectId)?.sortIndex : undefined;
            const rightIndex = right.objectId ? objectById.get(right.objectId)?.sortIndex : undefined;
            if (leftIndex !== undefined || rightIndex !== undefined) {
                return (leftIndex ?? Number.MAX_SAFE_INTEGER) - (rightIndex ?? Number.MAX_SAFE_INTEGER);
            }
            return compareNaturalNames(left.name, right.name);
        });
        return items.length > 0 ? [{ objectType, label, items }] : [];
    });
}
