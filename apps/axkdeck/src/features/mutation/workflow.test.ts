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
                sfsId: 1,
                storedSizeBytes: 512,
                sampleRate: 44_100,
                rootKey: 60,
                frameCount: 1,
                sampleWidthBytes: 2,
            },
        });
        const samples = [sample('Sample 2'), sample('Sample 10')];
        const catalog = {
            sampleBanks: [],
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

        workflow.requestSampleBankCreation(samples);
        expect(workflow.sampleBankCreationRequest?.samples).toEqual(samples);
        await workflow.submitSampleBankCreation('Layered');

        expect(startSampleBankCreation).toHaveBeenCalledWith(7, {
            partitionIndex: 0,
            volumeName: 'Samples',
            sampleBankName: 'Layered',
            sampleNames: ['Sample 2', 'Sample 10'],
        });
        expect(setWorkspaceView).toHaveBeenCalledWith('sample-banks');
        expect(clearSelection).toHaveBeenCalledOnce();
        expect(workflow.sampleBankCreationRequest).toBeNull();
    });

    it('keeps deletion blocked and does not implicitly repair placement', async () => {
        const inspectVolumeDeletion = vi.fn().mockResolvedValue({
            imageId: 'image-1',
            revision: 1,
            partitionIndex: 0,
            volumeName: 'Samples',
            canDelete: false,
            crossingRelationshipCount: 1,
            blockers: [{ code: 'KNOWN_RELATIONSHIP_CROSSES_VOLUME', message: 'Crossing link', count: 1 }],
        });
        const startVolumeMutation = vi.fn();
        const { workflow } = workflowWith({ inspectVolumeDeletion, startVolumeMutation });

        expect(workflow.requestVolumeAction(volume, 'delete-volume')).toBe(true);
        await vi.waitFor(() => expect(workflow.volumeDeletionInspection?.canDelete).toBe(false));
        await workflow.submitVolumeAction('Samples');

        expect(startVolumeMutation).not.toHaveBeenCalled();
        expect(workflow.placementRepairRequest).toBeNull();
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
