import type { FilesystemEntry } from '../../lib/filesystem';
import type { FilesystemExportActions, FilesystemExportResult } from '../../lib/filesystemExport';
import { userFacingMessage } from '../../lib/userFacingMessage';

export class FilesDragWorkflow {
    private running = false;
    private cancelled = false;
    private handedOff = false;
    private disposed = false;
    private jobId: number | null = null;
    private ticket: string | null = null;
    private driver: FilesystemExportActions | null = null;
    private requested = new Set<number>();

    constructor(
        private status: (message: string, busy: boolean) => void,
        private error: (message: string) => void,
    ) {}

    async start(
        revision: number,
        entries: FilesystemEntry[],
        driver: FilesystemExportActions,
        current: () => boolean,
    ): Promise<void> {
        if (
            this.running ||
            this.disposed ||
            !driver.drag ||
            !Number.isSafeInteger(revision) ||
            revision < 1 ||
            !entries.length ||
            entries.length > 10_000 ||
            entries.some((entry) => entry.filesystemMetadata || entry.issue)
        )
            return;
        const ids = entries.map((entry) => entry.id);
        this.running = true;
        this.cancelled = false;
        this.handedOff = false;
        this.driver = driver;
        this.requested.clear();
        let retained: FilesystemExportResult | null = null;
        const valid = () => !this.cancelled && !this.disposed && current();
        this.status('Preparing drag export', true);
        try {
            if (!valid()) return;
            const inspection = await driver.inspect(revision, ids);
            if (!valid()) return;
            if (inspection.revision !== revision) throw new Error('The image changed during drag preparation.');
            if (!inspection.entries.length || inspection.notices.length)
                throw new Error('This selection needs export review. Use Export to disk.');
            const job = await driver.execute(revision, ids, { kind: 'DOWNLOAD', directoryName: 'Files' }, (update) => {
                this.jobId = update.jobId;
                if (!valid()) this.cancelJob();
            });
            if (job.status === 'completed' && job.result) retained = job.result as FilesystemExportResult;
            if (!valid()) return;
            if (!retained || retained.destination !== 'DOWNLOAD' || !retained.download)
                throw new Error(job.error ?? 'The drag export did not complete.');
            if (retained.revision !== revision || retained.notices.length)
                throw new Error('The export changed. Review the selection through Export to disk.');
            this.ticket = await driver.drag.reserve(retained.download.sizeBytes);
            if (!valid()) return;
            await driver.drag.prepare(this.ticket, retained.download.contentPath);
            if (!valid()) return;
            this.handedOff = true;
            await driver.drag.start(this.ticket);
        } catch (error) {
            this.cancelJob();
            if (valid()) this.error(userFacingMessage(error));
        } finally {
            if (this.ticket) await driver.drag.cancel(this.ticket).catch(() => undefined);
            if (retained)
                await driver.release(retained).catch(() => {
                    if (!this.disposed) this.error('Export prepared; temporary server archive cleanup failed.');
                });
            this.ticket = null;
            this.jobId = null;
            this.driver = null;
            this.running = false;
            if (!this.disposed) this.status('', false);
        }
    }

    private cancelJob(): void {
        if (this.jobId === null || this.requested.has(this.jobId)) return;
        this.requested.add(this.jobId);
        void this.driver?.cancel(this.jobId).catch(() => undefined);
    }

    cancel(): void {
        if (!this.running || this.handedOff) return;
        this.cancelled = true;
        this.cancelJob();
        if (this.ticket) void this.driver?.drag?.cancel(this.ticket).catch(() => undefined);
        if (!this.disposed) this.status('Cancelling drag export', true);
    }

    dispose(): void {
        this.disposed = true;
        this.cancel();
    }
}
