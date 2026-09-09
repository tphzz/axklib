import type {
    FilesystemEntry,
    FilesystemMutationDriver,
    FilesystemRootCapabilities,
    FilesystemEditResult,
} from '../../lib/filesystem';
import type {
    FilesystemDropReader,
    FilesystemImportActions,
    FilesystemImportSourceEntry,
} from '../../lib/filesystemImport';
import type { InputFileLocation } from '../../lib/storageLocations';
import type { ClientUploadSource } from '../../lib/clientUploadSource';
import type { JobState } from '../../lib/transport';
import { userFacingMessage } from '../../lib/userFacingMessage';
import { validFilesystemName } from './editWorkflow.svelte';
import {
    fileSources,
    importPaths,
    sourceRows,
    importEdits,
    inspectedDestination,
    inspectedSources,
    type FilesImportRow,
} from './importReview';

type Phase =
    | 'choosing'
    | 'picking'
    | 'scanning'
    | 'uploading'
    | 'inspecting'
    | 'dirty'
    | 'ready'
    | 'writing'
    | 'refreshing'
    | 'checking'
    | 'warnings'
    | 'failed'
    | 'write-failed'
    | 'unconfirmed'
    | 'refresh-failed';

export class FilesImportWorkflow {
    target = $state<FilesystemEntry | null>(null);
    rows = $state<FilesImportRow[]>([]);
    phase = $state<Phase>('choosing');
    message = $state('Choose files');
    warnings = $state<string[]>([]);
    private releasedAfterWrite = false;
    jobId = $state<number | null>(null);
    cancelling = $state(false);
    sourceKind = $state<'files' | 'directory-contents' | 'drop'>('files');
    capabilities = $state<FilesystemRootCapabilities | null>(null);
    private revision = 0;
    private imports!: FilesystemImportActions;
    private mutations!: FilesystemMutationDriver;
    private abort: AbortController | null = null;
    private generation = 0;
    private disposed = false;
    constructor(private readonly setStatus: (message: string) => void = () => undefined) {}

    get busy(): boolean {
        return ['picking', 'scanning', 'uploading', 'inspecting', 'writing', 'refreshing', 'checking'].includes(
            this.phase,
        );
    }
    get canDismiss(): boolean {
        return (
            !['refreshing', 'checking', 'unconfirmed'].includes(this.phase) &&
            !(this.phase === 'writing' && (this.jobId === null || this.cancelling))
        );
    }
    get editable(): boolean {
        return (
            !!this.target &&
            !this.busy &&
            !['unconfirmed', 'refresh-failed', 'write-failed', 'warnings'].includes(this.phase)
        );
    }
    get canInspect(): boolean {
        return (
            this.editable &&
            this.rows.length > 0 &&
            this.rows.every((row) => (row.directory || !!row.snapshot) && this.validName(row.name))
        );
    }
    get canSubmit(): boolean {
        return (
            this.phase === 'ready' &&
            this.canInspect &&
            this.rows.every((row) => !!row.decision && row.decision.action !== 'CONFLICT') &&
            this.rows.some((row) =>
                ['CREATE_FILE', 'REPLACE_FILE', 'CREATE_DIRECTORY'].includes(row.decision?.action ?? ''),
            )
        );
    }
    get supportsClientUploads(): boolean {
        return this.imports?.supportsClientUploads ?? false;
    }
    get directoryMode(): boolean {
        return this.sourceKind === 'directory-contents' || this.rows.some((row) => row.directory);
    }
    validName(name: string): boolean {
        return !!this.capabilities && validFilesystemName(name, this.capabilities);
    }

