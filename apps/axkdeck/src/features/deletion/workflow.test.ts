import { describe, expect, it, vi } from 'vitest';
import type { ImageTransport, ObjectDeletionInspection } from '../../lib/transport';
import type { DiskTreeItem, PackageExportObject } from '../../lib/types';
import { JobController } from '../jobs/actions';
import { DeletionWorkflow } from './workflow.svelte';

const target: PackageExportObject = {
    kind: 'PROGRAM',
    objectId: 'program-8',
    name: '008: 909 HH',
    typeLabel: 'Program',
    partitionIndex: 1,
    partitionName: 'PARTITION 2',
    volumeName: 'importtest',
};
const volume: DiskTreeItem = {
    id: 'volume-1',
    name: 'importtest',
    kind: 'volume',
    childCount: 1,
    partitionIndex: 1,
};
const blockerMessage = "Sample Bank 'DX PER' stores Program 008 but is assigned by Program 021";

function inspection(blocked = false): ObjectDeletionInspection {
    return {
        canApply: !blocked,
        imageId: 'image-1',
        revision: 7,
        targetObjectIds: [target.objectId],
        referrerObjectIds: [],
        cleanupObjectIds: [],
        selectedObjectIds: [target.objectId],
        impacts: [
            {
                objectId: target.objectId,
                objectType: 'PROG',
                objectName: target.name,
                partitionIndex: 1,
                partitionName: target.partitionName,
                volumeName: target.volumeName,
                role: 'TARGET',
                status: blocked ? 'BLOCKED' : 'REQUIRED',
                requested: true,
                selected: true,
                storedSizeBytes: 2048,
                freedClusters: blocked ? 0 : 1,
                prerequisiteObjectIds: [],
                reason: blocked ? blockerMessage : '',
            },
        ],
        references: [],
        blockers: blocked
            ? [{ code: 'PROGRAM_LINKS_INCONSISTENT', message: blockerMessage, objectIds: [target.objectId, 'bank-21'] }]
            : [],
        warnings: [],
        estimatedFreedBytes: blocked ? 0 : 2048,
        estimatedFreedClusters: blocked ? 0 : 1,
    };
}

function createWorkflow() {
    const inspectObjectDeletion = vi.fn().mockResolvedValue(inspection());
    const startObjectDeletion = vi.fn().mockResolvedValue({ jobId: 41, status: 'queued' });
    const waitForJob = vi.fn().mockResolvedValue({ jobId: 41, status: 'completed' });
    const transport = {
        inspectObjectDeletion,
        startObjectDeletion,
        waitForJob,
        cancelJob: vi.fn(),
    } as unknown as ImageTransport;
    const refreshSession = vi.fn().mockResolvedValue(undefined);
    const setStatus = vi.fn();
    const setSelection = vi.fn();
    const workflow = new DeletionWorkflow({
        transport,
        jobs: new JobController(transport),
        sessionId: () => 17,
        activeVolumeId: () => volume.id,
        selectedSource: () => volume,
        refreshSession,
        invalidateSession: vi.fn().mockResolvedValue(undefined),
        stopPlayback: vi.fn().mockResolvedValue(undefined),
        selection: () => ({ items: [target], anchors: {} }),
        setSelection,
        setStatus,
        reportTiming: vi.fn(),
    });
    return {
        workflow,
        inspectObjectDeletion,
        startObjectDeletion,
        waitForJob,
        refreshSession,
        setStatus,
        setSelection,
    };
}

async function openWorkflow(workflow: DeletionWorkflow): Promise<void> {
    workflow.requestObjects([target]);
    await vi.waitFor(() => expect(workflow.objectRequest?.loading).toBe(false));
}

