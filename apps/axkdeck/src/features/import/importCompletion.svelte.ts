import { AxklibApiError } from '../../lib/httpErrors';
import type { ImageTransport, JobState } from '../../lib/transport';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { JobController } from '../jobs/actions';
import type { components } from '../../lib/generated/axklibApiV1';

export class ImportCompletion {
    phase = $state<
        'idle' | 'importing' | 'checking' | 'refreshing' | 'unconfirmed' | 'refresh-failed' | 'warnings' | 'completed'
    >('idle');
    message = $state('');
    failure = $state<JobState | null>(null);
    warnings = $state<string[]>([]);
    private reviewedWarnings: readonly string[] = [];
    private jobId: number | null = null;
    private completed: JobState | null = null;
    private finish: ((job: JobState) => Promise<void>) | null = null;
    private onUpdate: (job: JobState) => void = () => undefined;

    constructor(
        private readonly transport: Pick<ImageTransport, 'waitForJob'>,
        private readonly jobs: JobController,
    ) {}

    get busy(): boolean {
        return ['importing', 'checking', 'refreshing'].includes(this.phase);
    }
    get locked(): boolean {
        return this.phase !== 'idle';
    }
    get canCheck(): boolean {
        return this.phase === 'unconfirmed' && this.jobId !== null;
    }
    get canDismiss(): boolean {
        return ['idle', 'refresh-failed', 'warnings', 'completed'].includes(this.phase);
    }

    reset(): void {
        if (this.busy || this.phase === 'unconfirmed') return;
        this.phase = 'idle';
        this.message = '';
        this.failure = null;
        this.warnings = [];
        this.jobId = null;
        this.completed = null;
        this.finish = null;
    }

    async run(
        start: () => Promise<JobState>,
        finish: (job: JobState) => Promise<void>,
        onUpdate: (job: JobState) => void = () => undefined,
        reviewedWarnings: readonly string[] = [],
    ): Promise<boolean> {
        if (this.locked) return false;
        this.phase = 'importing';
        this.message = 'Importing';
        this.failure = null;
        this.warnings = [];
        this.reviewedWarnings = reviewedWarnings;
        this.finish = finish;
        this.onUpdate = onUpdate;
        try {
            const job = await this.jobs.run(async () => {
                const started = await start();
                this.jobId = started.jobId;
                return started;
            }, onUpdate);
            return await this.accept(job);
        } catch (error) {
            if (
                this.jobId === null &&
                error instanceof AxklibApiError &&
                error.status >= 400 &&
                error.status < 500 &&
                error.status !== 408
            ) {
                this.phase = 'idle';
                this.message = userFacingMessage(error);
            } else {
                this.unconfirmed(error);
            }
            return false;
        }
    }

    async recover(): Promise<boolean> {
        if (this.phase === 'refresh-failed' && this.completed) return this.refresh();
        if (!this.canCheck || this.jobId === null) return false;
        this.phase = 'checking';
        this.message = 'Checking import status';
        try {
            return await this.accept(await this.transport.waitForJob(this.jobId, this.onUpdate));
        } catch (error) {
            this.unconfirmed(error);
            return false;
        }
    }

    private async accept(job: JobState): Promise<boolean> {
        if (job.status !== 'completed') {
            if (job.status === 'failed' || job.status === 'cancelled') {
                this.failure = job;
                this.phase = 'idle';
                this.jobId = null;
                this.message = job.error ?? 'Import did not complete';
            } else this.unconfirmed(new Error('The job has not completed'));
            return false;
        }
        this.completed = job;
        this.warnings = alterationWarnings(job.result, this.reviewedWarnings);
        return this.refresh();
    }

    private async refresh(): Promise<boolean> {
        if (!this.finish || !this.completed) return false;
        this.phase = 'refreshing';
        this.message = 'Import saved; refreshing workspace';
        try {
            await this.finish(this.completed);
            this.phase = this.warnings.length ? 'warnings' : 'completed';
            this.message = this.warnings.length
                ? `Imported with ${this.warnings.length} warning${this.warnings.length === 1 ? '' : 's'}`
                : 'Import complete';
            return this.warnings.length === 0;
        } catch (error) {
            this.phase = 'refresh-failed';
            this.message = `Import saved; refresh failed: ${userFacingMessage(error)}`;
            return false;
        }
    }

    private unconfirmed(error: unknown): void {
        this.phase = 'unconfirmed';
        this.message = `Import outcome unconfirmed${this.jobId === null ? '; no job reference was received. Do not repeat the import' : ''}: ${userFacingMessage(error)}`;
    }
}

function alterationWarnings(value: unknown, reviewed: readonly string[]): string[] {
    if (!value || typeof value !== 'object' || !('kind' in value) || value.kind !== 'ALTERATION') return [];
    const result = value as components['schemas']['ImageSessionAlterationResult'];
    const warnings = (result.warnings ?? [])
        .map((warning) => warning.message)
        .filter((message) => !reviewed.includes(message));
    for (const operation of result.operations ?? []) {
        const clipped = operation.audioImport?.clippedSamples ?? 0;
        if (clipped > 0) warnings.push(`${operation.objectName}: ${clipped} clipped sample${clipped === 1 ? '' : 's'}`);
    }
    return [...new Set(warnings)];
}