    open(
        revision: number,
        target: FilesystemEntry,
        capabilities: FilesystemRootCapabilities,
        imports: FilesystemImportActions,
        mutations: FilesystemMutationDriver,
    ): boolean {
        if (
            this.disposed ||
            this.target ||
            revision < 1 ||
            !capabilities.putFile ||
            target.kind === 'file' ||
            target.rootId !== capabilities.rootId ||
            target.filesystemMetadata ||
            target.issue
        )
            return false;
        this.target = { ...target };
        this.capabilities = { ...capabilities };
        this.revision = revision;
        this.imports = imports;
        this.mutations = mutations;
        this.rows = [];
        this.phase = 'choosing';
        this.warnings = [];
        this.releasedAfterWrite = false;
        this.message = 'Choose files';
        this.jobId = null;
        this.cancelling = false;
        this.sourceKind = 'files';
        this.generation += 1;
        return true;
    }

    async chooseWorkspace(): Promise<void> {
        if (!this.editable) return;
        this.sourceKind = 'files';
        await this.acquire(async () => this.files(await this.imports.chooseFiles()), 'picking');
    }

    async chooseDirectory(): Promise<void> {
        if (!this.editable || !this.capabilities?.createDirectory) return;
        this.sourceKind = 'directory-contents';
        const abort = new AbortController();
        this.abort = abort;
        const generation = this.generation;
        await this.acquire(
            () =>
                this.imports.chooseDirectory(abort.signal, (message) => {
                    if (this.current(generation)) {
                        this.phase = 'scanning';
                        this.message = message;
                    }
                }),
            'picking',
        );
        if (this.abort === abort) this.abort = null;
    }

    private files(sources: InputFileLocation[] | null): FilesystemImportSourceEntry[] | null {
        return (
            sources?.map((source) => ({
                directory: false,
                relativePath: [
                    source.kind === 'server-file'
                        ? source.reference.relativePath.split('/').at(-1)!
                        : source.displayName,
                ],
                source,
            })) ?? null
        );
    }

    async chooseLocal(files: ClientUploadSource[]): Promise<void> {
        if (!this.editable || !files.length || !this.supportsClientUploads) return;
        this.sourceKind = 'files';
        const abort = new AbortController();
        this.abort = abort;
        const generation = this.generation;
        await this.acquire(
            async () =>
                this.files(
                    await this.imports.upload(files, abort.signal, (message) => {
                        if (this.current(generation)) this.message = message;
                    }),
                ),
            'uploading',
        );
        if (this.abort === abort) this.abort = null;
    }

    async chooseDropped(read: FilesystemDropReader): Promise<void> {
        if (!this.editable || !this.supportsClientUploads) return;
        this.sourceKind = 'drop';
        const abort = new AbortController();
        this.abort = abort;
        const generation = this.generation;
        const imports = this.imports;
        await this.acquire(async () => {
            const progress = (message: string) => {
                if (this.current(generation)) this.message = message;
            };
            const entries = await read(abort.signal, progress);
            abort.signal.throwIfAborted();
            if (!this.current(generation)) return null;
            if (entries.length > 10000) throw new Error('Choose at most 10000 entries.');
            if (entries.some((entry) => entry.directory) && !this.capabilities?.createDirectory)
                throw new Error('Directory creation is unavailable for this destination.');
            const files = entries.flatMap((entry) => (entry.directory ? [] : [entry.source]));
            this.phase = 'uploading';
            const uploaded = files.length ? await imports.upload(files, abort.signal, progress) : [];
            if (abort.signal.aborted || !this.current(generation) || uploaded.length !== files.length) {
                await imports.release(uploaded);
                abort.signal.throwIfAborted();
                if (!this.current(generation)) return null;
                throw new Error('The uploaded files do not match the dropped selection.');
            }
            let index = 0;
            return entries.map((entry) => (entry.directory ? entry : { ...entry, source: uploaded[index++] }));
        }, 'scanning');
        if (this.abort === abort) this.abort = null;
    }

