import { describe, expect, it, vi } from 'vitest';
import { JobController } from './actions';
import type { JobState } from '../../lib/transport';

function job(jobId: number, status: JobState['status']): JobState {
    return { jobId, kind: 'test', status };
}

describe('JobController', () => {
    it('tracks a job only until terminal reconciliation completes', async () => {
        const cancelJob = vi.fn(async () => undefined);
        let settle: ((value: JobState) => void) | undefined;
        const waitForJob = vi.fn(
            async () =>
                await new Promise<JobState>((resolve) => {
                    settle = resolve;
                }),
        );
        const controller = new JobController({ waitForJob, cancelJob });
        const running = controller.run(async () => job(7, 'queued'));

        await vi.waitFor(() => expect(waitForJob).toHaveBeenCalledOnce());
        await controller.cancel(7);
        expect(cancelJob).toHaveBeenCalledWith(7);

        settle?.(job(7, 'completed'));
        await expect(running).resolves.toMatchObject({ status: 'completed' });
        await controller.cancel(7);
        expect(cancelJob).toHaveBeenCalledOnce();
    });

    it('cancels every active job once during disposal and rejects later starts', async () => {
        const cancelled: number[] = [];
        const cancelJob = vi.fn(async (jobId: number) => {
            cancelled.push(jobId);
        });
        const waitForJob = vi.fn(async () => await new Promise<JobState>(() => undefined));
        const controller = new JobController({ waitForJob, cancelJob });
        void controller.run(async () => job(2, 'queued'));
        void controller.run(async () => job(3, 'queued'));
        await vi.waitFor(() => expect(waitForJob).toHaveBeenCalledTimes(2));

        await controller.dispose();
        expect(cancelled).toEqual([2, 3]);
        await expect(controller.run(async () => job(4, 'queued'))).rejects.toThrow('disposed');
    });
});