describe('DeletionWorkflow integrity blockers', () => {
    it('does not submit a job when the initial inspection contains an integrity blocker', async () => {
        const { workflow, inspectObjectDeletion, startObjectDeletion } = createWorkflow();
        inspectObjectDeletion.mockResolvedValue(inspection(true));
        await openWorkflow(workflow);

        await workflow.submitObjects();

        expect(startObjectDeletion).not.toHaveBeenCalled();
        expect(workflow.objectRequest?.inspection?.blockers[0]?.message).toBe(blockerMessage);
        expect(workflow.objectRequest?.busy).toBe(false);
        workflow.dispose();
    });

    it('does not submit a job when the final inspection discovers an integrity blocker', async () => {
        const { workflow, inspectObjectDeletion, startObjectDeletion } = createWorkflow();
        await openWorkflow(workflow);
        inspectObjectDeletion.mockResolvedValueOnce(inspection(true));

        await workflow.submitObjects();

        expect(startObjectDeletion).not.toHaveBeenCalled();
        expect(workflow.objectRequest?.inspection?.canApply).toBe(false);
        expect(workflow.objectRequest?.inspection?.blockers[0]?.message).toBe(blockerMessage);
        expect(workflow.objectRequest?.busy).toBe(false);
        workflow.dispose();
    });

    it('shows a persistent integrity blocker after a failed job without suggesting another retry', async () => {
        const {
            workflow,
            inspectObjectDeletion,
            startObjectDeletion,
            waitForJob,
            refreshSession,
            setStatus,
            setSelection,
        } = createWorkflow();
        await openWorkflow(workflow);
        inspectObjectDeletion.mockResolvedValueOnce(inspection()).mockResolvedValueOnce(inspection(true));
        waitForJob.mockResolvedValueOnce({ jobId: 41, status: 'failed', error: 'Image relationships changed' });

        await workflow.submitObjects();

        expect(startObjectDeletion).toHaveBeenCalledTimes(1);
        expect(refreshSession).toHaveBeenCalledWith({ partitionIndex: 1, volumeName: 'importtest' });
        expect(workflow.objectRequest?.inspection?.canApply).toBe(false);
        expect(workflow.objectRequest?.error).toContain(blockerMessage);
        expect(workflow.objectRequest?.error).not.toContain('review the deletion again');
        expect(setStatus.mock.lastCall?.[0]).not.toContain('review the deletion again');
        expect(setSelection).not.toHaveBeenCalled();
        await workflow.submitObjects();
        expect(startObjectDeletion).toHaveBeenCalledTimes(1);
        workflow.dispose();
    });

    it('does not claim a successful refresh or keep a stale actionable preview when refresh fails', async () => {
        const { workflow, inspectObjectDeletion, startObjectDeletion, waitForJob, refreshSession, setStatus } =
            createWorkflow();
        await openWorkflow(workflow);
        waitForJob.mockResolvedValueOnce({ jobId: 41, status: 'failed', error: 'Image revision changed' });
        refreshSession.mockRejectedValueOnce(new Error('Server disconnected'));

        await workflow.submitObjects();

        expect(workflow.objectRequest?.error).toContain('Server disconnected');
        expect(workflow.objectRequest?.error).not.toContain('has been refreshed');
        expect(setStatus.mock.lastCall?.[0]).not.toContain('has been refreshed');
        expect(workflow.objectRequest?.inspection?.canApply ?? false).toBe(false);
        expect(workflow.objectRequest?.busy).toBe(false);
        expect(inspectObjectDeletion).toHaveBeenCalledTimes(2);
        await workflow.submitObjects();
        expect(startObjectDeletion).toHaveBeenCalledTimes(1);
        workflow.dispose();
    });

    it('retains review-and-retry recovery for a stale selection once refresh finds no integrity blocker', async () => {
        const { workflow, startObjectDeletion, waitForJob, refreshSession } = createWorkflow();
        await openWorkflow(workflow);
        waitForJob.mockResolvedValueOnce({ jobId: 41, status: 'failed', error: 'Image revision changed' });

        await workflow.submitObjects();

        expect(refreshSession).toHaveBeenCalledTimes(1);
        expect(workflow.objectRequest?.inspection?.canApply).toBe(true);
        expect(workflow.objectRequest?.error).toContain('refreshed');
        expect(workflow.objectRequest?.error).toContain('review');
        expect(workflow.objectRequest?.busy).toBe(false);
        expect(startObjectDeletion).toHaveBeenCalledTimes(1);
        await workflow.submitObjects();
        expect(startObjectDeletion).toHaveBeenCalledTimes(2);
        expect(workflow.objectRequest).toBeNull();
        workflow.dispose();
    });
});
