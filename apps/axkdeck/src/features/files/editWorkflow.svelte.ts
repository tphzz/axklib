import type {
    FilesystemEdit,
    FilesystemEntry,
    FilesystemMutationDriver,
    FilesystemRootCapabilities,
} from '../../lib/filesystem';
import type { JobState } from '../../lib/transport';
import { userFacingMessage } from '../../lib/userFacingMessage';

export interface FilesEditReview {
    kind: 'create' | 'delete';
    revision: number;
    entries: FilesystemEntry[];
    capabilities: FilesystemRootCapabilities;
}

export function validFilesystemName(name: string, capabilities: FilesystemRootCapabilities): boolean {
    if (!name || name === '.' || name === '..' || /[/\\\0]/.test(name)) return false;
    if (new TextEncoder().encode(name).length > capabilities.maximumNameBytes) return false;
    try {
        return !!capabilities.namePattern && new RegExp(capabilities.namePattern).exec(name)?.[0] === name;
    } catch {
        return false;
    }
}

export class FilesEditWorkflow {
    review = $state<FilesEditReview | null>(null);
    name = $state('');
    phase = $state<'ready' | 'running' | 'refreshing' | 'failed' | 'refresh-failed' | 'unconfirmed'>('ready');
    message = $state('');
    jobId = $state<number | null>(null);
    cancelling = $state(false);
    private driver: FilesystemMutationDriver | null = null;
    private disposed = false;

    get busy(): boolean {
        return this.phase === 'running' || this.phase === 'refreshing';
    }
    get canSubmit(): boolean {
        return (
            !!this.review &&
            !this.busy &&
            (this.phase !== 'ready' ||
                this.review.kind === 'delete' ||
                validFilesystemName(this.name, this.review.capabilities))
        );
    }

    open(review: FilesEditReview, driver: FilesystemMutationDriver): boolean {
        if (this.disposed || this.review || !review.entries.length || review.revision < 1) return false;
        const capability =
            review.kind === 'create' ? review.capabilities.createDirectory : review.capabilities.deleteEntry;
        if (
            !capability ||
            review.entries.some(
                (entry) =>
                    entry.rootId !== review.capabilities.rootId ||
                    entry.filesystemMetadata ||
                    !!entry.issue ||
                    (review.kind === 'delete' ? !entry.parentId : entry.kind === 'file'),
            )
        )
            return false;
        if (review.kind === 'create' && review.entries.length !== 1) return false;
        this.review = {
            ...review,
            capabilities: { ...review.capabilities },
            entries: review.entries.map((entry) => ({ ...entry, ancestorIds: [...entry.ancestorIds] })),
        };
        this.driver = driver;
        this.phase = 'ready';
        this.name = '';
        this.message = 'Ready';
        this.jobId = null;
        this.cancelling = false;
        return true;
    }

    close(): void {
        if (!this.busy) this.review = null;
    }

    async submit(): Promise<void> {
        if (!this.canSubmit || !this.review || !this.driver || this.disposed) return;
        if (this.phase === 'unconfirmed' && this.jobId !== null) {
            await this.run(() => this.driver!.observe(this.jobId!, this.update));
        } else if (this.phase !== 'ready') {
            await this.refresh();
        } else {
            const review = this.review;
            const selected = new Set(review.entries.map((entry) => entry.id));
            const edits: FilesystemEdit[] =
                review.kind === 'create'
                    ? [{ kind: 'CREATE_DIRECTORY', parentEntryId: review.entries[0].id, relativePath: [this.name] }]
                    : review.entries
                          .filter((entry) => !entry.ancestorIds.some((id) => selected.has(id)))
                          .map((entry) => ({
                              kind: 'DELETE',
                              entryId: entry.id,
                              recursive: entry.kind === 'directory',
                          }));
            await this.run(() => this.driver!.execute(review.revision, edits, this.update));
        }
    }

    private update = (job: JobState): void => {
        if (this.disposed) {
            void this.driver?.cancel(job.jobId).catch(() => undefined);
            return;
        }
        this.jobId = job.jobId;
        this.message = this.cancelling ? 'Cancellation requested' : (job.progress?.label ?? 'Working');
    };

    private async run(operation: () => Promise<JobState>): Promise<void> {
        this.phase = 'running';
        this.message = 'Submitting';
        try {
            const job = await operation();
            if (this.disposed) return;
            if (job.status === 'completed') await this.refresh(true);
            else {
                this.phase = 'failed';
                this.message =
                    job.status === 'cancelled' ? 'Cancelled' : (job.error ?? 'The filesystem job did not complete.');
            }
        } catch (error) {
            if (this.disposed) return;
            this.phase = 'unconfirmed';
            this.message = `The write outcome is unconfirmed. ${userFacingMessage(error)}`;
        } finally {
            this.cancelling = false;
        }
    }

    private async refresh(committed = this.phase === 'refresh-failed'): Promise<void> {
        this.phase = 'refreshing';
        this.message = committed ? 'Changes saved. Refreshing' : 'Refreshing';
        try {
            await this.driver!.refresh();
            if (!this.disposed) this.review = null;
        } catch (error) {
            if (this.disposed) return;
            this.phase = committed ? 'refresh-failed' : 'failed';
            this.message = `${committed ? 'Changes saved; refresh failed. ' : ''}${userFacingMessage(error)}`;
        }
    }

    async cancel(): Promise<void> {
        if (this.phase !== 'running' || this.jobId === null || this.cancelling) return;
        this.cancelling = true;
        try {
            await this.driver!.cancel(this.jobId);
            if (!this.disposed && this.busy) this.message = 'Cancellation requested';
        } catch (error) {
            this.message = userFacingMessage(error);
            this.cancelling = false;
        }
    }

    dispose(): void {
        this.disposed = true;
        if ((this.phase === 'running' || this.phase === 'unconfirmed') && this.jobId !== null)
            void this.driver?.cancel(this.jobId).catch(() => undefined);
    }
}
