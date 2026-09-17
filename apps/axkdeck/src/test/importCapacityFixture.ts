import type { ImageSessionPackageImportPlan } from '../lib/transport';

export function capacityConflict(
    partitionIndex: number | null,
    nodeId = 'wave',
): ImageSessionPackageImportPlan['conflicts'][number] {
    return {
        code: 'SFS_CLUSTER_EXHAUSTED',
        message: `Allocation failed for ${nodeId}`,
        partitionIndex,
        nodeId,
        packageIndex: 0,
        rootIndex: 0,
        packageId: 'package',
        groupName: '',
        volumeName: 'Target',
        rawGroup: '',
        rawVolume: '',
    };
}
