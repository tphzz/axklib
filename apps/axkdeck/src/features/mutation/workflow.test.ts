import { describe, expect, it, vi } from 'vitest';

import type { ImageTransport, PlacementRepairInspection } from '../../lib/transport';
import type { SampleStructureItem } from '../../lib/types';
import { MutationWorkflow } from './workflow.svelte';

const volume = {
    id: 'volume-1',
    name: 'Samples',
    kind: 'volume' as const,
    childCount: 2,
    partitionIndex: 0,
};

const partition = {
    id: 'partition-0',
    name: 'Partition 1',
    kind: 'partition' as const,
    childCount: 2,
    partitionIndex: 0,
};

function workflowWith(transport: Partial<ImageTransport>, catalog: object = {}) {
    const run = vi.fn().mockImplementation(async (start: () => Promise<unknown>) => {
        await start();
        return { status: 'completed' };
    });
    const refreshSession = vi.fn().mockResolvedValue(undefined);
    const setWorkspaceView = vi.fn();
    const clearSelection = vi.fn();
    const workflow = new MutationWorkflow({
        transport: transport as ImageTransport,
        jobs: { run } as never,
        catalog: catalog as never,
        audition: { invalidateSession: vi.fn().mockResolvedValue(undefined) } as never,
        sessionId: () => 7,
        imageOpen: () => true,
        workspaceView: () => 'samples',
        setWorkspaceView,
        clearSelection,
        refreshSession,
        setStatus: vi.fn(),
        reportTiming: vi.fn(),
    });
    workflow.setCapabilities({
        volumeMutationsAvailable: true,
        partitionMutationsAvailable: true,
        objectRenameAvailable: false,
    });
    return { workflow, run, refreshSession, setWorkspaceView, clearSelection };
}

function placementInspection(overrides: Partial<PlacementRepairInspection> = {}): PlacementRepairInspection {
    return {
        imageId: 'image-1',
        revision: 1,
        scope: { kind: 'VOLUME', partitionIndex: 0, volumeName: 'Samples' },
        canRepair: true,
        repairObjectCount: 1,
        blockedObjectCount: 0,
        destinations: [
            {
                volumeName: 'Samples',
                createsVolume: false,
                objectCount: 1,
                objectTypeCounts: { SBNK: 1 },
            },
        ],
        blockers: [],
        ...overrides,
    };
}

