import { AxklibApiError } from '../../lib/httpErrors';
import { CapacityWriteRejected } from '../../lib/httpCapacityGate';
import type { ImageTransport, JobState } from '../../lib/transport';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { JobController } from '../jobs/actions';
import { diagnosticsEnabled, reportDiagnostic } from '../../lib/diagnostics';

export type VolumeActionPhase =
    'idle' | 'checking' | 'checking-status' | 'submitting' | 'refreshing' | 'unconfirmed' | 'refresh-failed';

export class VolumeActionExecution {
    phase = $state<VolumeActionPhase>('idle');
    error = $state('');
    private jobId: number | null = null;
    private serverJobId: string | undefined;
    private completed: JobState | null = null;
    private finish: (() => Promise<void>) | null = null;
    private onUpdate: (job: JobState) => void = () => {};
    private generation = 0;

    constructor(
        private readonly transport: Pick<ImageTransport, 'waitForJob'>,
        private readonly jobs: JobController,
    ) {}

    get busy(): boolean {
        return ['checking', 'checking-status', 'submitting', 'refreshing'].includes(this.phase);
    }
    get locked(): boolean {
        return this.phase !== 'idle';
    }
    get canDismiss(): boolean {
        return this.phase === 'idle' || this.phase === 'refresh-failed';
    }
    get recovery(): 'check' | 'refresh' | null {
        if (this.phase === 'refresh-failed') return 'refresh';
        return this.phase === 'unconfirmed' && this.jobId !== null ? 'check' : null;
    }

    reset(): void {
        this.generation++;
        this.phase = 'idle';
        this.error = '';
        this.jobId = null;
        this.serverJobId = undefined;
        this.completed = null;
        this.finish = null;
    }

    async run(
        start: () => Promise<JobState>,
        finish: () => Promise<void>,
        onUpdate: (job: JobState) => void,
        onStarted: () => void,
    ): Promise<void> {
        if (this.locked) return;
        const generation = ++this.generation;
        this.phase = 'checking';
        this.error = '';
        this.finish = finish;
        this.onUpdate = onUpdate;
        try {
            const result = await this.measure('job-wait', () =>
                this.jobs.run(
                    async () => {
                        const job = await start();
                        if (generation === this.generation) {
                            this.jobId = job.jobId;
                            this.serverJobId = job.serverJobId;
                        }
                        return job;
                    },
                    onUpdate,
                    () => {
                        if (generation !== this.generation) return;
                        this.phase = 'submitting';
                        onStarted();
                    },
                ),
            );
            await this.accept(result, generation);
        } catch (error) {
            if (generation !== this.generation) return;
            if (
                this.jobId === null &&
                (error instanceof CapacityWriteRejected ||
                    (error instanceof AxklibApiError &&
                        error.status >= 400 &&
                        error.status < 500 &&
                        error.status !== 408))
            ) {
                this.phase = 'idle';
                this.error = userFacingMessage(error);
            } else this.unconfirmed(error);
        }
    }

    async recover(): Promise<void> {
        const generation = this.generation;
        if (this.recovery === 'refresh') return this.refresh(generation);
        if (this.recovery !== 'check' || this.jobId === null) return;
        this.phase = 'checking-status';
        this.error = '';
        try {
            await this.accept(
                await this.measure('status-check', () => this.transport.waitForJob(this.jobId!, this.onUpdate)),
                generation,
            );
        } catch (error) {
            if (generation === this.generation) this.unconfirmed(error);
        }
    }

    private async accept(job: JobState, generation: number): Promise<void> {
        if (generation !== this.generation) return;
        if (job.status !== 'completed') {
            if (job.status === 'failed' || job.status === 'cancelled') {
                this.phase = 'idle';
                this.jobId = null;
                this.error = job.error ?? 'Image change did not complete';
            } else this.unconfirmed(new Error('The image job has not completed'));
            return;
        }
        this.completed = job;
        await this.refresh(generation);
    }

    private async refresh(generation: number): Promise<void> {
        if (!this.completed || !this.finish) return;
        this.phase = 'refreshing';
        this.error = '';
        try {
            await this.measure('workspace-refresh', this.finish);
            if (generation === this.generation) this.reset();
        } catch (error) {
            if (generation !== this.generation) return;
            this.phase = 'refresh-failed';
            this.error = `Image change saved; refresh failed: ${userFacingMessage(error)}`;
        }
    }

    private unconfirmed(error: unknown): void {
        this.phase = 'unconfirmed';
        this.error = `Image change could not be confirmed${this.jobId === null ? '; no job reference was received. Do not repeat the change' : ''}: ${userFacingMessage(error)}`;
    }

    private async measure<T>(phase: string, action: () => Promise<T>): Promise<T> {
        if (!diagnosticsEnabled()) return action();
        const started = performance.now();
        let outcome = 'resolved';
        try {
            return await action();
        } catch (error) {
            outcome = 'rejected';
            throw error;
        } finally {
            try {
                reportDiagnostic(
                    'image_mutation_frontend_phase',
                    {
                        phase,
                        jobId: this.serverJobId,
                        clientJobId: this.jobId,
                        durationMs: Math.round(performance.now() - started),
                        outcome,
                    },
                    'debug',
                );
            } catch {
                /* Diagnostics must not change the outcome of a write. */
            }
        }
    }
}
