import { beforeEach, describe, expect, it, vi } from 'vitest';
import type { ClientUploadSource } from '../../lib/clientUploadSource';
import { clientUploadLocation, serverFileLocation } from '../../lib/storageLocations';
import type { ImageSessionPackageImportPlan, ImageTransport, PackageInspection } from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import { PickerController, type PickerRequest } from '../dialogs/picker';
import type { JobController } from '../jobs/actions';
import { PackageImportWorkflow } from './packageWorkflow.svelte';

const nativeMocks = vi.hoisted(() => ({
    selectLocalPackage: vi.fn(),
    nativeFileSource: vi.fn(),
}));

vi.mock('../../lib/nativePackages', () => ({ selectLocalPackage: nativeMocks.selectLocalPackage }));
vi.mock('../../lib/nativeFileSource', () => ({ nativeFileSource: nativeMocks.nativeFileSource }));

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

function programPlan(
    applied: boolean,
    destinationStart = 5,
    planToken = applied ? 'checked-plan' : 'suggested-plan',
): ImageSessionPackageImportPlan {
    const mappings = Array.from({ length: 33 }, (_, index) => ({
        packageIndex: 0,
        nodeId: `program-${index + 1}`,
        sourceSlot: index + 1,
        destinationSlot: index + destinationStart,
        requiresUserAction: false,
    }));
    return {
        schemaVersion: '1.0',
        imageId: 'image-1',
        revision: 1,
        planToken,
        expiresInSeconds: 600,
        planId: `${planToken}-id`,
        targetKind: 'SFS',
        targetSnapshotId: 'snapshot-1',
        packages: [
            {
                packageIndex: 0,
                packageId: 'program-package',
                sourceVolumeName: 'Imported',
                destinationVolumeName: 'Imported',
                objectCount: mappings.length,
                payloadBytes: 0,
                objectCounts: { programs: mappings.length, sampleBanks: 0, samples: 0, waveData: 0, sequences: 0 },
            },
        ],
        valid: applied,
        warnings: [],
        opaqueSequences: [],
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
                suggestedStartSlot: destinationStart,
                requiredSlotCount: 33,
                availableSlotCount: 124,
                occupiedRanges: [{ first: 1, last: 4 }],
                sourceRanges: [{ first: 1, last: 33 }],
                destinationRanges: [{ first: destinationStart, last: destinationStart + 32 }],
                mappings,
            },
        ],
        allocation: [],
    };
}