    private async acquire(
        operation: () => Promise<FilesystemImportSourceEntry[] | null>,
        phase: 'picking' | 'uploading' | 'scanning',
    ): Promise<void> {
        const generation = this.generation;
        const imports = this.imports;
        this.phase = phase;
        this.message =
            phase === 'picking'
                ? 'Choosing files'
                : phase === 'scanning'
                  ? 'Reading dropped entries'
                  : 'Uploading files';
        this.jobId = null;
        try {
            const entries = await operation();
            const sources = entries?.flatMap((entry) => (entry.directory ? [] : [entry.source])) ?? [];
            if (!this.current(generation)) {
                await imports.release(sources);
                return;
            }
            if (entries === null) {
                this.phase = this.rows.length ? 'dirty' : 'choosing';
                this.message = this.rows.length ? 'Selection unchanged. Review the entries' : 'Choose files';
                return;
            }
            if (entries.length > 10000) {
                await imports.release(sources);
                throw new Error('Choose at most 10000 entries.');
            }
            const previous = fileSources(this.rows);
            try {
                this.rows = sourceRows(entries);
            } catch (error) {
                await imports.release(sources);
                throw error;
            }
            await imports.release(previous);
            if (!this.current(generation)) return;
            this.phase = 'inspecting';
            this.message = 'Inspecting sources';
            if (sources.length) {
                const job = await imports.inspectInputs(sources, this.update(generation));
                if (!this.current(generation)) return;
                this.rows = inspectedSources(job, this.rows);
            }
            this.phase = 'dirty';
            if (this.canInspect) await this.inspect();
            else
                this.message = this.rows.length
                    ? 'Correct the destination names before reviewing'
                    : 'The selected directory contains no importable entries';
        } catch (error) {
            this.fail(error, generation);
        }
    }

    rename(index: number, name: string): void {
        if (!this.editable || !this.rows[index]) return;
        this.rows[index].name = name;
        this.invalidate();
    }
    setConflict(index: number, conflict: 'SKIP' | 'REPLACE'): void {
        if (!this.editable || !this.rows[index] || this.rows[index].directory) return;
        this.rows[index].conflict = conflict;
        this.invalidate();
    }
    setAllConflicts(conflict: 'SKIP' | 'REPLACE'): void {
        if (!this.editable) return;
        for (const row of this.rows) if (!row.directory) row.conflict = conflict;
        this.invalidate();
    }
    private invalidate(): void {
        this.rows = this.rows.map((row) => ({ ...row, decision: null }));
        this.phase = 'dirty';
        this.message = 'Review the updated entries';
    }

    async inspect(): Promise<void> {
        if (!this.canInspect || !this.target) return;
        const generation = this.generation;
        this.phase = 'inspecting';
        this.message = 'Reviewing destination';
        this.jobId = null;
        this.cancelling = false;
        try {
            const paths = importPaths(this.rows);
            const entries = this.rows.map((row, index) => ({
                relativePath: paths[index],
                directory: row.directory,
                sizeBytes: row.directory ? 0 : row.snapshot!.sizeBytes,
                conflict: row.conflict,
            }));
            const job = await this.imports.inspectDestination(
                this.revision,
                this.target.id,
                entries,
                this.update(generation),
            );
            if (!this.current(generation)) return;
            this.rows = inspectedDestination(job, this.revision, this.target.id, this.rows);
            this.phase = 'ready';
            const conflicts = this.rows.filter((row) => row.decision?.action === 'CONFLICT').length;
            const changes = this.rows.filter((row) =>
                ['CREATE_FILE', 'REPLACE_FILE', 'CREATE_DIRECTORY'].includes(row.decision?.action ?? ''),
            ).length;
            this.message = conflicts
                ? `${conflicts} blocking conflicts`
                : changes
                  ? `${changes} ${this.rows.some((row) => row.directory) ? (changes === 1 ? 'entry' : 'entries') : changes === 1 ? 'file' : 'files'} ready to import`
                  : 'No changes to import';
        } catch (error) {
            this.fail(error, generation);
        } finally {
            if (this.current(generation)) this.cancelling = false;
        }
    }

