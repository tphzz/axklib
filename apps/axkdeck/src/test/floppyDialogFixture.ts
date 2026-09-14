import { FloppyImportWorkflow } from '../features/import/floppyWorkflow.svelte';
import { PickerController } from '../features/dialogs/picker';
import type { ImageTransport, ImageSessionPackageImportPlan } from '../lib/transport';
import type { JobController } from '../features/jobs/actions';
import type { DiskTreeItem } from '../lib/types';

export function floppyDialogFixture(count = 250) {
    const partition: DiskTreeItem = {
        id: 'p0',
        name: 'Partition 1',
        kind: 'partition',
        partitionIndex: 0,
        childCount: 1,
        children: [{ id: 'v0', name: 'Existing', kind: 'volume', partitionIndex: 0, childCount: 0 }],
    };
    const plan: ImageSessionPackageImportPlan = {
        schemaVersion: '1.0',
        imageId: 'image',
        revision: 1,
        planToken: 'plan',
        expiresInSeconds: 900,
        planId: 'plan',
        targetKind: 'SFS',
        targetSnapshotId: 'snapshot',
        valid: true,
        warnings: [],
        conflicts: [],
        actions: [],
        opaqueSequences: [],
        programAssignmentAdjustments: [],
        programSlotPlacements: [],
        allocation: [],
        sfsIndexCapacity: [],
        packages: [],
    };
    const transport = {
        releaseFloppyInspection: async () => {},
        releaseImagePackageImportPlan: async () => {},
        planFloppyImport: async () => plan,
    } as unknown as ImageTransport;
    const workflow = new FloppyImportWorkflow({
        transport,
        jobs: {} as JobController,
        picker: new PickerController(() => {}),
        isDesktop: false,
        sessionId: () => 1,
        sourceItems: () => [partition],
        mutationsAvailable: () => true,
        invalidateSession: async () => {},
        refreshSession: async () => {},
        setStatus: () => {},
        otherFormat: async () => {},
    });
    workflow.open(partition);
    const request = workflow.request!;
    request.status = 'ready';
    request.volumeName = 'New volume';
    request.inspection = {
        format: 'A_SERIES',
        inspectionToken: 'inspection',
        complete: true,
        label: 'Sampler floppy',
        nextRequiredIndex: null,
        members: [{ index: 1, label: 'Sampler floppy' }],
        excludedFiles: [{ memberName: 'test.img', path: 'SYSTEM2.002', sizeBytes: 1024 }],
        issues: [],
        objects: Array.from({ length: count }, (_, i) => ({
            objectKey: `wave-${i}`,
            name: `Wave ${i}`,
            displayName: `Wave ${i}`,
            objectType: 'SMPL',
            sizeBytes: 2048,
            requiredObjectKeys: [],
            exclusionReason: '',
        })),
    };
    request.selected = request.inspection.objects.map((o) => o.objectKey);
    request.members = [
        {
            id: 1,
            name: 'Sampler floppy.img',
            source: {
                name: 'Sampler floppy.img',
                size: 1474560,
                type: 'application/octet-stream',
                readChunk: async () => new Blob(),
            },
            input: null,
            upload: null,
        },
    ];
    request.plan = plan;
    request.dirty = false;
    return workflow;
}
