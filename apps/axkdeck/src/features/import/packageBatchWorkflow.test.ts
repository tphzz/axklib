import { describe, expect, it, vi } from 'vitest';
import { serverFileLocation } from '../../lib/storageLocations';
import type { ImageSessionPackageImportPlan, ImageTransport, PackageInspection } from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import { PickerController } from '../dialogs/picker';
import type { JobController } from '../jobs/actions';
import { PackageBatchImportWorkflow } from './packageBatchWorkflow.svelte';
import { PackagePickerHistory } from './packagePickerHistory';

function inspection(
    packageId: string,
    name: string,
    programs: number,
    packageKind: Exclude<PackageInspection['packageKind'], 'BUNDLE'> = 'VOLUME',
): PackageInspection {
    const rootKind = packageKind === 'SEQUENCE' ? 'SEQU' : packageKind;
    return {
        schemaVersion: '1.0',
        packageId,
        packageKind,
        requiredExtension: packageKind === 'VOLUME' ? '.axkvol' : '.axkprg',
        sourceMediaKind: 'SFS',
        valid: true,
        payloadsVerified: true,
        totalPayloadBytes: programs * 100,
        roots: [{ kind: rootKind, displayName: name, nodeIds: [] }],
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
    it('clears a shared existing volume without planning when its partition changes', () => {
        const firstVolume: DiskTreeItem = {
            id: 'volume-0',
            name: 'First',
            kind: 'volume',
            childCount: 0,
            partitionIndex: 0,
        };
        const secondVolume: DiskTreeItem = {
            id: 'volume-1',
            name: 'Second',
            kind: 'volume',
            childCount: 0,
            partitionIndex: 1,
        };
        const planImagePackageImport = vi.fn();
        const workflow = new PackageBatchImportWorkflow({
            transport: { planImagePackageImport } as unknown as ImageTransport,
            jobs: {} as JobController,
            picker: new PickerController(() => undefined),
            pickerHistory: new PackagePickerHistory(),
            isDesktop: false,
            sessionId: () => 17,
            invalidateSession: vi.fn(),
            refreshSession: vi.fn(),
            setStatus: vi.fn(),
            sourceItems: () => [firstVolume, secondVolume],
        });

        workflow.open(firstVolume);
        workflow.setDestinationPartition(1);

        expect(workflow.request).toMatchObject({
            destinationStrategy: 'shared',
            destinationMode: 'existing',
            destinationPartitionIndex: 1,
            destinationVolumeName: '',
            plan: null,
        });
        expect(planImagePackageImport).not.toHaveBeenCalled();
    });

    it('waits for an explicit conflict check after loading packages', async () => {
        const sources = [serverFileLocation({ rootId: 'workspace', relativePath: 'one.axkvol' }, 'one.axkvol')];
        const planImagePackageImport = vi.fn().mockResolvedValue(plan(['One'], 'explicit-plan'));
        const picker = new PickerController(() => undefined);
        const workflow = new PackageBatchImportWorkflow({
            transport: {
                inspectPackage: vi.fn().mockResolvedValue(inspection('package-0', 'One', 1)),
                planImagePackageImport,
                releaseImagePackageImportPlan: vi.fn().mockResolvedValue(undefined),
            } as unknown as ImageTransport,
            jobs: {} as JobController,
            picker,
            pickerHistory: new PackagePickerHistory(),
            isDesktop: false,
            sessionId: () => 17,
            invalidateSession: vi.fn().mockResolvedValue(undefined),
            refreshSession: vi.fn().mockResolvedValue(undefined),
            setStatus: vi.fn(),
        });
        const partition: DiskTreeItem = {
            id: 'partition-0',
            name: 'Partition 1',
            kind: 'partition',
            childCount: 0,
            partitionIndex: 0,
        };

        workflow.open(partition);
        const choosing = workflow.chooseWorkspace();
        picker.finish(sources);
        await choosing;

        expect(planImagePackageImport).not.toHaveBeenCalled();
        expect(workflow.request).toMatchObject({
            plan: null,
            status: 'ready',
            hasUnvalidatedChanges: true,
        });

        await workflow.replan();

        expect(planImagePackageImport).toHaveBeenCalledTimes(1);
        expect(workflow.request?.plan?.planToken).toBe('explicit-plan');

        const itemId = workflow.request!.items[0].id;
        workflow.opaqueSequenceAction(itemId, 'sequence-1', 'SKIP');
        workflow.setDestinationPartition(1);
        await Promise.resolve();

        expect(planImagePackageImport).toHaveBeenCalledTimes(1);
        expect(workflow.request).toMatchObject({
            plan: null,
            status: 'ready',
            hasUnvalidatedChanges: true,
        });
    });

    it('checks suggested Program slots once within an explicit batch conflict check', async () => {
        const sources = [
            serverFileLocation({ rootId: 'workspace', relativePath: 'pads-1.axkvol' }, 'pads-1.axkvol'),
            serverFileLocation({ rootId: 'workspace', relativePath: 'pads-2.axkvol' }, 'pads-2.axkvol'),
            serverFileLocation({ rootId: 'workspace', relativePath: 'pads-3.axkvol' }, 'pads-3.axkvol'),
        ];
        const suggestedPlan = plan(['Pads 1', 'Pads 2', 'Pads 3'], 'suggested-plan');
        suggestedPlan.valid = false;
        suggestedPlan.programSlotPlacements = [
            {
                placementId: 'placement-1',
                partitionIndex: 0,
                volumeName: 'Target',
                mode: 'CONTIGUOUS',
                applied: false,
                suggestedStartSlot: 21,
                requiredSlotCount: 3,
                availableSlotCount: 108,
                occupiedRanges: [{ first: 1, last: 20 }],
                sourceRanges: [{ first: 1, last: 3 }],
                destinationRanges: [{ first: 21, last: 23 }],
                mappings: sources.map((_, packageIndex) => ({
                    packageIndex,
                    nodeId: `package-${packageIndex}-0`,
                    sourceSlot: packageIndex + 1,
                    destinationSlot: packageIndex + 21,
                    requiresUserAction: false,
                })),
            },
        ];
        const checkedPlan = plan(['Pads 1', 'Pads 2', 'Pads 3'], 'checked-plan');
        checkedPlan.programSlotPlacements = suggestedPlan.programSlotPlacements.map((placement) => ({
            ...placement,
            applied: true,
        }));
        const planImagePackageImport = vi.fn().mockResolvedValueOnce(suggestedPlan).mockResolvedValueOnce(checkedPlan);
        const picker = new PickerController(() => undefined);
        const volume: DiskTreeItem = {
            id: 'volume-0',
            name: 'Target',
            kind: 'volume',
            childCount: 20,
            partitionIndex: 0,
        };
        const workflow = new PackageBatchImportWorkflow({
            transport: {
                inspectPackage: vi
                    .fn()
                    .mockResolvedValueOnce(inspection('package-0', 'Pads 1', 1))
                    .mockResolvedValueOnce(inspection('package-1', 'Pads 2', 1))
                    .mockResolvedValueOnce(inspection('package-2', 'Pads 3', 1)),
                planImagePackageImport,
                releaseImagePackageImportPlan: vi.fn().mockResolvedValue(undefined),
            } as unknown as ImageTransport,
            jobs: {} as JobController,
            picker,
            pickerHistory: new PackagePickerHistory(),
            isDesktop: false,
            sessionId: () => 17,
            invalidateSession: vi.fn().mockResolvedValue(undefined),
            refreshSession: vi.fn().mockResolvedValue(undefined),
            setStatus: vi.fn(),
            sourceItems: () => [volume],
        });

        workflow.open(volume);
        const choosing = workflow.chooseWorkspace();
        picker.finish(sources);
        await choosing;
        await workflow.replan();

        expect(planImagePackageImport).toHaveBeenCalledTimes(2);
        expect(planImagePackageImport).toHaveBeenNthCalledWith(
            2,
            17,
            sources,
            { kind: 'EXISTING_VOLUME', partitionIndex: 0, volumeName: 'Target' },
            [],
            sources.map((_, packageIndex) => ({
                packageIndex,
                nodeId: `package-${packageIndex}-0`,
                destinationSlot: packageIndex + 21,
            })),
            'suggested-plan',
            [],
        );
        expect(workflow.request).toMatchObject({
            plan: { planToken: 'checked-plan', valid: true },
            status: 'ready',
            hasUnvalidatedChanges: false,
        });
        expect(Object.values(workflow.request!.programSlots)).toEqual([21, 22, 23]);
    });

    it('limits automatic batch Program slot checks to one replacement plan', async () => {
        const source = serverFileLocation({ rootId: 'workspace', relativePath: 'one.axkvol' }, 'one.axkvol');
        const firstPlan = plan(['One'], 'suggested-plan-1');
        firstPlan.valid = false;
        firstPlan.programSlotPlacements = [
            {
                placementId: 'placement-1',
                partitionIndex: 0,
                volumeName: 'One',
                mode: 'CONTIGUOUS',
                applied: false,
                suggestedStartSlot: 5,
                requiredSlotCount: 1,
                availableSlotCount: 124,
                occupiedRanges: [{ first: 1, last: 4 }],
                sourceRanges: [{ first: 1, last: 1 }],
                destinationRanges: [{ first: 5, last: 5 }],
                mappings: [
                    {
                        packageIndex: 0,
                        nodeId: 'package-0-0',
                        sourceSlot: 1,
                        destinationSlot: 5,
                        requiresUserAction: false,
                    },
                ],
            },
        ];
        const secondPlan = {
            ...firstPlan,
            planToken: 'suggested-plan-2',
            planId: 'suggested-plan-2-id',
        };
        const planImagePackageImport = vi.fn().mockResolvedValueOnce(firstPlan).mockResolvedValueOnce(secondPlan);
        const picker = new PickerController(() => undefined);
        const workflow = new PackageBatchImportWorkflow({
            transport: {
                inspectPackage: vi.fn().mockResolvedValue(inspection('package-0', 'One', 1)),
                planImagePackageImport,
                releaseImagePackageImportPlan: vi.fn().mockResolvedValue(undefined),
            } as unknown as ImageTransport,
            jobs: {} as JobController,
            picker,
            pickerHistory: new PackagePickerHistory(),
            isDesktop: false,
            sessionId: () => 17,
            invalidateSession: vi.fn().mockResolvedValue(undefined),
            refreshSession: vi.fn().mockResolvedValue(undefined),
            setStatus: vi.fn(),
        });

        workflow.open({
            id: 'partition-0',
            name: 'Partition 1',
            kind: 'partition',
            childCount: 0,
            partitionIndex: 0,
        });
        const choosing = workflow.chooseWorkspace();
        picker.finish([source]);
        await choosing;
        await workflow.replan();

        expect(planImagePackageImport).toHaveBeenCalledTimes(2);
        expect(workflow.request).toMatchObject({
            plan: { planToken: 'suggested-plan-2', valid: false },
            status: 'ready',
            hasUnvalidatedChanges: false,
        });
    });

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

        expect(planImagePackageImport).not.toHaveBeenCalled();
        await workflow.replan();

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

    it('accumulates multiple object rename drafts before one explicit replan', async () => {
        const sources = [
            serverFileLocation({ rootId: 'workspace', relativePath: 'one.axkvol' }, 'one.axkvol'),
            serverFileLocation({ rootId: 'workspace', relativePath: 'two.axkvol' }, 'two.axkvol'),
        ];
        const planImagePackageImport = vi
            .fn()
            .mockResolvedValueOnce(plan(['One', 'Two'], 'initial-plan'))
            .mockResolvedValueOnce(plan(['One', 'Two'], 'renamed-plan'));
        const picker = new PickerController(() => undefined);
        const workflow = new PackageBatchImportWorkflow({
            transport: {
                inspectPackage: vi
                    .fn()
                    .mockResolvedValueOnce(inspection('package-0', 'One', 1))
                    .mockResolvedValueOnce(inspection('package-1', 'Two', 1)),
                planImagePackageImport,
                releaseImagePackageImportPlan: vi.fn().mockResolvedValue(undefined),
            } as unknown as ImageTransport,
            jobs: {} as JobController,
            picker,
            pickerHistory: new PackagePickerHistory(),
            isDesktop: false,
            sessionId: () => 17,
            invalidateSession: vi.fn().mockResolvedValue(undefined),
            refreshSession: vi.fn().mockResolvedValue(undefined),
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

        await workflow.replan();

        const [one, two] = workflow.request!.items;
        workflow.rename(one.id, 'sample-1', 'Kick fixed');
        workflow.rename(two.id, 'sample-2', 'Snare fixed');

        expect(workflow.request?.renames).toEqual({
            [`${one.id}:sample-1`]: 'Kick fixed',
            [`${two.id}:sample-2`]: 'Snare fixed',
        });
        expect(planImagePackageImport).toHaveBeenCalledTimes(1);

        await workflow.replan();

        expect(planImagePackageImport).toHaveBeenCalledTimes(2);
        expect(planImagePackageImport.mock.calls[1][3]).toEqual([
            { packageIndex: 0, nodeId: 'sample-1', destinationName: 'Kick fixed' },
            { packageIndex: 1, nodeId: 'sample-2', destinationName: 'Snare fixed' },
        ]);
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

        await workflow.replan();

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
        expect(setStatus).toHaveBeenCalledWith('Imported 2 packages');
    });

    it('keeps multiple volume packages targeted at the selected existing volume', async () => {
        const sources = [
            serverFileLocation({ rootId: 'workspace', relativePath: 'one.axkvol' }, 'one.axkvol'),
            serverFileLocation({ rootId: 'workspace', relativePath: 'two.axkvol' }, 'two.axkvol'),
        ];
        const planImagePackageImport = vi.fn().mockResolvedValue(plan(['Target', 'Target'], 'volume-plan'));
        const picker = new PickerController(() => undefined);
        const volume: DiskTreeItem = {
            id: 'volume-0',
            name: 'Target',
            kind: 'volume',
            childCount: 0,
            partitionIndex: 0,
        };
        const partition: DiskTreeItem = {
            id: 'partition-0',
            name: 'Partition 1',
            kind: 'partition',
            childCount: 1,
            partitionIndex: 0,
            children: [volume],
        };
        const workflow = new PackageBatchImportWorkflow({
            transport: {
                inspectPackage: vi
                    .fn()
                    .mockResolvedValueOnce(inspection('package-0', 'One', 1))
                    .mockResolvedValueOnce(inspection('package-1', 'Two', 1)),
                planImagePackageImport,
                releaseImagePackageImportPlan: vi.fn().mockResolvedValue(undefined),
            } as unknown as ImageTransport,
            jobs: {} as JobController,
            picker,
            pickerHistory: new PackagePickerHistory(),
            isDesktop: false,
            sessionId: () => 17,
            invalidateSession: vi.fn().mockResolvedValue(undefined),
            refreshSession: vi.fn().mockResolvedValue(undefined),
            setStatus: vi.fn(),
            sourceItems: () => [partition],
        });

        workflow.open(volume);
        const choosing = workflow.chooseWorkspace();
        picker.finish(sources);
        await choosing;

        expect(workflow.request).toMatchObject({
            destinationStrategy: 'shared',
            destinationMode: 'existing',
            destinationPartitionIndex: 0,
            destinationVolumeName: 'Target',
            plan: null,
            hasUnvalidatedChanges: true,
        });
        expect(workflow.canUseSeparateVolumes()).toBe(true);
        expect(planImagePackageImport).not.toHaveBeenCalled();

        await workflow.replan();

        expect(planImagePackageImport).toHaveBeenCalledWith(
            17,
            sources,
            { kind: 'EXISTING_VOLUME', partitionIndex: 0, volumeName: 'Target' },
            [],
            [],
            undefined,
            [],
        );
    });

    it('plans mixed package kinds into one selected existing volume', async () => {
        const sources = [
            serverFileLocation({ rootId: 'workspace', relativePath: 'one.axkprg' }, 'one.axkprg'),
            serverFileLocation({ rootId: 'workspace', relativePath: 'two.axksbac' }, 'two.axksbac'),
        ];
        const planImagePackageImport = vi.fn().mockResolvedValue(plan(['Target', 'Target'], 'mixed-plan'));
        const picker = new PickerController(() => undefined);
        const volume: DiskTreeItem = {
            id: 'volume-0',
            name: 'Target',
            kind: 'volume',
            childCount: 0,
            partitionIndex: 0,
        };
        const partition: DiskTreeItem = {
            id: 'partition-0',
            name: 'Partition 1',
            kind: 'partition',
            childCount: 1,
            partitionIndex: 0,
            children: [volume],
        };
        const workflow = new PackageBatchImportWorkflow({
            transport: {
                inspectPackage: vi
                    .fn()
                    .mockResolvedValueOnce(inspection('package-0', 'Program', 1, 'PROGRAM'))
                    .mockResolvedValueOnce(inspection('package-1', 'Bank', 0, 'SBAC')),
                planImagePackageImport,
                releaseImagePackageImportPlan: vi.fn().mockResolvedValue(undefined),
            } as unknown as ImageTransport,
            jobs: {} as JobController,
            picker,
            pickerHistory: new PackagePickerHistory(),
            isDesktop: false,
            sessionId: () => 17,
            invalidateSession: vi.fn().mockResolvedValue(undefined),
            refreshSession: vi.fn().mockResolvedValue(undefined),
            setStatus: vi.fn(),
            sourceItems: () => [partition],
        });

        workflow.open(volume);
        const choosing = workflow.chooseWorkspace();
        picker.finish(sources);
        await choosing;

        await workflow.replan();

        expect(workflow.request?.destinationStrategy).toBe('shared');
        expect(workflow.canUseSeparateVolumes()).toBe(false);
        expect(planImagePackageImport).toHaveBeenCalledWith(
            17,
            sources,
            { kind: 'EXISTING_VOLUME', partitionIndex: 0, volumeName: 'Target' },
            [],
            [],
            undefined,
            [],
        );
    });

    it('plans multiple portable packages into one explicitly named new volume', async () => {
        const sources = [
            serverFileLocation({ rootId: 'workspace', relativePath: 'one.axkprg' }, 'one.axkprg'),
            serverFileLocation({ rootId: 'workspace', relativePath: 'two.axksbnk' }, 'two.axksbnk'),
        ];
        const planImagePackageImport = vi.fn().mockResolvedValue(plan(['Combined', 'Combined'], 'shared-plan'));
        const picker = new PickerController(() => undefined);
        const partition: DiskTreeItem = {
            id: 'partition-0',
            name: 'Partition 1',
            kind: 'partition',
            childCount: 0,
            partitionIndex: 0,
        };
        const workflow = new PackageBatchImportWorkflow({
            transport: {
                inspectPackage: vi
                    .fn()
                    .mockResolvedValueOnce(inspection('package-0', 'Program', 1, 'PROGRAM'))
                    .mockResolvedValueOnce(inspection('package-1', 'Sample', 0, 'SBNK')),
                planImagePackageImport,
                releaseImagePackageImportPlan: vi.fn().mockResolvedValue(undefined),
            } as unknown as ImageTransport,
            jobs: {} as JobController,
            picker,
            pickerHistory: new PackagePickerHistory(),
            isDesktop: false,
            sessionId: () => 17,
            invalidateSession: vi.fn().mockResolvedValue(undefined),
            refreshSession: vi.fn().mockResolvedValue(undefined),
            setStatus: vi.fn(),
            sourceItems: () => [partition],
        });

        workflow.open(partition);
        const choosing = workflow.chooseWorkspace();
        picker.finish(sources);
        await choosing;

        expect(workflow.request?.destinationStrategy).toBe('shared');
        expect(workflow.request?.destinationMode).toBe('create');
        expect(workflow.canUseSeparateVolumes()).toBe(false);
        workflow.setDestinationVolumeName('Combined');
        await workflow.replan();

        expect(planImagePackageImport).toHaveBeenCalledTimes(1);
        expect(planImagePackageImport).toHaveBeenCalledWith(
            17,
            sources,
            { kind: 'CREATE_VOLUME', partitionIndex: 0, volumeName: 'Combined' },
            [],
            [],
            undefined,
            [],
        );
    });

    it('refreshes and waits for an explicit conflict check when the image changed before apply', async () => {
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
        await workflow.replan();
        await workflow.apply();

        expect(refreshSession).toHaveBeenCalledWith({ partitionIndex: 0 });
        expect(releaseImagePackageImportPlan).toHaveBeenCalledWith('stale-plan');
        expect(planImagePackageImport).toHaveBeenCalledTimes(1);
        expect(workflow.request).toMatchObject({
            plan: null,
            status: 'ready',
            hasUnvalidatedChanges: true,
        });
        expect(setStatus).toHaveBeenLastCalledWith('Image changed; check import conflicts again');

        await workflow.replan();

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
        await workflow.replan();
        await workflow.apply();

        expect(refreshSession).toHaveBeenCalledWith({ partitionIndex: 0, volumeName: 'One' });
        expect(workflow.request).toBeNull();
        expect(setStatus).toHaveBeenLastCalledWith(
            'Import completion could not be confirmed; review the refreshed image before retrying',
        );
    });
});
