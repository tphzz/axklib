import type { ImageSessionPackageImportPlan } from './transport';
import type { ImportPartitionOption } from '../features/import/packageDestinations';

type Conflict = ImageSessionPackageImportPlan['conflicts'][number];

export function isImportSpaceConflict(conflict: Conflict): boolean {
    return conflict.code === 'SFS_CLUSTER_EXHAUSTED';
}

export function importCapacityGroups(conflicts: Conflict[], partitions: ImportPartitionOption[]) {
    const groups = new Map<number | null, { partitionIndex: number | null; name: string; conflicts: Conflict[] }>();
    for (const conflict of conflicts) {
        if (!isImportSpaceConflict(conflict)) continue;
        const index = conflict.partitionIndex;
        let group = groups.get(index);
        if (!group) {
            group = {
                partitionIndex: index,
                name:
                    partitions.find((partition) => partition.partitionIndex === index)?.name ||
                    (index === null ? 'the destination partition' : `Partition ${index + 1}`),
                conflicts: [],
            };
            groups.set(index, group);
        }
        group.conflicts.push(conflict);
    }
    return [...groups.values()];
}
