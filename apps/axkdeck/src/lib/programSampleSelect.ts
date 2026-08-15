import type {
    ProgramAssignmentRow,
    ProgramSampleSelectRow,
    ProgramSampleSelectRows,
    SampleStructureItem,
} from './types';

const activeAssignmentStates = new Set(['confirmed-active', 'source-load-assignment']);
const nameCollator = new Intl.Collator(undefined, { numeric: true, sensitivity: 'base' });

function isActive(row: ProgramAssignmentRow): boolean {
    return activeAssignmentStates.has(row.relationship.assignmentState);
}

function displayValues(rows: ProgramAssignmentRow[]): string[] {
    const displays = new Set<string>();
    for (const row of rows) {
        const display = row.relationship.receiveChannelDisplay.trim() || 'Unknown';
        displays.add(display);
    }
    return [...displays];
}

function inventoryRow(
    item: SampleStructureItem,
    assignments: ProgramAssignmentRow[],
    activeAssignments: ProgramAssignmentRow[],
): ProgramSampleSelectRow {
    return {
        id: item.objectId,
        targetType: item.objectType,
        targetName: item.name,
        targetObjectId: item.objectId,
        navigable: true,
        assigned: activeAssignments.length > 0,
        receiveChannelDisplays: activeAssignments.length > 0 ? displayValues(activeAssignments) : ['off'],
        sourceLoad: activeAssignments.some((row) => row.relationship.assignmentState === 'source-load-assignment'),
        relationships: assignments.map((row) => row.relationship),
    };
}

function unresolvedRow(row: ProgramAssignmentRow): ProgramSampleSelectRow {
    return {
        id: row.relationship.id,
        targetType: row.targetType,
        targetName: row.targetName,
        targetObjectId: row.targetObjectId,
        navigable: false,
        assigned: true,
        receiveChannelDisplays: displayValues([row]),
        sourceLoad: row.relationship.assignmentState === 'source-load-assignment',
        relationships: [row.relationship],
    };
}

function typeOrder(type: string): number {
    if (type === 'SBAC') return 0;
    if (type === 'SBNK') return 1;
    return 2;
}

export function programSampleSelectRows(
    assignments: ProgramAssignmentRow[],
    sampleBanks: SampleStructureItem[],
    samples: SampleStructureItem[],
): ProgramSampleSelectRows {
    const orderedAssignments = assignments.toSorted(
        (left, right) => (left.relationship.assignmentIndex ?? 0) - (right.relationship.assignmentIndex ?? 0),
    );
    const assignmentsByTarget = new Map<string, ProgramAssignmentRow[]>();
    const activeByTarget = new Map<string, ProgramAssignmentRow[]>();
    for (const row of orderedAssignments) {
        if (!row.targetObjectId) continue;
        const related = assignmentsByTarget.get(row.targetObjectId) ?? [];
        related.push(row);
        assignmentsByTarget.set(row.targetObjectId, related);
        if (!isActive(row)) continue;
        const active = activeByTarget.get(row.targetObjectId) ?? [];
        active.push(row);
        activeByTarget.set(row.targetObjectId, active);
    }

    const inventory = [...sampleBanks, ...samples];
    const inventoryById = new Map(inventory.map((item) => [item.objectId, item]));
    const rowsById = new Map(
        inventory.map((item) => {
            const active = activeByTarget.get(item.objectId) ?? [];
            return [item.objectId, inventoryRow(item, assignmentsByTarget.get(item.objectId) ?? [], active)] as const;
        }),
    );

    const assigned: ProgramSampleSelectRow[] = [];
    const includedTargetIds = new Set<string>();
    for (const row of orderedAssignments.filter(isActive)) {
        if (row.targetObjectId && inventoryById.has(row.targetObjectId)) {
            if (includedTargetIds.has(row.targetObjectId)) continue;
            includedTargetIds.add(row.targetObjectId);
            assigned.push(rowsById.get(row.targetObjectId)!);
        } else {
            assigned.push(unresolvedRow(row));
        }
    }

    const unresolved = assigned.filter((row) => !row.navigable);
    const all = [...rowsById.values(), ...unresolved].toSorted((left, right) => {
        const typeDifference = typeOrder(left.targetType) - typeOrder(right.targetType);
        if (typeDifference !== 0) return typeDifference;
        const nameDifference = nameCollator.compare(left.targetName, right.targetName);
        return nameDifference !== 0 ? nameDifference : left.id.localeCompare(right.id);
    });

    return { assigned, all };
}
