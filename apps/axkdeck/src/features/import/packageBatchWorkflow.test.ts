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
        sfsIndexCapacity: [],
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
        const releaseImagePackageImportPlan = vi.fn().mockResolvedValue(undefined);
        const startImagePackageImport = vi.fn().mockResolvedValue({ jobId: 8, status: 'queued' });
        const transport = {
            inspectPackage: vi
                .fn()
                .mockResolvedValueOnce(inspection('package-0', 'Drums', 1))
                .mockResolvedValueOnce(inspection('package-1', 'Drums', 2)),
            planImagePackageImport,
            releaseImagePackageImportPlan,
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

        const secondItemId = workflow.request!.items[1].id;
        workflow.setSelected(secondItemId, false);
        workflow.setSelected(secondItemId, true);
        workflow.renameVolume(secondItemId, 'Percussion');
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
        expect(releaseImagePackageImportPlan).not.toHaveBeenCalled();

        await workflow.apply();
        expect(startImagePackageImport).toHaveBeenCalledWith('renamed-plan');
        expect(refreshSession).toHaveBeenCalledWith({ partitionIndex: 0, volumeName: 'Drums' });
        expect(pickerHistory.lastImportedWorkspaceFile).toEqual({
            rootId: 'workspace',
            relativePath: 'two.axkvol',
        });
        expect(workflow.request).toBeNull();
    });

    it('preserves package state while replanning and importing only selected packages', async () => {
        const sources = [
            serverFileLocation({ rootId: 'workspace', relativePath: 'one.axkvol' }, 'one.axkvol'),
            serverFileLocation({ rootId: 'workspace', relativePath: 'two.axkvol' }, 'two.axkvol'),
            serverFileLocation({ rootId: 'workspace', relativePath: 'three.axkvol' }, 'three.axkvol'),
        ];
        const initialPlan = plan(['One', 'Two', 'Three'], 'initial-plan');
        initialPlan.valid = false;
        const planImagePackageImport = vi
            .fn()
            .mockResolvedValueOnce(initialPlan)
            .mockResolvedValueOnce(plan(['One', 'Three'], 'subset-plan'));
        const releaseImagePackageImportPlan = vi.fn().mockResolvedValue(undefined);
        const startImagePackageImport = vi.fn().mockResolvedValue({ jobId: 9, status: 'queued' });
        const transport = {
            inspectPackage: vi
                .fn()
                .mockResolvedValueOnce(inspection('package-0', 'One', 1))
                .mockResolvedValueOnce(inspection('package-1', 'Two', 1))
                .mockResolvedValueOnce(inspection('package-2', 'Three', 1)),
            planImagePackageImport,
            releaseImagePackageImportPlan,
            startImagePackageImport,
        } as unknown as ImageTransport;
        const picker = new PickerController(() => undefined);
        const pickerHistory = new PackagePickerHistory();
        const setStatus = vi.fn();
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
            refreshSession: vi.fn().mockResolvedValue(undefined),
            setStatus,
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

        const [one, two, three] = workflow.request!.items;
        expect(workflow.request?.items.every((item) => item.selected)).toBe(true);
        workflow.renameVolume(three.id, 'Three edited');
        workflow.setSelected(two.id, false);

        expect(workflow.request?.items).toHaveLength(3);
        expect(workflow.request?.items[1].selected).toBe(false);
        expect(workflow.request?.volumeNames[three.id]).toBe('Three edited');
        expect(workflow.request?.hasUnvalidatedChanges).toBe(true);

        await workflow.replan();

        expect(planImagePackageImport).toHaveBeenNthCalledWith(
            2,
            17,
            [sources[0], sources[2]],
            {
                kind: 'CREATE_VOLUMES_FROM_HINTS',
                partitionIndex: 0,
                volumeNameOverrides: [
                    { packageIndex: 0, volumeName: 'One' },
                    { packageIndex: 1, volumeName: 'Three edited' },
                ],
            },
            [],
            [],
            undefined,
            [],
        );
        expect(releaseImagePackageImportPlan).toHaveBeenCalledWith('initial-plan');
        expect(releaseImagePackageImportPlan.mock.invocationCallOrder[0]).toBeLessThan(
            planImagePackageImport.mock.invocationCallOrder[1],
        );
        expect(workflow.destinationName(one.id)).toBe('One');
        expect(workflow.destinationName(two.id)).toBe('Two');
        expect(workflow.destinationName(three.id)).toBe('Three edited');

        await workflow.apply();

        expect(startImagePackageImport).toHaveBeenCalledWith('subset-plan');
        expect(pickerHistory.lastImportedWorkspaceFile).toEqual({
            rootId: 'workspace',
            relativePath: 'three.axkvol',
        });
        expect(setStatus).toHaveBeenCalledWith('Imported 2 volume packages');
    });

    it('refreshes and replans automatically when the image changed before apply', async () => {
        const source = serverFileLocation({ rootId: 'workspace', relativePath: 'one.axkvol' }, 'one.axkvol');
        const planImagePackageImport = vi
            .fn()
            .mockResolvedValueOnce(plan(['One'], 'stale-plan'))
            .mockResolvedValueOnce(plan(['One'], 'fresh-plan'));
        const releaseImagePackageImportPlan = vi.fn().mockResolvedValue(undefined);
        const refreshSession = vi.fn().mockResolvedValue(undefined);
        const setStatus = vi.fn();
        const picker = new PickerController(() => undefined);
        const transport = {
            inspectPackage: vi.fn().mockResolvedValue(inspection('package-0', 'One', 1)),
            planImagePackageImport,
            releaseImagePackageImportPlan,
            startImagePackageImport: vi.fn().mockResolvedValue({ jobId: 10, status: 'queued' }),
        } as unknown as ImageTransport;
        const workflow = new PackageBatchImportWorkflow({
            transport,
            jobs: {
                run: vi.fn(async (start: () => Promise<unknown>) => {
                    await start();
                    return {
                        status: 'failed',
                        errorCode: 'image_revision_stale',
                        error: 'image session revision changed',
                    };
                }),
            } as unknown as JobController,
            picker,
            pickerHistory: new PackagePickerHistory(),
            isDesktop: false,
            sessionId: () => 17,
            invalidateSession: vi.fn().mockResolvedValue(undefined),
            refreshSession,
            setStatus,
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
        picker.finish([source]);
        await choosing;
        await workflow.apply();

        expect(refreshSession).toHaveBeenCalledWith({ partitionIndex: 0 });
        expect(releaseImagePackageImportPlan).toHaveBeenCalledWith('stale-plan');
        expect(planImagePackageImport).toHaveBeenCalledTimes(2);
        expect(planImagePackageImport).toHaveBeenLastCalledWith(
            17,
            [source],
            {
                kind: 'CREATE_VOLUMES_FROM_HINTS',
                partitionIndex: 0,
                volumeNameOverrides: [{ packageIndex: 0, volumeName: 'One' }],
            },
            [],
            [],
            undefined,
            [],
        );
        expect(workflow.request?.plan?.planToken).toBe('fresh-plan');
        expect(workflow.request?.status).toBe('ready');
        expect(setStatus).toHaveBeenLastCalledWith('Image changed; import conflicts checked again');
    });

    it('refreshes and closes after an already-submitted job cannot be confirmed', async () => {
        const source = serverFileLocation({ rootId: 'workspace', relativePath: 'one.axkvol' }, 'one.axkvol');
        const refreshSession = vi.fn().mockResolvedValue(undefined);
        const setStatus = vi.fn();
        const picker = new PickerController(() => undefined);
        const transport = {
            inspectPackage: vi.fn().mockResolvedValue(inspection('package-0', 'One', 1)),
            planImagePackageImport: vi.fn().mockResolvedValue(plan(['One'], 'uncertain-plan')),
            releaseImagePackageImportPlan: vi.fn().mockResolvedValue(undefined),
            startImagePackageImport: vi.fn().mockResolvedValue({ jobId: 11, status: 'queued' }),
        } as unknown as ImageTransport;
        const workflow = new PackageBatchImportWorkflow({
            transport,
            jobs: {
                run: vi.fn(
                    async (
                        start: () => Promise<unknown>,
                        _onUpdate: (job: unknown) => void,
                        onStarted: (job: unknown) => void | Promise<void>,
                    ) => {
                        const job = await start();
                        await onStarted(job);
                        throw new Error('job result violated its declared schema');
                    },
                ),
            } as unknown as JobController,
            picker,
            pickerHistory: new PackagePickerHistory(),
            isDesktop: false,
            sessionId: () => 17,
            invalidateSession: vi.fn().mockResolvedValue(undefined),
            refreshSession,
            setStatus,
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
        picker.finish([source]);
        await choosing;
        await workflow.apply();

        expect(refreshSession).toHaveBeenCalledWith({ partitionIndex: 0, volumeName: 'One' });
        expect(workflow.request).toBeNull();
        expect(setStatus).toHaveBeenLastCalledWith(
            'Import completion could not be confirmed; review the refreshed image before retrying',
        );
    });
});
