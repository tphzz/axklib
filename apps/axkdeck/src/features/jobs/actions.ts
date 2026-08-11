import type { ImageTransport, JobState } from '../../lib/transport';

type JobTransport = Pick<ImageTransport, 'waitForJob' | 'cancelJob'>;

export class JobController {
    private readonly activeJobIds = new Set<number>();
    private disposed = false;

    constructor(private readonly transport: JobTransport) {}

    async run(
        start: () => Promise<JobState>,
        onUpdate: (job: JobState) => void = () => undefined,
        onStarted: (job: JobState) => void | Promise<void> = () => undefined,
    ): Promise<JobState> {
        if (this.disposed) throw new Error('Job controller is disposed');
        const job = await start();
        if (this.disposed) {
            await this.transport.cancelJob(job.jobId).catch(() => undefined);
            throw new Error('Job controller is disposed');
        }
        this.activeJobIds.add(job.jobId);
        try {
            await onStarted(job);
            return await this.transport.waitForJob(job.jobId, onUpdate);
        } finally {
            this.activeJobIds.delete(job.jobId);
        }
    }

    async cancel(jobId: number): Promise<void> {
        if (!this.activeJobIds.has(jobId)) return;
        await this.transport.cancelJob(jobId);
    }

    async dispose(): Promise<void> {
        if (this.disposed) return;
        this.disposed = true;
        const jobIds = [...this.activeJobIds];
        for (const jobId of jobIds) {
            await this.transport.cancelJob(jobId).catch(() => undefined);
        }
    }
}
