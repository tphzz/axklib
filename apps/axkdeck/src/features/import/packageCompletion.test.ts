import { describe, expect, it, vi } from 'vitest';
import { serverFileLocation } from '../../lib/storageLocations';
import type { ImageSessionPackageImportPlan, ImageTransport, JobState, PackageInspection } from '../../lib/transport';
import { PickerController } from '../dialogs/picker';
import type { JobController } from '../jobs/actions';
import { PackageBatchImportWorkflow } from './packageBatchWorkflow.svelte';
import { PackagePickerHistory } from './packagePickerHistory';
import { PackageImportWorkflow } from './packageWorkflow.svelte';

const source = serverFileLocation({ rootId: 'workspace', relativePath: 'One.axkvol' }, 'One.axkvol');
const plan = {
    valid: true,
    planToken: 'plan-1',
    packages: [{ destinationVolumeName: 'One' }],
} as ImageSessionPackageImportPlan;
const completed: JobState = { jobId: 11, kind: 'package-import', status: 'completed' };

function fixture(batch: boolean, refreshSession = vi.fn().mockResolvedValue(undefined)) {
    const run = vi.fn().mockResolvedValue(completed);
    const waitForJob = vi.fn().mockResolvedValue(completed);
    const dependencies = {
        transport: {
            releaseClientUpload: vi.fn().mockResolvedValue(undefined),
            releaseImagePackageImportPlan: vi.fn().mockResolvedValue(undefined),
            startImagePackageImport: vi.fn().mockResolvedValue({ ...completed, status: 'queued' }),
            waitForJob,
        } as unknown as ImageTransport,
        jobs: { run } as unknown as JobController,
        picker: new PickerController(() => undefined),
        pickerHistory: new PackagePickerHistory(),
        isDesktop: false,
        sessionId: () => 17,
        invalidateSession: vi.fn().mockResolvedValue(undefined),
        refreshSession,
        setStatus: vi.fn(),
    };
    const workflow = batch ? new PackageBatchImportWorkflow(dependencies) : new PackageImportWorkflow(dependencies);
    workflow.open({ id: 'volume', name: 'One', kind: 'volume', childCount: 0, partitionIndex: 0 });
    if (workflow instanceof PackageBatchImportWorkflow) {
        workflow.request = {
            ...workflow.request!,
            status: 'ready',
            plan,
            items: [
                {
                    id: 'one',
                    selected: true,
                    source,
                    sourceName: 'One',
                    inspection: {} as PackageInspection,
                    upload: null,
                    localPath: null,
                },
            ],
        };
    } else {
        workflow.request = { ...workflow.request!, status: 'ready', plan, source };
    }
    return { workflow, run, waitForJob, refreshSession, dependencies };
}

describe.each([false, true])('package completion (batch=%s)', (batch) => {
    it('keeps the dialog locked until refresh finishes', async () => {
        let finish!: () => void;
        const refresh = vi.fn(
            () =>
                new Promise<void>((resolve) => {
                    finish = resolve;
                }),
        );
        const { workflow } = fixture(batch, refresh);
        const applying = workflow.apply();
        await vi.waitFor(() => expect(refresh).toHaveBeenCalledOnce());
        expect(workflow.request?.status).toBe('applying');
        finish();
        await applying;
        expect(workflow.request).toBeNull();
    });

    it('retains a committed import after refresh failure and refreshes without writing again', async () => {
        const refresh = vi.fn().mockRejectedValueOnce(new Error('refresh disconnected')).mockResolvedValue(undefined);
        const { workflow, run } = fixture(batch, refresh);
        await workflow.apply();
        expect(workflow.request).not.toBeNull();
        expect(workflow.completion.phase).toBe('refresh-failed');
        await workflow.apply();
        expect(run).toHaveBeenCalledOnce();
        await workflow.recoverCompletion();
        expect(workflow.request).toBeNull();
        expect(refresh).toHaveBeenCalledTimes(2);
        expect(run).toHaveBeenCalledOnce();
    });

    it('allows dismissal after a committed import cannot refresh', async () => {
        const { workflow } = fixture(batch, vi.fn().mockRejectedValue(new Error('refresh disconnected')));
        await workflow.apply();
        expect(workflow.completion.phase).toBe('refresh-failed');
        await workflow.close();
        expect(workflow.request).toBeNull();
    });

    it('retains the acknowledged job after a lost result and checks it without resubmitting', async () => {
        const { workflow, run, waitForJob, refreshSession } = fixture(batch);
        run.mockImplementationOnce(async (start) => {
            await start();
            throw new Error('connection lost');
        });
        await workflow.apply();
        expect(workflow.completion.phase).toBe('unconfirmed');
        expect(refreshSession).not.toHaveBeenCalled();
        await workflow.close();
        expect(workflow.request).not.toBeNull();
        await workflow.apply();
        expect(run).toHaveBeenCalledOnce();
        await workflow.recoverCompletion();
        expect(waitForJob).toHaveBeenCalledWith(11, expect.any(Function));
        expect(workflow.request).toBeNull();
        expect(run).toHaveBeenCalledOnce();
    });

    it('returns to editable review after checking confirms the job failed', async () => {
        const { workflow, run, waitForJob, refreshSession } = fixture(batch);
        run.mockImplementationOnce(async (start) => {
            await start();
            throw new Error('connection lost');
        });
        waitForJob.mockResolvedValueOnce({ ...completed, status: 'failed', error: 'Capacity changed' });
        await workflow.apply();
        await workflow.recoverCompletion();
        expect(workflow.request).toMatchObject({ status: 'ready', error: 'Capacity changed' });
        expect(workflow.completion.phase).toBe('idle');
        expect(workflow.completion.message).toBe('');
        expect(refreshSession).not.toHaveBeenCalled();
    });
});