describe('MutationWorkflow', () => {
    it('creates a Sample Bank from selected Samples in the supplied order', async () => {
        const sample = (name: string): SampleStructureItem => ({
            id: `sample-${name}`,
            objectId: `sample-${name}`,
            name,
            objectType: 'SBNK',
            sampleBankObjectIds: name === 'Sample 2' ? ['bank-existing'] : [],
            object: {
                key: `sample-${name}`,
                objectType: 'SBNK',
                name,
                partitionIndex: 0,
                partitionName: 'Partition 1',
                volumeName: 'Samples',
                categoryName: 'SBNK',
                objectEncoding: 'current',
                directoryEntryName: `${name}.001`,
                sfsId: 1,
                storedSizeBytes: 512,
                sizeWithDependenciesBytes: null,
                sampleRate: 44_100,
                rootKey: 60,
                storedFrameCount: 1,
                waveStartFrame: 0,
                waveLengthFrames: 1,
                storageState: 'COMPLETE' as const,
                sampleWidthBytes: 2,
            },
        });
        const samples = [sample('Sample 2'), sample('Sample 10')];
        const catalog = {
            sampleBanks: [],
            programs: [],
            relationships: [],
            membersForBank: () => [],
            selectedBankId: '',
            inspectorObjectId: '',
            editorObjectIds: { 'sample-banks': '' },
        };
        const startSampleBankCreation = vi.fn().mockResolvedValue({ jobId: 1, status: 'queued' });
        const { workflow, clearSelection, setWorkspaceView } = workflowWith({ startSampleBankCreation }, catalog);
        workflow.setCapabilities({
            volumeMutationsAvailable: true,
            partitionMutationsAvailable: true,
            objectRenameAvailable: true,
        });

        workflow.requestSampleBankAssignment(samples);
        expect(workflow.sampleBankAssignmentRequest?.samples).toEqual(samples);
        expect(workflow.sampleBankAssignmentRequest?.options).toEqual([]);
        await workflow.submitSampleBankAssignment({ mode: 'new', name: 'Layered' });

        expect(startSampleBankCreation).toHaveBeenCalledWith(7, {
            partitionIndex: 0,
            volumeName: 'Samples',
            sampleBankName: 'Layered',
            sampleNames: ['Sample 2', 'Sample 10'],
        });
        expect(setWorkspaceView).toHaveBeenCalledWith('sample-banks');
        expect(clearSelection).toHaveBeenCalledOnce();
        expect(workflow.sampleBankAssignmentRequest).toBeNull();
    });

    it('assigns selected Samples to an existing Sample Bank while retaining existing members', async () => {
        const sample = (name: string, sampleBankObjectIds: string[] = []): SampleStructureItem => ({
            id: `sample-${name}`,
            objectId: `sample-${name}`,
            name,
            objectType: 'SBNK',
            sampleBankObjectIds,
            object: {
                key: `sample-${name}`,
                objectType: 'SBNK',
                name,
                partitionIndex: 0,
                partitionName: 'Partition 1',
                volumeName: 'Samples',
                categoryName: 'SBNK',
                objectEncoding: 'current',
                directoryEntryName: `${name}.001`,
                sfsId: 1,
                storedSizeBytes: 512,
                sizeWithDependenciesBytes: null,
                sampleRate: 44_100,
                rootKey: 60,
                storedFrameCount: 1,
                waveStartFrame: 0,
                waveLengthFrames: 1,
                storageState: 'COMPLETE' as const,
                sampleWidthBytes: 2,
            },
        });
        const target = sample('Layered') as SampleStructureItem;
        target.objectType = 'SBAC';
        target.object.objectType = 'SBAC';
        target.objectId = 'bank-layered';
        target.object.key = 'bank-layered';
        target.memberCount = 2;
        const other = sample('Other Bank') as SampleStructureItem;
        other.objectType = 'SBAC';
        other.object.objectType = 'SBAC';
        other.objectId = 'bank-other';
        other.object.key = 'bank-other';
        const retained = sample('Retained', ['bank-layered']);
        const moved = sample('Moved', ['bank-other']);
        const catalog = {
            sampleBanks: [other, target],
            programs: [],
            relationships: [],
            membersForBank: (bankId: string) => (bankId === 'bank-layered' ? [retained, sample('Existing')] : [moved]),
            selectedBankId: '',
            inspectorObjectId: '',
            editorObjectIds: { 'sample-banks': '' },
        };
        const startSampleBankAssignment = vi.fn().mockResolvedValue({ jobId: 1, status: 'queued' });
        const { workflow, clearSelection, setWorkspaceView } = workflowWith({ startSampleBankAssignment }, catalog);
        workflow.setCapabilities({
            volumeMutationsAvailable: true,
            partitionMutationsAvailable: true,
            objectRenameAvailable: true,
        });

        workflow.requestSampleBankAssignment([moved, retained]);
        expect(workflow.sampleBankAssignmentRequest?.options).toEqual(
            expect.arrayContaining([
                expect.objectContaining({
                    objectId: 'bank-layered',
                    memberCount: 2,
                    selectedMemberCount: 1,
                    movedSampleCount: 1,
                    reassignedSampleCount: 1,
                    finalMemberCount: 3,
                }),
            ]),
        );
        await workflow.submitSampleBankAssignment({ mode: 'existing', bankObjectId: 'bank-layered' });

        expect(startSampleBankAssignment).toHaveBeenCalledWith(7, {
            partitionIndex: 0,
            volumeName: 'Samples',
            sampleBankName: 'Layered',
            sampleNames: ['Moved', 'Retained'],
        });
        expect(setWorkspaceView).toHaveBeenCalledWith('sample-banks');
        expect(catalog.selectedBankId).toBe('bank-layered');
        expect(clearSelection).toHaveBeenCalledOnce();
        expect(workflow.sampleBankAssignmentRequest).toBeNull();
    });

    it('blocks new and existing Sample Bank targets when a selected Sample is assigned directly to a Program', async () => {
        const selected = {
            id: 'sample-direct',
            objectId: 'sample-direct',
            name: 'Direct Sample',
            objectType: 'SBNK' as const,
            sampleBankObjectIds: [],
            object: {
                key: 'sample-direct',
                objectType: 'SBNK',
                name: 'Direct Sample',
                partitionIndex: 0,
                partitionName: 'Partition 1',
                volumeName: 'Samples',
                categoryName: 'SBNK',
                objectEncoding: 'current',
                directoryEntryName: 'DIRECT.001',
                sfsId: 1,
                storedSizeBytes: 512,
                sizeWithDependenciesBytes: null,
                sampleRate: 44_100,
                rootKey: 60,
                storedFrameCount: 1,
                waveStartFrame: 0,
                waveLengthFrames: 1,
                storageState: 'COMPLETE' as const,
                sampleWidthBytes: 2,
            },
        };
        const bank = {
            ...selected,
            id: 'bank-1',
            objectId: 'bank-1',
            name: 'Bank 1',
            objectType: 'SBAC' as const,
            object: { ...selected.object, key: 'bank-1', objectType: 'SBAC', name: 'Bank 1' },
        };
        const catalog = {
            sampleBanks: [bank],
            programs: [{ objectId: 'program-1', name: 'Lead', slot: '001' }],
            relationships: [
                {
                    sourceObjectId: 'program-1',
                    targetObjectId: 'sample-direct',
                    relationshipType: 'PROG_ASSIGNMENT_TO_SBNK',
                    quality: 'KNOWN',
                    assignmentState: 'stored-assignment',
                },
            ],
            membersForBank: () => [],
        };
        const startSampleBankCreation = vi.fn();
        const startSampleBankAssignment = vi.fn();
        const { workflow } = workflowWith({ startSampleBankCreation, startSampleBankAssignment }, catalog);
        workflow.setCapabilities({
            volumeMutationsAvailable: true,
            partitionMutationsAvailable: true,
            objectRenameAvailable: true,
        });

        workflow.requestSampleBankAssignment([selected]);

        expect(workflow.sampleBankAssignmentRequest?.blockers).toEqual([
            { sampleName: 'Direct Sample', programName: '001: Lead' },
        ]);
        await workflow.submitSampleBankAssignment({ mode: 'new', name: 'New Bank' });
        await workflow.submitSampleBankAssignment({ mode: 'existing', bankObjectId: 'bank-1' });
        expect(startSampleBankCreation).not.toHaveBeenCalled();
        expect(startSampleBankAssignment).not.toHaveBeenCalled();
    });

    it('does not block Sample Bank assignment for an unresolved Program row', () => {
        const selected = {
            id: 'sample-inactive',
            objectId: 'sample-inactive',
            name: 'Inactive Sample',
            objectType: 'SBNK' as const,
            sampleBankObjectIds: [],
            object: {
                key: 'sample-inactive',
                objectType: 'SBNK',
                name: 'Inactive Sample',
                partitionIndex: 0,
                partitionName: 'Partition 1',
                volumeName: 'Samples',
                categoryName: 'SBNK',
                objectEncoding: 'current',
                directoryEntryName: 'INACTIVE.001',
                sfsId: 1,
                storedSizeBytes: 512,
                sizeWithDependenciesBytes: null,
                sampleRate: 44_100,
                rootKey: 60,
                storedFrameCount: 1,
                waveStartFrame: 0,
                waveLengthFrames: 1,
                storageState: 'COMPLETE' as const,
                sampleWidthBytes: 2,
            },
        };
        const bank = {
            ...selected,
            id: 'bank-1',
            objectId: 'bank-1',
            name: 'Bank 1',
            objectType: 'SBAC' as const,
            object: { ...selected.object, key: 'bank-1', objectType: 'SBAC', name: 'Bank 1' },
        };
        const catalog = {
            sampleBanks: [bank],
            programs: [{ objectId: 'program-1', name: 'Lead', slot: '001' }],
            relationships: [
                {
                    sourceObjectId: 'program-1',
                    targetObjectId: 'sample-inactive',
                    relationshipType: 'PROG_ASSIGNMENT_TO_SBNK',
                    quality: 'KNOWN',
                    assignmentState: 'unknown',
                },
            ],
            membersForBank: () => [],
        };
        const { workflow } = workflowWith({}, catalog);
        workflow.setCapabilities({
            volumeMutationsAvailable: true,
            partitionMutationsAvailable: true,
            objectRenameAvailable: true,
        });

        workflow.requestSampleBankAssignment([selected]);

        expect(workflow.sampleBankAssignmentRequest?.blockers).toEqual([]);
    });

    it('keeps deletion blocked and does not implicitly repair placement', async () => {
        const inspectVolumeDeletion = vi.fn().mockResolvedValue({
            imageId: 'image-1',
            revision: 1,
            targets: [{ partitionIndex: 0, volumeName: 'Samples' }],
            canDelete: false,
            crossingRelationshipCount: 1,
            blockers: [{ code: 'KNOWN_RELATIONSHIP_CROSSES_VOLUME', message: 'Crossing link', count: 1 }],
        });
        const startVolumeMutations = vi.fn();
        const { workflow } = workflowWith({ inspectVolumeDeletion, startVolumeMutations });

        expect(workflow.requestVolumeAction(volume, 'delete-volume')).toBe(true);
        await vi.waitFor(() => expect(workflow.volumeDeletionInspection?.canDelete).toBe(false));
        await workflow.submitVolumeAction('Samples');

        expect(startVolumeMutations).not.toHaveBeenCalled();
        expect(workflow.placementRepairRequest).toBeNull();
    });

    it('deletes multiple volumes in one alteration job after inspecting their union', async () => {
        const secondVolume = { ...volume, id: 'volume-2', name: 'Programs', partitionIndex: 2 };
        const targets = [
            { partitionIndex: 0, volumeName: 'Samples' },
            { partitionIndex: 2, volumeName: 'Programs' },
        ];
        const inspectVolumeDeletion = vi.fn().mockResolvedValue({
            imageId: 'image-1',
            revision: 1,
            targets,
            canDelete: true,
            crossingRelationshipCount: 0,
            blockers: [],
        });
        const startVolumeMutations = vi.fn().mockResolvedValue({ jobId: 1, status: 'queued' });
        const { workflow, refreshSession } = workflowWith({ inspectVolumeDeletion, startVolumeMutations });

        expect(workflow.requestVolumeDeletion([volume, secondVolume])).toBe(true);
        await vi.waitFor(() => expect(workflow.volumeDeletionInspection?.canDelete).toBe(true));
        await workflow.submitVolumeAction('');

        expect(inspectVolumeDeletion).toHaveBeenCalledWith(7, targets);
        expect(startVolumeMutations).toHaveBeenCalledWith(7, [
            { kind: 'delete', partitionIndex: 0, volumeName: 'Samples' },
            { kind: 'delete', partitionIndex: 2, volumeName: 'Programs' },
        ]);
        expect(refreshSession).toHaveBeenCalledWith({ partitionIndex: 0, volumeName: undefined });
    });

    it('rejects duplicate deletion targets even when their tree ids differ', () => {
        const duplicateVolume = { ...volume, id: 'duplicate-volume' };
        const { workflow } = workflowWith({});

        expect(workflow.requestVolumeDeletion([volume, duplicateVolume])).toBe(false);
        expect(workflow.volumeAction).toBeNull();
    });

    it('repairs a uniquely attributable volume placement through the explicit action', async () => {
        const inspectPlacement = vi
            .fn()
            .mockResolvedValueOnce(placementInspection())
            .mockResolvedValueOnce(
                placementInspection({
                    revision: 2,
                    canRepair: false,
                    repairObjectCount: 0,
                    destinations: [],
                }),
            );
        const startPlacementRepair = vi.fn().mockResolvedValue({ jobId: 1, status: 'queued' });
        const { workflow, refreshSession } = workflowWith({ inspectPlacement, startPlacementRepair });

        expect(workflow.requestVolumeAction(volume, 'repair-placement')).toBe(true);
        await vi.waitFor(() => expect(workflow.placementRepairRequest?.inspection?.repairObjectCount).toBe(1));
        await workflow.submitPlacementRepair();

        const scope = { kind: 'VOLUME', partitionIndex: 0, volumeName: 'Samples' };
        expect(startPlacementRepair).toHaveBeenCalledWith(7, scope, undefined);
        expect(refreshSession).toHaveBeenCalledWith({ partitionIndex: 0, volumeName: 'Samples' });
        expect(inspectPlacement).toHaveBeenLastCalledWith(7, scope);
        expect(workflow.placementRepairRequest?.message).toBe('Repaired placement for 1 object.');
    });

    it('creates a named recovery volume for ownerless partition objects', async () => {
        const initial = placementInspection({
            scope: { kind: 'PARTITION', partitionIndex: 0 },
            recoveryVolumeName: 'Recovered',
            repairObjectCount: 62,
            destinations: [
                {
                    volumeName: 'Recovered',
                    createsVolume: true,
                    objectCount: 62,
                    objectTypeCounts: { SMPL: 62 },
                },
            ],
        });
        const inspectPlacement = vi
            .fn()
            .mockResolvedValueOnce(initial)
            .mockResolvedValueOnce(
                placementInspection({
                    scope: { kind: 'PARTITION', partitionIndex: 0 },
                    canRepair: false,
                    repairObjectCount: 0,
                    destinations: [],
                }),
            );
        const startPlacementRepair = vi.fn().mockResolvedValue({ jobId: 1, status: 'queued' });
        const { workflow, refreshSession } = workflowWith({ inspectPlacement, startPlacementRepair });

        expect(workflow.requestVolumeAction(partition, 'repair-placement')).toBe(true);
        await vi.waitFor(() => expect(workflow.placementRepairRequest?.inspection).toEqual(initial));
        await workflow.submitPlacementRepair('Recovered Waves');

        const scope = { kind: 'PARTITION', partitionIndex: 0 };
        expect(startPlacementRepair).toHaveBeenCalledWith(7, scope, 'Recovered Waves');
        expect(refreshSession).toHaveBeenCalledWith({ partitionIndex: 0, volumeName: 'Recovered Waves' });
    });
});