describe('PackageImportWorkflow', () => {
    beforeEach(() => {
        nativeMocks.selectLocalPackage.mockReset();
        nativeMocks.nativeFileSource.mockReset();
    });

    it('checks compact Program suggestions automatically and supports a later manual recheck', async () => {
        const source = serverFileLocation(
            { rootId: 'workspace', relativePath: 'CosmoPad and others.axkprg' },
            'CosmoPad and others.axkprg',
        );
        const inspectPackage = vi.fn().mockResolvedValue(inspection);
        const planImagePackageImport = vi
            .fn()
            .mockResolvedValueOnce(programPlan(false))
            .mockResolvedValueOnce(programPlan(true))
            .mockResolvedValueOnce(programPlan(true, 9, 'rechecked-plan'));
        const transport = {
            inspectPackage,
            planImagePackageImport,
            releaseImagePackageImportPlan: vi.fn().mockResolvedValue(undefined),
        } as unknown as ImageTransport;
        const picker = new PickerController(() => undefined);
        const runJob = vi.fn();
        const workflow = new PackageImportWorkflow({
            transport,
            jobs: { run: runJob } as unknown as JobController,
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
        expect(planImagePackageImport).toHaveBeenNthCalledWith(
            1,
            17,
            [source],
            { kind: 'EXISTING_VOLUME', partitionIndex: 0, volumeName: 'Imported' },
            [],
            [],
            undefined,
            [],
        );
        expect(planImagePackageImport).toHaveBeenNthCalledWith(
            2,
            17,
            [source],
            { kind: 'EXISTING_VOLUME', partitionIndex: 0, volumeName: 'Imported' },
            [],
            Array.from({ length: 33 }, (_, index) => ({
                packageIndex: 0,
                nodeId: `program-${index + 1}`,
                destinationSlot: index + 5,
            })),
            'suggested-plan',
            [],
        );
        expect(workflow.request?.plan?.valid).toBe(true);
        expect(workflow.request?.renames).toEqual({});
        expect(workflow.request?.hasUnvalidatedChanges).toBe(false);

        workflow.programStart('placement-1', 9);
        expect(workflow.request?.hasUnvalidatedChanges).toBe(true);
        await workflow.apply();
        expect(runJob).not.toHaveBeenCalled();

        await workflow.replan();

        expect(planImagePackageImport).toHaveBeenNthCalledWith(
            3,
            17,
            [source],
            { kind: 'EXISTING_VOLUME', partitionIndex: 0, volumeName: 'Imported' },
            [],
            Array.from({ length: 33 }, (_, index) => ({
                packageIndex: 0,
                nodeId: `program-${index + 1}`,
                destinationSlot: index + 9,
            })),
            'checked-plan',
            [],
        );
        expect(workflow.request?.plan?.planToken).toBe('rechecked-plan');
        expect(workflow.request?.hasUnvalidatedChanges).toBe(false);
    });

    it('replans immediately with an explicit opaque Sequence decision', async () => {
        const source = serverFileLocation(
            { rootId: 'workspace', relativePath: 'NordMicroDrums.axkvol' },
            'NordMicroDrums.axkvol',
        );
        const undecidedPlan: ImageSessionPackageImportPlan = {
            ...programPlan(true, 5, 'opaque-plan'),
            valid: false,
            opaqueSequences: [{ packageIndex: 0, nodeId: 'sequence-1', name: 'NordMicroDrums', action: null }],
            conflicts: [
                {
                    code: 'OPAQUE_SEQUENCE_DECISION_REQUIRED',
                    message: 'opaque Sequence requires an explicit import decision',
                    nodeId: 'sequence-1',
                    packageId: 'program-package',
                    packageIndex: 0,
                    rootIndex: 0,
                    partitionIndex: 0,
                    groupName: '',
                    volumeName: 'Imported',
                    rawGroup: '',
                    rawVolume: '',
                },
            ],
            actions: [],
            programSlotPlacements: [],
        };
        const preservedPlan: ImageSessionPackageImportPlan = {
            ...undecidedPlan,
            planToken: 'preserved-plan',
            planId: 'preserved-plan-id',
            valid: true,
            opaqueSequences: [
                {
                    packageIndex: 0,
                    nodeId: 'sequence-1',
                    name: 'NordMicroDrums',
                    action: 'PRESERVE_UNCHANGED',
                },
            ],
            conflicts: [],
        };
        const planImagePackageImport = vi
            .fn()
            .mockResolvedValueOnce(undecidedPlan)
            .mockResolvedValueOnce(preservedPlan);
        const picker = new PickerController(() => undefined);
        const workflow = new PackageImportWorkflow({
            transport: {
                inspectPackage: vi.fn().mockResolvedValue(inspection),
                planImagePackageImport,
                releaseImagePackageImportPlan: vi.fn().mockResolvedValue(undefined),
            } as unknown as ImageTransport,
            jobs: {} as JobController,
            picker,
            isDesktop: false,
            sessionId: () => 17,
            invalidateSession: vi.fn(),
            refreshSession: vi.fn(),
            setStatus: vi.fn(),
        });

        workflow.open({
            id: 'volume-imported',
            name: 'Imported',
            kind: 'volume',
            childCount: 0,
            partitionIndex: 0,
        });
        const choosing = workflow.chooseWorkspace();
        picker.finish(source);
        await choosing;

        expect(workflow.request?.plan?.valid).toBe(false);
        expect(workflow.request?.opaqueSequenceActions).toEqual({});

        workflow.opaqueSequenceAction('sequence-1', 'PRESERVE_UNCHANGED');

        await vi.waitFor(() => expect(workflow.request?.plan?.planToken).toBe('preserved-plan'));
        expect(planImagePackageImport).toHaveBeenNthCalledWith(
            2,
            17,
            [source],
            { kind: 'EXISTING_VOLUME', partitionIndex: 0, volumeName: 'Imported' },
            [],
            [],
            'opaque-plan',
            [{ packageIndex: 0, nodeId: 'sequence-1', action: 'PRESERVE_UNCHANGED' }],
        );
        expect(workflow.request?.hasUnvalidatedChanges).toBe(false);
        expect(workflow.request?.plan?.valid).toBe(true);
    });

    it('retains an automatic Program slot assignment when skipping an opaque Sequence', async () => {
        const source = serverFileLocation(
            { rootId: 'workspace', relativePath: 'NordMicroDrums.axkvol' },
            'NordMicroDrums.axkvol',
        );
        const opaqueConflict = {
            code: 'OPAQUE_SEQUENCE_DECISION_REQUIRED',
            message: 'opaque Sequence requires an explicit import decision',
            nodeId: 'sequence-1',
            packageId: 'program-package',
            packageIndex: 0,
            rootIndex: 0,
            partitionIndex: 0,
            groupName: '',
            volumeName: 'Imported',
            rawGroup: '',
            rawVolume: '',
        };
        const initialPlan: ImageSessionPackageImportPlan = {
            ...programPlan(false, 5, 'initial-plan'),
            opaqueSequences: [{ packageIndex: 0, nodeId: 'sequence-1', name: 'NordMicroDrums', action: null }],
            conflicts: [...programPlan(false, 5).conflicts, opaqueConflict],
        };
        const checkedPlan: ImageSessionPackageImportPlan = {
            ...programPlan(true, 5, 'checked-plan'),
            valid: false,
            opaqueSequences: [{ packageIndex: 0, nodeId: 'sequence-1', name: 'NordMicroDrums', action: null }],
            conflicts: [opaqueConflict],
        };
        const skippedPlan: ImageSessionPackageImportPlan = {
            ...programPlan(true, 5, 'skipped-plan'),
            opaqueSequences: [{ packageIndex: 0, nodeId: 'sequence-1', name: 'NordMicroDrums', action: 'SKIP' }],
        };
        const planImagePackageImport = vi
            .fn()
            .mockResolvedValueOnce(initialPlan)
            .mockResolvedValueOnce(checkedPlan)
            .mockResolvedValueOnce(skippedPlan);
        const picker = new PickerController(() => undefined);
        const workflow = new PackageImportWorkflow({
            transport: {
                inspectPackage: vi.fn().mockResolvedValue(inspection),
                planImagePackageImport,
                releaseImagePackageImportPlan: vi.fn().mockResolvedValue(undefined),
            } as unknown as ImageTransport,
            jobs: {} as JobController,
            picker,
            isDesktop: false,
            sessionId: () => 17,
            invalidateSession: vi.fn(),
            refreshSession: vi.fn(),
            setStatus: vi.fn(),
        });

        workflow.open({
            id: 'volume-imported',
            name: 'Imported',
            kind: 'volume',
            childCount: 109,
            partitionIndex: 0,
        });
        const choosing = workflow.chooseWorkspace();
        picker.finish(source);
        await choosing;

        expect(workflow.request?.plan?.planToken).toBe('checked-plan');
        expect(workflow.request?.plan?.valid).toBe(false);

        workflow.opaqueSequenceAction('sequence-1', 'SKIP');

        await vi.waitFor(() => expect(workflow.request?.plan?.planToken).toBe('skipped-plan'));
        expect(planImagePackageImport).toHaveBeenNthCalledWith(
            3,
            17,
            [source],
            { kind: 'EXISTING_VOLUME', partitionIndex: 0, volumeName: 'Imported' },
            [],
            Array.from({ length: 33 }, (_, index) => ({
                packageIndex: 0,
                nodeId: `program-${index + 1}`,
                destinationSlot: index + 5,
            })),
            'checked-plan',
            [{ packageIndex: 0, nodeId: 'sequence-1', action: 'SKIP' }],
        );
        expect(workflow.request?.hasUnvalidatedChanges).toBe(false);
        expect(workflow.request?.plan?.valid).toBe(true);
    });

    it('preserves unchecked Program slots when a manual conflict check fails', async () => {
        const source = serverFileLocation({ rootId: 'workspace', relativePath: 'programs.axkprg' }, 'programs.axkprg');
        const planImagePackageImport = vi
            .fn()
            .mockResolvedValueOnce(programPlan(false))
            .mockResolvedValueOnce(programPlan(true))
            .mockRejectedValueOnce(new Error('conflict check failed'));
        const transport = {
            inspectPackage: vi.fn().mockResolvedValue(inspection),
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

        workflow.open({
            id: 'volume-imported',
            name: 'Imported',
            kind: 'volume',
            childCount: 4,
            partitionIndex: 0,
        });
        const choosing = workflow.chooseWorkspace();
        picker.finish(source);
        await choosing;
        workflow.programStart('placement-1', 9);

        await workflow.replan();

        expect(workflow.request?.status).toBe('ready');
        expect(workflow.request?.plan?.planToken).toBe('checked-plan');
        expect(workflow.request?.programSlots['program-1']).toBe(9);
        expect(workflow.request?.hasUnvalidatedChanges).toBe(true);
        expect(workflow.request?.error).toBe('Conflict check failed');
    });

    it('limits automatic Program slot checks to one follow-up plan', async () => {
        const source = serverFileLocation({ rootId: 'workspace', relativePath: 'programs.axkprg' }, 'programs.axkprg');
        const planImagePackageImport = vi
            .fn()
            .mockResolvedValueOnce(programPlan(false, 5, 'suggested-plan-1'))
            .mockResolvedValueOnce(programPlan(false, 5, 'suggested-plan-2'));
        const transport = {
            inspectPackage: vi.fn().mockResolvedValue(inspection),
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

        workflow.open({
            id: 'volume-imported',
            name: 'Imported',
            kind: 'volume',
            childCount: 4,
            partitionIndex: 0,
        });
        const choosing = workflow.chooseWorkspace();
        picker.finish(source);
        await choosing;

        expect(planImagePackageImport).toHaveBeenCalledTimes(2);
        expect(workflow.request?.status).toBe('ready');
        expect(workflow.request?.plan?.planToken).toBe('suggested-plan-2');
        expect(workflow.request?.plan?.valid).toBe(false);
        expect(workflow.request?.hasUnvalidatedChanges).toBe(false);
    });

    it('restores the workspace directory and file from the last completed package import', async () => {
        const source = serverFileLocation(
            { rootId: 'workspace', relativePath: 'packages/Imported.axkvol' },
            'Imported.axkvol',
        );
        const transport = {
            inspectPackage: vi.fn().mockResolvedValue(inspection),
            planImagePackageImport: vi
                .fn()
                .mockResolvedValueOnce(programPlan(false))
                .mockResolvedValueOnce(programPlan(true)),
            releaseImagePackageImportPlan: vi.fn().mockResolvedValue(undefined),
        } as unknown as ImageTransport;
        const pickerRequests: PickerRequest[] = [];
        const picker = new PickerController((request) => {
            if (request) pickerRequests.push(request);
        });
        const workflow = new PackageImportWorkflow({
            transport,
            jobs: {
                run: vi.fn().mockResolvedValue({ jobId: 1, kind: 'package-import', status: 'completed', result: {} }),
            } as unknown as JobController,
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
        pickerRequests[0]!.ondirectorychange?.({ rootId: 'workspace', relativePath: 'packages' });
        picker.finish(source);
        await choosing;
        await workflow.apply();

        workflow.open(target);
        const choosingAgain = workflow.chooseWorkspace();

        expect(pickerRequests.at(-1)?.initialDirectory).toEqual({ rootId: 'workspace', relativePath: 'packages' });
        expect(pickerRequests.at(-1)?.initialFile).toEqual(source.reference);
        picker.finish(null);
        await choosingAgain;
    });

    it('does not replace the remembered workspace file when a later import fails', async () => {
        const successfulSource = serverFileLocation(
            { rootId: 'workspace', relativePath: 'packages/Working.axkvol' },
            'Working.axkvol',
        );
        const failedSource = serverFileLocation(
            { rootId: 'workspace', relativePath: 'packages/Broken.axkvol' },
            'Broken.axkvol',
        );
        const transport = {
            inspectPackage: vi.fn().mockResolvedValue(inspection),
            planImagePackageImport: vi
                .fn()
                .mockResolvedValueOnce(programPlan(false))
                .mockResolvedValueOnce(programPlan(true))
                .mockResolvedValueOnce(programPlan(false, 5, 'failed-suggestion'))
                .mockResolvedValueOnce(programPlan(true, 5, 'failed-plan')),
            releaseImagePackageImportPlan: vi.fn().mockResolvedValue(undefined),
        } as unknown as ImageTransport;
        const pickerRequests: PickerRequest[] = [];
        const picker = new PickerController((request) => {
            if (request) pickerRequests.push(request);
        });
        const run = vi
            .fn()
            .mockResolvedValueOnce({ jobId: 1, kind: 'package-import', status: 'completed', result: {} })
            .mockResolvedValueOnce({ jobId: 2, kind: 'package-import', status: 'failed', error: 'write failed' });
        const workflow = new PackageImportWorkflow({
            transport,
            jobs: { run } as unknown as JobController,
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
        let choosing = workflow.chooseWorkspace();
        pickerRequests[0]!.ondirectorychange?.({ rootId: 'workspace', relativePath: 'packages' });
        picker.finish(successfulSource);
        await choosing;
        await workflow.apply();

        workflow.open(target);
        choosing = workflow.chooseWorkspace();
        picker.finish(failedSource);
        await choosing;
        await workflow.apply();
        await workflow.close();

        workflow.open(target);
        choosing = workflow.chooseWorkspace();
        expect(pickerRequests.at(-1)?.initialFile).toEqual(successfulSource.reference);
        picker.finish(null);
        await choosing;
    });

    it('passes the last successfully imported native package to the next native chooser', async () => {
        const localPath = 'previous-package.axkvol';
        const upload = clientUploadLocation({ uploadId: 'upload-1' }, 'PACKAGE', 'Imported.axkvol');
        const localSource: ClientUploadSource = {
            name: 'Imported.axkvol',
            type: 'application/octet-stream',
            size: 1024,
            readChunk: vi.fn(),
        };
        nativeMocks.selectLocalPackage.mockResolvedValueOnce(localPath).mockResolvedValueOnce(null);
        nativeMocks.nativeFileSource.mockResolvedValue(localSource);
        const transport = {
            uploadClientFile: vi.fn().mockResolvedValue(upload),
            inspectPackage: vi.fn().mockResolvedValue(inspection),
            planImagePackageImport: vi
                .fn()
                .mockResolvedValueOnce(programPlan(false))
                .mockResolvedValueOnce(programPlan(true)),
            releaseImagePackageImportPlan: vi.fn().mockResolvedValue(undefined),
            releaseClientUpload: vi.fn().mockResolvedValue(undefined),
        } as unknown as ImageTransport;
        const workflow = new PackageImportWorkflow({
            transport,
            jobs: {
                run: vi.fn().mockResolvedValue({ jobId: 1, kind: 'package-import', status: 'completed', result: {} }),
            } as unknown as JobController,
            picker: new PickerController(() => undefined),
            isDesktop: true,
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
        await workflow.chooseLocal();
        await workflow.apply();
        workflow.open(target);
        await workflow.chooseLocal();

        expect(nativeMocks.selectLocalPackage).toHaveBeenNthCalledWith(1, null);
        expect(nativeMocks.selectLocalPackage).toHaveBeenNthCalledWith(2, localPath);
    });
});
