import type { AxklibFilesystemImports } from './su700Actions';
import type { Su700Inspection, Su700Request, Su700ImportResult } from '../../../lib/su700Import';
import type { InputFileLocation } from '../../../lib/storageLocations';
import type { ClientFilesystemImportEntry } from '../../../lib/filesystemImport';
import type { JobState } from '../../../lib/transport';
import type { FilesController } from '../../files/controller.svelte';
import { userFacingMessage } from '../../../lib/userFacingMessage';
import { isFloppyCandidate } from './su700Drop';
import { AxklibApiError } from '../../../lib/httpErrors';

export class Su700Workflow {
    opened = $state(false);
    busy = $state(false);
    phase = $state<'review' | 'writing' | 'unconfirmed' | 'complete' | 'refresh-failed' | 'warnings'>('review');
    warnings = $state<string[]>([]);
    message = $state('Choose a floppy image');
    inspection = $state<Su700Inspection | null>(null);
    name = $state('');
    rootIndex = $state<number | null>(null);
    extras = $state<string[]>([]);
    source = $state<InputFileLocation | null>(null);
    private revision = 0;
    private jobId: number | null = null;
    private pendingWrite: Su700Request | null = null;
    private abort = new AbortController();
    private disposed = false;
    constructor(
        private readonly currentController: () => FilesController,
        readonly imports: AxklibFilesystemImports,
        readonly sessionId: number,
        private readonly refresh: () => Promise<void>,
        private readonly setStatus: (message: string) => void = () => undefined,
    ) {}
    get controller() {
        return this.currentController();
    }
    get roots() {
        return this.controller.roots.filter(
            (root) =>
                this.controller.capabilitiesFor(root.id)?.createDirectory &&
                this.controller.capabilitiesFor(root.id)?.putFile &&
                this.controller.capabilitiesFor(root.id)?.supportedImports.includes('SU700_FLOPPY'),
        );
    }
    get enabled() {
        return !!this.imports.su700 && this.roots.length > 0;
    }
    get validName() {
        return (
            /^[\x20-\x7e]{1,16}$/.test(this.name) &&
            this.name.trim() === this.name &&
            !/[\\/:*?"<>|]/.test(this.name) &&
            !['.', '..'].includes(this.name)
        );
    }
    get canReview() {
        return (
            this.opened &&
            !this.disposed &&
            !this.abort.signal.aborted &&
            !this.busy &&
            this.phase === 'review' &&
            this.validName &&
            !!this.source &&
            this.inspection?.status === 'COMPLETE' &&
            this.rootIndex !== null
        );
    }
    get canSubmit() {
        return this.canReview && !!this.inspection?.destinationReady;
    }
    get canDismiss() {
        return (
            this.phase !== 'unconfirmed' &&
            !(this.busy && ['complete', 'refresh-failed', 'warnings'].includes(this.phase))
        );
    }
    updateName(value: string) {
        this.name = value;
        this.invalidate();
    }
    updateRoot(value: number) {
        this.rootIndex = value;
        this.invalidate();
    }
    toggleExtra(path: string) {
        this.extras = this.extras.includes(path) ? this.extras.filter((item) => item !== path) : [...this.extras, path];
        this.invalidate();
    }
    private invalidate() {
        if (this.inspection) this.inspection = { ...this.inspection, destinationReady: false };
        this.message = 'Review the destination before importing';
    }
    private update = (job: JobState) => {
        this.jobId = job.jobId;
        if (this.abort.signal.aborted) void this.imports.su700?.cancel(job.jobId).catch(() => undefined);
    };
    private result<T>(job: JobState): T {
        if (job.status !== 'completed' || !job.result)
            throw new Error(
                job.error || (job.status === 'cancelled' ? 'Cancelled' : 'The operation did not complete.'),
            );
        return job.result as T;
    }
    private request(destination: boolean): Su700Request {
        if (!this.source) throw new Error('Choose a source image.');
        const root = this.rootIndex === null ? null : this.roots[this.rootIndex];
        return {
            source: this.source,
            includedExtras: destination ? this.extras : null,
            destination:
                destination && root
                    ? {
                          sessionId: this.sessionId,
                          expectedRevision: this.revision,
                          rootEntryId: root.id,
                          volumeName: this.name,
                      }
                    : null,
        };
    }
    async open(entries?: ClientFilesystemImportEntry[]): Promise<boolean> {
        if (this.disposed || !this.enabled || this.opened) return false;
        if (entries && !(await isFloppyCandidate(entries))) return false;
        if (this.disposed || !this.enabled || this.opened) return true;
        this.opened = true;
        this.busy = true;
        this.abort = new AbortController();
        this.phase = 'review';
        this.warnings = [];
        this.pendingWrite = null;
        this.inspection = null;
        this.revision = this.controller.revision;
        this.rootIndex = Math.max(
            0,
            this.roots.findIndex((root) => root.id === this.controller.rootId),
        );
        this.message = entries ? 'Reading floppy image' : 'Choose one floppy image';
        try {
            const sources = entries
                ? await this.imports.upload(
                      entries.flatMap((entry) => (entry.directory ? [] : [entry.source])),
                      this.abort.signal,
                      (message) => {
                          this.message = message;
                      },
                  )
                : await this.imports.chooseFiles('Import SU700 floppy');
            if (!sources?.length) {
                this.opened = false;
                return true;
            }
            if (sources.length !== 1) {
                await this.imports.release(sources);
                throw new Error('Choose one complete SU700 floppy image. Disk sets are not supported.');
            }
            this.source = sources[0];
            this.abort.signal.throwIfAborted();
            this.message = 'Inspecting SU700 contents';
            const result = this.result<Su700Inspection>(
                await this.imports.su700!.run(this.request(false), this.update),
            );
            this.abort.signal.throwIfAborted();
            if (result.status === 'UNRELATED' && entries) {
                await this.release();
                this.opened = false;
                return false;
            }
            this.inspection = result;
            this.name = result.suggestedVolumeName;
            this.extras = result.files.filter((file) => file.role === 'EXTRA').map((file) => file.sourcePath);
            this.message =
                result.status === 'COMPLETE'
                    ? 'Review the destination before importing'
                    : result.issue || 'This is not a supported SU700 floppy.';
        } catch (error) {
            this.message = userFacingMessage(error);
        } finally {
            this.busy = false;
            this.jobId = null;
            if (this.disposed || this.abort.signal.aborted) {
                await this.release();
                this.opened = false;
            }
        }
        return true;
    }
    async review(): Promise<void> {
        if (!this.canReview) return;
        this.busy = true;
        this.message = 'Checking destination and free space';
        try {
            this.inspection = this.result<Su700Inspection>(
                await this.imports.su700!.run(this.request(true), this.update),
            );
            this.message = this.inspection.destinationReady
                ? 'Ready to import into a new volume'
                : this.inspection.issue || 'Import is not available';
        } catch (error) {
            this.invalidate();
            this.message = userFacingMessage(error);
        } finally {
            this.busy = false;
            this.jobId = null;
            if (this.disposed || this.abort.signal.aborted) {
                await this.release();
                this.opened = false;
            }
        }
    }
    async submit(): Promise<void> {
        if (!this.canSubmit || !this.inspection) return;
        const snapshot = JSON.stringify(this.inspection.snapshot);
        const files = JSON.stringify(this.inspection.files);
        await this.review();
        if (!this.canSubmit || !this.inspection) return;
        if (snapshot !== JSON.stringify(this.inspection.snapshot) || files !== JSON.stringify(this.inspection.files)) {
            this.message = 'Source contents changed. Review the updated contents and confirm again.';
            this.extras = this.inspection.files.filter((file) => file.role === 'EXTRA').map((file) => file.sourcePath);
            this.inspection = { ...this.inspection, destinationReady: false };
            return;
        }
        this.phase = 'writing';
        this.busy = true;
        this.message = 'Importing';
        this.pendingWrite = {
            ...this.request(true),
            expectedSource: this.inspection.snapshot,
            idempotencyKey: crypto.randomUUID(),
        };
        try {
            const job = await this.imports.su700!.run(this.pendingWrite, this.update);
            await this.finish(job);
        } catch (error) {
            this.phase = 'unconfirmed';
            this.message = `Write outcome unconfirmed. ${userFacingMessage(error)}`;
            if (
                !this.jobId &&
                error instanceof AxklibApiError &&
                error.status >= 400 &&
                error.status < 500 &&
                error.status !== 408
            ) {
                this.phase = 'complete';
                this.message = userFacingMessage(error);
                this.pendingWrite = null;
                await this.release();
            }
        } finally {
            this.busy = false;
        }
    }
    async checkStatus() {
        if ((!this.jobId && !this.pendingWrite) || this.busy) return;
        this.busy = true;
        try {
            const job = this.jobId
                ? await this.imports.su700!.observe(this.jobId, this.update)
                : await this.imports.su700!.run(this.pendingWrite!, this.update);
            await this.finish(job);
        } catch (error) {
            this.message = userFacingMessage(error);
        } finally {
            this.busy = false;
        }
    }
    private async finish(job: JobState) {
        if (!['completed', 'failed', 'cancelled'].includes(job.status)) {
            this.phase = 'unconfirmed';
            this.message = 'Write outcome unconfirmed';
            return;
        }
        await this.release();
        this.pendingWrite = null;
        if (job.status !== 'completed') {
            this.phase = 'complete';
            this.message = job.error || 'Import cancelled';
            return;
        }
        this.phase = 'complete';
        this.message = `Imported into ${this.name}`;
        this.warnings = (job.result as Su700ImportResult | undefined)?.warnings ?? [];
        await this.retryRefresh();
    }
    async retryRefresh() {
        this.busy = true;
        try {
            await this.refresh();
            this.phase = this.warnings.length ? 'warnings' : 'complete';
            this.message = `Imported into ${this.name}${this.warnings.length ? ' with warnings' : ''}`;
            this.setStatus(this.message);
            this.opened = this.warnings.length > 0;
        } catch (error) {
            this.phase = 'refresh-failed';
            this.message = `Import saved; refresh failed. ${userFacingMessage(error)}`;
        } finally {
            this.busy = false;
        }
    }
    async close() {
        if (!this.canDismiss) return;
        if (this.busy) {
            this.abort.abort();
            if (this.jobId)
                await this.imports.su700?.cancel(this.jobId).catch((error) => {
                    this.message = userFacingMessage(error);
                });
            return;
        }
        this.opened = false;
        await this.release();
    }
    private async release() {
        const source = this.source;
        if (source) await this.imports.release([source]);
        this.source = null;
    }
    dispose() {
        this.disposed = true;
        void this.close();
    }
}
