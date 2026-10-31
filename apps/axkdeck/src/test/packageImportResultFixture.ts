import type { components } from '../lib/generated/axklibApiV1';

export function packageImportResult(
    targetKind: components['schemas']['ImageSessionPackageImportResult']['targetKind'] = 'SFS',
): components['schemas']['ImageSessionPackageImportResult'] {
    return {
        schemaVersion: '1.0',
        imageId: 'image-1',
        documentId: 'document-1',
        sourceMemberId: 'source-1',
        revision: 2,
        planId: 'plan-1',
        targetKind,
        targetSnapshotId: 'snapshot-1',
        applied: true,
        objectCount: 1,
        actions: [],
        allocation: [],
        programAssignmentAdjustments: [],
        programSlotPlacements: [],
    };
}
