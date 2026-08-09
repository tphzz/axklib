import { describe, expect, it, vi } from 'vitest';
import { serverFileLocation } from '../../lib/storageLocations';
import type { ImageSessionPackageImportPlan, ImageTransport, PackageInspection } from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import { PickerController } from '../dialogs/picker';
import type { JobController } from '../jobs/actions';
import { PackageBatchImportWorkflow } from './packageBatchWorkflow.svelte';
import { PackagePickerHistory } from './packagePickerHistory';

function inspection(packageId: string, name: string, programs: number): PackageInspection {
    return {
        schemaVersion: '1.0',
        packageId,
        packageKind: 'VOLUME',
        requiredExtension: '.axkvol',
        sourceMediaKind: 'SFS',
        valid: true,
        payloadsVerified: true,
        totalPayloadBytes: programs * 100,
        roots: [{ kind: 'VOLUME', displayName: name, nodeIds: [] }],
        objects: Array.from({ length: programs }, (_, index) => ({
            nodeId: `${packageId}-${index}`,
            objectType: 'PROG' as const,
            name: String(index + 1).padStart(3, '0'),
            payloadSha256: '',
            normalizedSha256: '',
            semanticSha256: null,
            audioSha256: null,
            payloadSizeBytes: 100,
        })),
        relationships: [],
        relationshipCount: 0,
        issues: [],
    };
}

function plan(names: string[], token: string): ImageSessionPackageImportPlan {
    return {
        schemaVersion: '1.0',
        imageId: 'image-1',
        revision: 1,
        planToken: token,
        expiresInSeconds: 600,
        planId: `${token}-id`,
        targetKind: 'SFS',
        targetSnapshotId: 'snapshot-1',
        packages: names.map((name, packageIndex) => ({
            packageIndex,
            packageId: `package-${packageIndex}`,
            sourceVolumeName: 'Drums',
            destinationVolumeName: name,
            objectCount: packageIndex + 1,
            payloadBytes: (packageIndex + 1) * 100,
            objectCounts: {
                programs: packageIndex + 1,
                sampleBanks: 0,
                samples: 0,
                waveData: 0,
                sequences: 0,
            },
        })),
        valid: true,
        warnings: [],
        conflicts: [],
        actions: [],
        opaqueSequences: [],
        programAssignmentAdjustments: [],
        programSlotPlacements: [],
        allocation: [],
    };
}

describe('PackageBatchImportWorkflow', () => {
    it('previews unique server suggestions, replans edited names, and applies one atomic plan', async () => {
        const sources = [
            serverFileLocation({ rootId: 'workspace', relativePath: 'one.axkvol' }, 'one.axkvol'),
            serverFileLocation({ rootId: 'workspace', relativePath: 'two.axkvol' }, 'two.axkvol'),
        ];
        const planImagePackageImport = vi
            .fn()
            .mockResolvedValueOnce(plan(['Drums', 'Drums 2'], 'initial-plan'))
            .mockResolvedValueOnce(plan(['Drums', 'Percussion'], 'renamed-plan'));
        const startImagePackageImport = vi.fn().mockResolvedValue({ jobId: 8, status: 'queued' });
        const transport = {
            inspectPackage: vi
                .fn()
                .mockResolvedValueOnce(inspection('package-0', 'Drums', 1))
                .mockResolvedValueOnce(inspection('package-1', 'Drums', 2)),
            planImagePackageImport,
            releaseImagePackageImportPlan: vi.fn().mockResolvedValue(undefined),
            startImagePackageImport,
        } as unknown as ImageTransport;
        const picker = new PickerController(() => undefined);
        const pickerHistory = new PackagePickerHistory();
        const refreshSession = vi.fn().mockResolvedValue(undefined);
        const workflow = new PackageBatchImportWorkflow({
            transport,
            jobs: {
                run: vi.fn(async (start: () => Promise<unknown>) => {
                    await start();
                    return { status: 'completed' };
                }),
            } as unknown as JobController,
            picker,
            pickerHistory,
            isDesktop: false,
            sessionId: () => 17,
            invalidateSession: vi.fn().mockResolvedValue(undefined),
            refreshSession,
            setStatus: vi.fn(),
        });
        const partition: DiskTreeItem = {
            id: 'partition-0',
            name: 'Partition 1',
            kind: 'partition',
            childCount: 1,
            partitionIndex: 0,
        };

        workflow.open(partition);
        const choosing = workflow.chooseWorkspace();
        picker.finish(sources);
        await choosing;

        expect(workflow.request?.plan?.packages.map((item) => item.destinationVolumeName)).toEqual([
            'Drums',
            'Drums 2',
        ]);
        expect(planImagePackageImport).toHaveBeenNthCalledWith(
            1,
            17,
            sources,
            {
                kind: 'CREATE_VOLUMES_FROM_HINTS',
                partitionIndex: 0,
                volumeNameOverrides: [],
            },
            [],
            [],
            undefined,
            [],
        );

        workflow.renameVolume(1, 'Percussion');
        expect(workflow.request?.hasUnvalidatedChanges).toBe(true);
        await workflow.replan();
        expect(planImagePackageImport).toHaveBeenNthCalledWith(
            2,
            17,
            sources,
            {
                kind: 'CREATE_VOLUMES_FROM_HINTS',
                partitionIndex: 0,
                volumeNameOverrides: [
                    { packageIndex: 0, volumeName: 'Drums' },
                    { packageIndex: 1, volumeName: 'Percussion' },
                ],
            },
            [],
            [],
            'initial-plan',
            [],
        );

        await workflow.apply();
        expect(startImagePackageImport).toHaveBeenCalledWith('renamed-plan');
        expect(refreshSession).toHaveBeenCalledWith({ partitionIndex: 0, volumeName: 'Drums' });
        expect(pickerHistory.lastImportedWorkspaceFile).toEqual({
            rootId: 'workspace',
            relativePath: 'two.axkvol',
        });
        expect(workflow.request).toBeNull();
    });
});