    async submit(): Promise<void> {
        if (!this.target || this.disposed || this.busy) return;
        if (this.phase === 'refresh-failed' || this.phase === 'write-failed') {
            await this.refresh(this.phase === 'refresh-failed');
            return;
        }
        const observe = this.phase === 'unconfirmed' && this.jobId !== null;
        if (!observe && !this.canSubmit) return;
        const generation = this.generation;
        const jobId = this.jobId;
        this.phase = observe ? 'checking' : 'writing';
        this.message = observe ? 'Checking status' : 'Importing';
        this.cancelling = false;
        if (!observe) this.jobId = null;
        try {
            const job = observe
                ? await this.mutations.observe(jobId!, this.update(generation, true))
                : await this.mutations.execute(
                      this.revision,
                      importEdits(this.target.id, this.rows),
                      this.update(generation, true),
                  );
            if (!this.current(generation)) {
                if (['completed', 'failed', 'cancelled'].includes(job.status)) await this.release();
                return;
            }
            if (job.status === 'completed') {
                this.warnings = (job.result as FilesystemEditResult | undefined)?.warnings ?? [];
                await this.refresh();
            } else {
                this.phase = 'write-failed';
                this.message = job.status === 'cancelled' ? 'Cancelled' : job.error || 'Import failed';
            }
        } catch (error) {
            if (this.current(generation)) {
                this.phase = 'unconfirmed';
                this.message = `Write outcome unconfirmed. ${userFacingMessage(error)}`;
            }
        } finally {
            if (this.current(generation)) this.cancelling = false;
        }
    }

    private async refresh(committed = true): Promise<void> {
        this.phase = 'refreshing';
        this.message = committed ? 'Changes saved. Refreshing' : 'Refreshing';
        try {
            if (committed && !this.releasedAfterWrite) {
                await this.imports.release(fileSources(this.rows));
                this.releasedAfterWrite = true;
            }
            await this.mutations.refresh();
            if (committed && this.warnings.length) {
                this.phase = 'warnings';
                this.message = 'Imported files with warnings';
                this.setStatus(this.message);
                return;
            }
            if (committed) this.setStatus('Imported files');
            this.phase = 'ready';
            this.close();
        } catch (error) {
            this.phase = committed ? 'refresh-failed' : 'write-failed';
            this.message = `${committed ? 'Changes saved; refresh failed. ' : 'Refresh failed. '}${userFacingMessage(error)}`;
        }
    }
    private update(generation: number, writing = false): (job: JobState) => void {
        const driver = writing ? this.mutations : this.imports;
        return (job) => {
            if (!this.current(generation)) {
                void driver.cancel(job.jobId).catch(() => undefined);
                return;
            }
            this.jobId = job.jobId;
            if (job.progress?.label) this.message = job.progress.label;
        };
    }
    private current(generation: number): boolean {
        return !this.disposed && !!this.target && generation === this.generation;
    }
    private fail(error: unknown, generation: number): void {
        if (this.current(generation)) {
            this.phase = 'failed';
            this.message = userFacingMessage(error);
            this.cancelling = false;
        }
    }
    private async release(): Promise<void> {
        const sources = fileSources(this.rows);
        this.rows = [];
        if (!this.releasedAfterWrite) await this.imports.release(sources).catch(() => undefined);
    }
    close(): void {
        if (!this.target) return;
        if (['writing', 'refreshing', 'checking', 'unconfirmed'].includes(this.phase)) return;
        if (this.phase === 'inspecting' && this.jobId !== null)
            void this.imports.cancel(this.jobId).catch(() => undefined);
        this.abort?.abort();
        this.target = null;
        this.generation += 1;
        void this.release();
    }
    async cancel(): Promise<void> {
        if (this.cancelling || !['writing', 'inspecting', 'uploading', 'scanning'].includes(this.phase)) return;
        this.cancelling = true;
        this.abort?.abort();
        try {
            if (this.jobId !== null)
                await (this.phase === 'writing' ? this.mutations : this.imports).cancel(this.jobId);
            this.message = 'Cancellation requested';
        } catch (error) {
            this.message = userFacingMessage(error);
            this.cancelling = false;
        }
    }
    dispose(): void {
        if (this.phase === 'writing' && this.jobId !== null)
            void this.mutations.cancel(this.jobId).catch(() => undefined);
        this.close();
        this.disposed = true;
    }
}
