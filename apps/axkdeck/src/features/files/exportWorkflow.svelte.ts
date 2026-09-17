import type { FilesystemEntry } from '../../lib/filesystem';
import type {
    FilesystemExportActions,
    FilesystemExportInspection,
    FilesystemExportResult,
    FilesystemExportRoute,
    FilesystemExportTarget,
} from '../../lib/filesystemExport';
import type { JobState } from '../../lib/transport';
import { userFacingMessage } from '../../lib/userFacingMessage';

interface ExportReview {
    revision: number;
    entryIds: string[];
    name: string;
    directComputer: boolean;
    desktop: boolean;
}

export class FilesExportWorkflow {
    constructor(private readonly setStatus: (message: string) => void = () => undefined) {}

    review = $state<ExportReview | null>(null);
    inspection = $state.raw<FilesystemExportInspection | null>(null);
    phase = $state<'inspecting' | 'ready' | 'choosing' | 'running' | 'saving' | 'completed' | 'failed' | 'unconfirmed'>(
        'ready',
    );
    message = $state('');
    jobId = $state<number | null>(null);
    cancelling = $state(false);
    route = $state<FilesystemExportRoute | null>(null);
    destinationName = $state('');
    private driver: FilesystemExportActions | null = null;
    private target: FilesystemExportTarget | null = null;
    private retained: FilesystemExportResult | null = null;
    private generation = 0;
    private disposed = false;

    get busy(): boolean {
        return ['inspecting', 'choosing', 'running', 'saving'].includes(this.phase);
    }
    get canChoose(): boolean {
        return (
            !!this.review &&
            !!this.inspection &&
            (!!this.inspection.rootDirectory || this.inspection.entries.length > 0) &&
            ['ready', 'failed'].includes(this.phase)
        );
    }
    get entryCount(): number {
        return (this.inspection?.entries.length ?? 0) + (this.inspection?.rootDirectory ? 1 : 0);
    }
    get canCancel(): boolean {
        return (
            !this.cancelling &&
            ((this.phase === 'running' && this.jobId !== null) ||
                (this.phase === 'saving' && !!this.target?.cancelPublication))
        );
    }

    async open(revision: number, entries: FilesystemEntry[], driver: FilesystemExportActions): Promise<void> {
        if (
            this.disposed ||
            this.review ||
            !Number.isSafeInteger(revision) ||
            revision < 1 ||
            !entries.length ||
            entries.length > 10_000 ||
            entries.some((entry) => entry.filesystemMetadata || entry.issue)
        )
            return;
        const generation = ++this.generation;
        this.driver = driver;
        this.review = {
            revision,
            entryIds: entries.map((entry) => entry.id),
            name: entries.length === 1 ? entries[0].name : 'Files',
            directComputer: driver.directComputer,
            desktop: driver.desktop,
        };
        this.inspection = null;
        this.target = null;
        this.route = null;
        this.destinationName = '';
        this.jobId = null;
        this.cancelling = false;
        this.phase = 'inspecting';
        this.message = 'Inspecting files';
        try {
            const inspection = await driver.inspect(revision, [...this.review.entryIds], 'EXPORT_FOLDER');
            if (generation !== this.generation) return;
            if (inspection.revision !== revision)
                throw new Error('The image changed. Close and review the selection again.');
            this.inspection = inspection;
            this.phase = 'ready';
            this.message = this.entryCount ? 'Ready' : 'No exportable entries';
        } catch (error) {
            if (generation !== this.generation) return;
            this.phase = 'failed';
            this.message = userFacingMessage(error);
        }
    }

    async choose(route: FilesystemExportRoute): Promise<void> {
        const review = this.review;
        const driver = this.driver;
        if (
            !this.canChoose ||
            !review ||
            !driver ||
            (review.directComputer && route !== 'computer') ||
            (this.route !== null && route !== this.route) ||
            (route === 'computer' && !review.desktop)
        )
            return;
        const generation = this.generation;
        const previous = this.phase;
        const previousRoute = this.route;
        this.route = route;
        this.phase = 'choosing';
        try {
            const suggestedName = this.inspection!.rootDirectory?.name ?? 'Files';
            const target = await driver.chooseDestination(route, suggestedName);
            if (generation !== this.generation) return;
            if (!target) {
                this.phase = previous;
                this.route = previousRoute;
                return;
            }
            this.target = target;
            this.destinationName =
                target.destination.kind === 'DOWNLOAD'
                    ? target.destination.directoryName
                    : (target.destination.output.relativePath.split('/').at(-1) ?? suggestedName);
            if (this.retained) await this.save(this.retained, generation);
            else {
                this.jobId = null;
                await this.run((update) =>
                    driver.execute(review.revision, [...review.entryIds], target.destination, update, 'EXPORT_FOLDER'),
                );
            }
        } catch (error) {
            if (generation !== this.generation) return;
            this.phase = 'failed';
            this.message = userFacingMessage(error);
        }
    }

