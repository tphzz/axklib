import type {
    ImageSessionPackageImportDestination,
    ImageSessionPackageImportPlan,
    PackageOpaqueSequenceDecision,
    PackageProgramSlotAssignment,
    PackageRename,
} from '../../lib/transport';
import type { BatchPackageItem, PackageBatchImportRequest } from './packageBatchTypes';
import { importDestination, suggestedPackageVolumeName, type ImportDestinations } from './packageDestinations';

export interface BatchPlanArguments {
    destination: ImageSessionPackageImportDestination;
    renames: PackageRename[];
    programSlotAssignments: PackageProgramSlotAssignment[];
    opaqueSequenceDecisions: PackageOpaqueSequenceDecision[];
}

export function batchDecisionKey(itemId: string, nodeId: string): string {
    return `${itemId}:${nodeId}`;
}

export function separateVolumesAvailable(items: readonly BatchPackageItem[]): boolean {
    return (
        items.length > 0 &&
        items.every(
            (item) =>
                item.inspection.packageKind === 'VOLUME' &&
                item.inspection.roots.length === 1 &&
                item.inspection.roots[0]?.kind === 'VOLUME',
        )
    );
}

export function suggestedSharedVolumeName(items: readonly BatchPackageItem[]): string {
    const selected = items.filter((item) => item.selected);
    return selected.length === 1 ? suggestedPackageVolumeName(selected[0].sourceName, selected[0].inspection) : '';
}

export function batchDestinationName(request: PackageBatchImportRequest, itemId: string): string {
    const item = request.items.find((candidate) => candidate.id === itemId);
    if (!item) return '';
    const packageIndex = request.items
        .filter((candidate) => candidate.selected)
        .findIndex((entry) => entry.id === itemId);
    return (
        request.volumeNames[itemId] ??
        (!request.hasUnvalidatedChanges && packageIndex >= 0
            ? request.plan?.packages.find((candidate) => candidate.packageIndex === packageIndex)?.destinationVolumeName
            : undefined) ??
        item.inspection.roots[0]?.displayName ??
        ''
    );
}

export function normalizedBatchDestination(
    request: PackageBatchImportRequest,
    items: BatchPackageItem[],
    destinations: ImportDestinations,
): Partial<PackageBatchImportRequest> {
    if (request.destinationStrategy === 'separate' && separateVolumesAvailable(items)) return {};
    const selectedVolume = destinations.volumes.find(
        (volume) =>
            volume.partitionIndex === request.destinationPartitionIndex &&
            volume.volumeName === request.destinationVolumeName,
    );
    const mode = selectedVolume ? 'existing' : request.item?.kind === 'partition' ? 'create' : 'existing';
    const firstVolume = selectedVolume ?? destinations.volumes[0];
    return {
        destinationStrategy: 'shared',
        destinationMode: mode,
        destinationPartitionIndex:
            mode === 'existing'
                ? (firstVolume?.partitionIndex ?? null)
                : (request.destinationPartitionIndex ?? destinations.partitions[0]?.partitionIndex ?? null),
        destinationVolumeName: mode === 'existing' ? (firstVolume?.volumeName ?? '') : suggestedSharedVolumeName(items),
    };
}

export function batchPlanArguments(
    request: PackageBatchImportRequest,
    selectedItems: BatchPackageItem[],
): BatchPlanArguments | null {
    const sharedDestination = importDestination(
        request.destinationMode,
        request.destinationPartitionIndex,
        request.destinationVolumeName,
    );
    if (
        request.destinationPartitionIndex === null ||
        (request.destinationStrategy === 'shared' && !sharedDestination) ||
        (request.destinationStrategy === 'separate' && !separateVolumesAvailable(selectedItems))
    ) {
        return null;
    }
    const packageEntries = new Map(selectedItems.map((item, packageIndex) => [item.id, packageIndex]));
    const decisions = <T, R>(
        values: Record<string, T>,
        convert: (packageIndex: number, nodeId: string, value: T) => R,
    ): R[] =>
        Object.entries(values).flatMap(([key, value]) => {
            const separator = key.indexOf(':');
            const packageIndex = packageEntries.get(key.slice(0, separator));
            return separator < 0 || packageIndex === undefined
                ? []
                : [convert(packageIndex, key.slice(separator + 1), value)];
        });
    const volumeNameOverrides = selectedItems
        .map((item, packageIndex) => ({
            packageIndex,
            volumeName: request.volumeNames[item.id]?.trim() ?? '',
        }))
        .filter((item) => item.volumeName.length > 0);
    return {
        destination:
            request.destinationStrategy === 'separate'
                ? {
                      kind: 'CREATE_VOLUMES_FROM_HINTS',
                      partitionIndex: request.destinationPartitionIndex,
                      volumeNameOverrides,
                  }
                : sharedDestination!,
        renames: decisions(request.renames, (packageIndex, nodeId, destinationName) => ({
            packageIndex,
            nodeId,
            destinationName: destinationName.trim(),
        })).filter((rename) => rename.destinationName.length > 0),
        programSlotAssignments: decisions(request.programSlots, (packageIndex, nodeId, destinationSlot) => ({
            packageIndex,
            nodeId,
            destinationSlot,
        })).sort(
            (left, right) =>
                left.destinationSlot - right.destinationSlot ||
                left.packageIndex - right.packageIndex ||
                left.nodeId.localeCompare(right.nodeId),
        ),
        opaqueSequenceDecisions: decisions(request.opaqueSequenceActions, (packageIndex, nodeId, action) => ({
            packageIndex,
            nodeId,
            action,
        })),
    };
}

export function mergeBatchPlanSuggestions(
    request: PackageBatchImportRequest,
    selectedItems: BatchPackageItem[],
    plan: ImageSessionPackageImportPlan,
): Pick<PackageBatchImportRequest, 'volumeNames' | 'renames' | 'programSlots'> & { suggestedSlotsAdded: boolean } {
    const volumeNames = { ...request.volumeNames };
    for (const summary of plan.packages) {
        const item = selectedItems[summary.packageIndex];
        if (item) volumeNames[item.id] ??= summary.destinationVolumeName;
    }
    const placementKeys = new Set(
        plan.programSlotPlacements.flatMap((placement) =>
            placement.mappings.flatMap((mapping) => {
                const item = selectedItems[mapping.packageIndex];
                return item ? [batchDecisionKey(item.id, mapping.nodeId)] : [];
            }),
        ),
    );
    const renames = Object.fromEntries(Object.entries(request.renames).filter(([key]) => !placementKeys.has(key)));
    for (const action of plan.actions) {
        const item = selectedItems[action.packageIndex];
        if (!item) continue;
        const key = batchDecisionKey(item.id, action.nodeId);
        if (
            !placementKeys.has(key) &&
            plan.conflicts.some(
                (conflict) => conflict.packageIndex === action.packageIndex && conflict.nodeId === action.nodeId,
            ) &&
            !renames[key]
        ) {
            renames[key] = action.destinationName;
        }
    }
    const programSlots = { ...request.programSlots };
    let suggestedSlotsAdded = false;
    for (const placement of plan.programSlotPlacements) {
        for (const mapping of placement.mappings) {
            const item = selectedItems[mapping.packageIndex];
            if (!item) continue;
            const key = batchDecisionKey(item.id, mapping.nodeId);
            if (programSlots[key] === undefined) {
                programSlots[key] = mapping.destinationSlot;
                suggestedSlotsAdded = true;
            }
        }
    }
    return { volumeNames, renames, programSlots, suggestedSlotsAdded };
}
