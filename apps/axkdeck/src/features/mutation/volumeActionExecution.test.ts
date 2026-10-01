import { describe, expect, it, vi } from 'vitest';
import { VolumeActionExecution } from './volumeActionExecution.svelte';
import { JobController } from '../jobs/actions';
import { AxklibApiError } from '../../lib/httpErrors';
import { CapacityWriteRejected } from '../../lib/httpCapacityGate';
import type { JobState } from '../../lib/transport';

const queued: JobState = { jobId: 9, kind: 'test', status: 'queued' };
const completed: JobState = { ...queued, status: 'completed' };

function setup() {
    const waitForJob = vi.fn().mockResolvedValue(completed);
    const transport = { waitForJob, cancelJob: vi.fn() };
    const execution = new VolumeActionExecution(transport, new JobController(transport));
    return { execution, waitForJob, finish: vi.fn().mockResolvedValue(undefined), started: vi.fn() };
}

describe('volume action execution recovery', () => {
    it.each([
        [new CapacityWriteRejected('Inspection failed'), 'idle'],
        [new AxklibApiError('refused', 'Invalid name', 422), 'idle'],
        [new AxklibApiError('timeout', 'Response timed out', 408), 'unconfirmed'],
        [new AxklibApiError('internal', 'Server disconnected', 500), 'unconfirmed'],
        [new Error('Response lost'), 'unconfirmed'],
    ])('distinguishes definite refusal from uncertainty (%s)', async (error, phase) => {
        const { execution, finish, started, waitForJob } = setup();
        await execution.run(
            async () => {
                throw error;
            },
            finish,
            vi.fn(),
            started,
        );
        expect(execution.phase).toBe(phase);
        expect(execution.locked).toBe(phase === 'unconfirmed');
        expect(execution.canDismiss).toBe(phase === 'idle');
        expect(execution.recovery).toBeNull();
        expect(finish).not.toHaveBeenCalled();
        expect(waitForJob).not.toHaveBeenCalled();
        expect(started).not.toHaveBeenCalled();
    });

    it.each(['failed', 'cancelled'] as const)('returns to editing after definitive job %s', async (status) => {
        const { execution, waitForJob, finish, started } = setup();
        waitForJob.mockResolvedValueOnce({ ...queued, status, error: 'Not saved' });
        await execution.run(async () => queued, finish, vi.fn(), started);
        expect(execution.phase).toBe('idle');
        expect(execution.error).toBe('Not saved');
        expect(execution.recovery).toBeNull();
        expect(finish).not.toHaveBeenCalled();
        expect(started).toHaveBeenCalledOnce();
    });

    it('checks the same pending job once and ignores a reset during recovery', async () => {
        const { execution, waitForJob, finish, started } = setup();
        waitForJob.mockResolvedValueOnce(queued);
        await execution.run(async () => queued, finish, vi.fn(), started);
        expect(execution.phase).toBe('unconfirmed');
        let resolve!: (job: JobState) => void;
        waitForJob.mockImplementationOnce(
            () =>
                new Promise((done) => {
                    resolve = done;
                }),
        );
        const recovery = execution.recover();
        expect(execution.phase).toBe('checking-status');
        await execution.recover();
        expect(waitForJob).toHaveBeenCalledTimes(2);
        execution.reset();
        resolve(completed);
        await recovery;
        expect(finish).not.toHaveBeenCalled();
        expect(execution.phase).toBe('idle');
    });

    it('does not publish an acknowledgement or refresh for an obsolete request', async () => {
        const { execution, finish, started } = setup();
        let acknowledge!: (job: JobState) => void;
        const pending = execution.run(
            () =>
                new Promise((resolve) => {
                    acknowledge = resolve;
                }),
            finish,
            vi.fn(),
            started,
        );
        execution.reset();
        acknowledge(queued);
        await pending;
        expect(execution.phase).toBe('idle');
        expect(started).not.toHaveBeenCalled();
        expect(finish).not.toHaveBeenCalled();
    });
});
