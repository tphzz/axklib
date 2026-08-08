import { describe, expect, it, vi } from 'vitest';
import { serverFileLocation } from '../../lib/storageLocations';
import type { ImageSessionPackageImportPlan, ImageTransport, PackageInspection } from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import { PickerController } from '../dialogs/picker';
import type { JobController } from '../jobs/actions';
import { PackageImportWorkflow } from './packageWorkflow.svelte';

const inspection: PackageInspection = {
    schemaVersion: '1.0',
    packageId: 'program-package',
    packageKind: 'PROGRAM',
    requiredExtension: '.axkprg',
    sourceMediaKind: 'A3K_ARCHIVE',
    valid: true,
    payloadsVerified: true,
    totalPayloadBytes: 1024,
    roots: [],
    objects: [],
    relationships: [],
    relationshipCount: 0,
    issues: [],
};

function programPlan(applied: boolean): ImageSessionPackageImportPlan {
    const mappings = Array.from({ length: 33 }, (_, index) => ({
        packageIndex: 0,
        nodeId: `program-${index + 1}`,
        sourceSlot: index + 1,
        destinationSlot: index + 5,
        requiresUserAction: false,
    }));
    return {
        schemaVersion: '1.0',
        imageId: 'image-1',
        revision: 1,
        planToken: applied ? 'checked-plan' : 'suggested-plan',
        expiresInSeconds: 600,
        planId: applied ? 'checked-id' : 'suggested-id',
        targetKind: 'SFS',
        targetSnapshotId: 'snapshot-1',
        valid: applied,
        warnings: [],
        conflicts: applied
            ? []
            : mappings.slice(0, 4).map((mapping) => ({
                  code: 'SFS_NAME_CONFLICT',
                  message: 'destination already contains the same object name with different content',
                  nodeId: mapping.nodeId,
                  packageId: 'program-package',
                  packageIndex: 0,
                  rootIndex: mapping.sourceSlot - 1,
                  partitionIndex: 0,
                  groupName: '',
                  volumeName: 'Imported',
                  rawGroup: '',
                  rawVolume: '',
              })),
        actions: mappings.map((mapping) => ({
            actionId: `action-${mapping.nodeId}`,
            packageIndex: 0,
            rootIndex: mapping.sourceSlot - 1,
            packageId: 'program-package',
            nodeId: mapping.nodeId,
            objectType: 'PROG',
            sourceName: String(mapping.sourceSlot).padStart(3, '0'),
            destinationName: String(mapping.destinationSlot).padStart(3, '0'),
            partitionIndex: 0,
            groupName: '',
            volumeName: 'Imported',
            rawGroup: '',
            rawVolume: '',
            actions: applied ? (['INSERT'] as const) : (['CONFLICT'] as const),
            canonicalActionId: null,
            targetSfsId: null,
            targetWaveDataReferenceValue: null,
        })),
        programAssignmentAdjustments: [],
        programSlotPlacements: [
            {
                placementId: 'placement-1',
                partitionIndex: 0,
                volumeName: 'Imported',
                mode: 'CONTIGUOUS',
                applied,
                suggestedStartSlot: 5,
                requiredSlotCount: 33,
                availableSlotCount: 124,
                occupiedRanges: [{ first: 1, last: 4 }],
                sourceRanges: [{ first: 1, last: 33 }],
                destinationRanges: [{ first: 5, last: 37 }],
                mappings,
            },
        ],
        allocation: [],
    };
}

describe('PackageImportWorkflow', () => {
    it('replans compact Program suggestions as slot assignments without accumulating renames', async () => {
        const source = serverFileLocation(
            { rootId: 'workspace', relativePath: 'CosmoPad and others.axkprg' },
            'CosmoPad and others.axkprg',
        );
        const inspectPackage = vi.fn().mockResolvedValue(inspection);
        const planImagePackageImport = vi
            .fn()
            .mockResolvedValueOnce(programPlan(false))
            .mockResolvedValueOnce(programPlan(true));
        const transport = {
            inspectPackage,
            planImagePackageImport,
            releaseImagePackageImportPlan: vi.fn().mockResolvedValue(undefined),
        } as unknown as ImageTransport;
        const picker = new PickerController(() => undefined);
        const workflow = new PackageImportWorkflow({
            transport,
            jobs: {} as JobController,
            picker,
            isDesktop: false,
            sessionId: () => 17,
            invalidateSession: vi.fn(),
            refreshSession: vi.fn(),
            setStatus: vi.fn(),
        });
        const target: DiskTreeItem = {
            id: 'volume-imported',
            name: 'Imported',
            kind: 'volume',
            childCount: 4,
            partitionIndex: 0,
        };

        workflow.open(target);
        const choosing = workflow.chooseWorkspace();
        picker.finish(source);
        await choosing;

        expect(workflow.request?.renames).toEqual({});
        expect(workflow.request?.programSlots).toEqual(
            Object.fromEntries(Array.from({ length: 33 }, (_, index) => [`program-${index + 1}`, index + 5])),
        );
        expect(planImagePackageImport).toHaveBeenNthCalledWith(1, 17, source, 0, 'Imported', [], []);

        await workflow.replan();

        expect(planImagePackageImport).toHaveBeenNthCalledWith(
            2,
            17,
            source,
            0,
            'Imported',
            [],
            Array.from({ length: 33 }, (_, index) => ({
                nodeId: `program-${index + 1}`,
                destinationSlot: index + 5,
            })),
            'suggested-plan',
        );
        expect(workflow.request?.plan?.valid).toBe(true);
        expect(workflow.request?.renames).toEqual({});
    });
});
