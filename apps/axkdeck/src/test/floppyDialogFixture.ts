import { FloppyImportWorkflow } from '../features/import/floppyWorkflow.svelte';
import { PickerController } from '../features/dialogs/picker';
import type { ImageTransport, ImageSessionPackageImportPlan } from '../lib/transport';
import type { JobController } from '../features/jobs/actions';
import type { DiskTreeItem } from '../lib/types';
import { serverDirectoryLocation } from '../lib/storageLocations';

export function floppyDialogFixture(count = 250, directSource = false, folder?: string) {
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
        connectionMode: directSource ? 'local' : 'remote',
        releaseFloppyInspection: async () => {},
        releaseImagePackageImportPlan: async () => {},
        planFloppyImport: async () => plan,
        startFloppyInspection: async () => ({
            jobId: 1,
            kind: 'images.floppy_import.inspect',
            status: 'completed',
            result: inspection,
        }),
    } as unknown as ImageTransport;
    const workflow = new FloppyImportWorkflow({
        transport,
        jobs: { run: (start: () => Promise<unknown>) => start() } as unknown as JobController,
        picker: new PickerController(() => {}),
        isDesktop: directSource,
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
    if (!folder) workflow.setDestination('create', 0, 'New volume');
    request.inspection = {
        format: 'A_SERIES',
        inspectionToken: 'inspection',
        complete: true,
        label: folder ? '' : 'Sampler floppy',
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
    const inspection = request.inspection;
    if (folder) {
        request.members = [];
        void workflow.add([serverDirectoryLocation({ rootId: 'yamaha', relativePath: folder }, `Yamaha/${folder}`)]);
    }
    return workflow;
}