    async checkStatus(): Promise<void> {
        if (this.phase !== 'unconfirmed' || this.jobId === null || !this.driver) return;
        await this.run((update) => this.driver!.observe(this.jobId!, update));
    }

    private async run(operation: (update: (job: JobState) => void) => Promise<JobState>): Promise<void> {
        const generation = this.generation;
        const driver = this.driver!;
        this.phase = 'running';
        this.message = 'Exporting';
        try {
            const job = await operation((update) => {
                if (generation !== this.generation) {
                    void driver.cancel(update.jobId).catch(() => undefined);
                    return;
                }
                this.jobId = update.jobId;
                this.message = this.cancelling ? 'Cancellation requested' : (update.progress?.label ?? 'Exporting');
            });
            const result = job.status === 'completed' ? (job.result as FilesystemExportResult | undefined) : undefined;
            if (generation !== this.generation) {
                if (result) await driver.release(result).catch(() => undefined);
                return;
            }
            if (job.status === 'completed' && result) await this.save(result, generation);
            else if (job.status === 'failed' || job.status === 'cancelled') {
                this.phase = 'failed';
                this.message = job.status === 'cancelled' ? 'Cancelled' : (job.error ?? 'Export failed');
            } else throw new Error('The export has not returned a terminal result.');
        } catch (error) {
            if (generation !== this.generation) return;
            this.phase = 'unconfirmed';
            this.message = `The export outcome is unconfirmed. ${userFacingMessage(error)}`;
        } finally {
            this.cancelling = false;
        }
    }

    private async save(result: FilesystemExportResult, generation: number): Promise<void> {
        const driver = this.driver!;
        this.retained = result;
        this.phase = 'saving';
        this.cancelling = false;
        this.message = 'Saving files';
        try {
            await this.target!.publish(result);
            this.target = null;
            const cleaned = await this.release();
            if (generation === this.generation) {
                this.phase = 'completed';
                const count = result.entries.length + (result.rootDirectory ? 1 : 0);
                this.message = `Exported ${count} ${count === 1 ? 'entry' : 'entries'}`;
                if (!cleaned) this.message += '; temporary archive cleanup failed';
                else if (result.notices.length)
                    this.message += `; ${result.notices.length} ${result.notices.length === 1 ? 'notice' : 'notices'}`;
                this.setStatus(this.message);
                if (cleaned && !result.notices.length) this.close();
            }
        } catch (error) {
            if (generation !== this.generation) {
                await driver.release(result).catch(() => undefined);
                return;
            }
            this.phase = 'failed';
            this.message = userFacingMessage(error);
        } finally {
            if (generation === this.generation) this.cancelling = false;
        }
    }

    private async release(): Promise<boolean> {
        const result = this.retained;
        this.retained = null;
        if (!result) return true;
        return this.driver!.release(result).then(
            () => true,
            () => false,
        );
    }

    async cancel(): Promise<void> {
        if (!this.canCancel) return;
        const generation = this.generation;
        const phase = this.phase;
        const target = this.target;
        const isCurrent = () => generation === this.generation && phase === this.phase && target === this.target;
        this.cancelling = true;
        try {
            if (phase === 'saving') await target!.cancelPublication!();
            else await this.driver!.cancel(this.jobId!);
            if (isCurrent()) this.message = 'Cancellation requested';
        } catch (error) {
            if (isCurrent()) {
                this.cancelling = false;
                this.message = userFacingMessage(error);
            }
        }
    }

    close(): void {
        if (this.busy && this.phase !== 'inspecting') return;
        if (this.phase === 'unconfirmed' && this.jobId !== null)
            void this.driver?.cancel(this.jobId).catch(() => undefined);
        ++this.generation;
        this.review = null;
        void this.release();
    }

    dispose(): void {
        this.disposed = true;
        ++this.generation;
        if (['running', 'unconfirmed'].includes(this.phase) && this.jobId !== null)
            void this.driver?.cancel(this.jobId).catch(() => undefined);
        if (this.phase === 'saving') void this.target?.cancelPublication?.().catch(() => undefined);
        else void this.release();
        this.review = null;
    }
}
