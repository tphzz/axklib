import { AxklibApiError } from '../../lib/httpErrors';
import type { ImageTransport, JobState } from '../../lib/transport';
import { userFacingMessage } from '../../lib/userFacingMessage';
import { objectEditorAdapter } from './registry';
import type { ObjectEditorDocument } from './workflow.svelte';

interface Dependencies {
    transport: Pick<ImageTransport, 'waitForJob'> & Partial<Pick<ImageTransport, 'startSampleDuplication'>>;
    load: (sessionId: number, id: string) => Promise<ObjectEditorDocument | null>;
    check: (document: ObjectEditorDocument) => Promise<boolean>;
    refresh: () => Promise<void>;
    stop: () => void;
    status: (message: string) => void;
    otherWritePending: () => boolean;
}

export class SampleDuplication {
    visible = $state(false);
    name = $state('');
    source = $state.raw<ObjectEditorDocument | null>(null);
    phase = $state<'loading' | 'editable' | 'checking' | 'writing' | 'unconfirmed' | 'refresh-failed'>('editable');
    message = $state('');
    private existingNames = new Set<string>();
    private generation = 0;
    private jobId: number | null = null;
    private submittedName = '';
    private oncreated: (name: string) => Promise<void> = async () => {};
    constructor(private readonly dependencies: Dependencies) {}

    get locked(): boolean {
        return this.visible && !['editable', 'loading'].includes(this.phase);
    }
    get canCheck(): boolean {
        return this.phase === 'unconfirmed' && this.jobId !== null;
    }
    get validation(): string {
        if (!this.source) return this.message || 'Loading Sample parameters';
        if (!this.dependencies.transport.startSampleDuplication) return 'Sample duplication is unavailable';
        if (this.dependencies.otherWritePending() || this.source.phase !== 'editable')
            return 'Resolve the pending Sample save first';
        if (this.source.validation) return this.source.validation;
        if (!this.source.detail?.editing?.canEditPlayback)
            return 'Duplication requires matching stereo playback windows and sample rates';
        const name = this.name.trim();
        if (!/^[\x20-\x7e]{1,16}$/.test(name)) return 'Use 1-16 printable ASCII characters';
        if (this.existingNames.has(name.toLowerCase())) return 'A Sample with this name already exists in this volume';
        return '';
    }
    get canSubmit(): boolean {
        return this.visible && this.phase === 'editable' && !this.validation;
    }

    async open(
        sessionId: number,
        objectId: string,
        existingNames: string[],
        oncreated: (name: string) => Promise<void>,
    ): Promise<void> {
        if (this.locked || this.dependencies.otherWritePending()) return;
        const generation = ++this.generation;
        this.visible = true;
        this.source = null;
        this.name = '';
        this.phase = 'loading';
        this.message = 'Loading Sample parameters';
        this.existingNames = new Set(existingNames.map((name) => name.toLowerCase()));
        this.jobId = null;
        this.oncreated = oncreated;
        try {
            const source = await this.dependencies.load(sessionId, objectId);
            if (generation !== this.generation) return;
            this.source = source;
            if (!source) this.message = 'Duplication is unavailable for this Sample format';
            else {
                const stem = source.detail!.object.name;
                this.existingNames.add(stem.toLowerCase());
                for (let index = 1; index <= 10000; index++) {
                    const suffix = index === 1 ? ' Copy' : ` Copy ${index}`;
                    const candidate = `${stem.slice(0, 16 - suffix.length).trimEnd()}${suffix}`;
                    if (!this.existingNames.has(candidate.toLowerCase())) {
                        this.name = candidate;
                        break;
                    }
                }
                this.message = '';
            }
        } catch (error) {
            if (generation === this.generation) this.message = userFacingMessage(error);
        } finally {
            if (generation === this.generation) this.phase = 'editable';
        }
    }
    close(): void {
        if (this.locked) return;
        this.generation++;
        this.visible = false;
        this.source = null;
    }
    async submit(): Promise<void> {
        if (!this.canSubmit || !this.source) return;
        const source = this.source;
        this.phase = 'checking';
        this.message = 'Checking Sample';
        this.dependencies.stop();
        try {
            if (!(await this.dependencies.check(source)) || this.validation) {
                this.phase = 'editable';
                this.message = this.validation;
                return;
            }
        } catch (error) {
            this.phase = 'editable';
            this.message = userFacingMessage(error);
            return;
        }
        const edit = objectEditorAdapter(source.detail!)!.edit(
            source.detail!,
            source.draft.changes,
            source.draft.values,
        );
        if (edit.operation.type !== 'update_sbnk_parameters') {
            this.phase = 'editable';
            this.message = 'Only Samples can be duplicated here';
            return;
        }
        this.submittedName = this.name.trim();
        this.phase = 'writing';
        this.message = 'Duplicating Sample';
        try {
            const job = await this.dependencies.transport.startSampleDuplication!(source.sessionId, {
                expectedRevision: edit.expectedRevision,
                operation: {
                    ...edit.operation,
                    id: 'sample-duplicate',
                    type: 'duplicate_sbnk',
                    new_name: this.submittedName,
                },
            });
            this.jobId = job.jobId;
            await this.accept(await this.dependencies.transport.waitForJob(job.jobId, () => {}));
        } catch (error) {
            if (
                this.jobId === null &&
                error instanceof AxklibApiError &&
                error.status >= 400 &&
                error.status < 500 &&
                error.status !== 408
            ) {
                this.phase = 'editable';
                this.message = userFacingMessage(error);
            } else this.unconfirmed(error);
        }
    }
    async recover(): Promise<void> {
        if (this.phase === 'refresh-failed') return this.finish();
        if (!this.canCheck) return;
        this.phase = 'writing';
        this.message = 'Checking duplication status';
        try {
            await this.accept(await this.dependencies.transport.waitForJob(this.jobId!, () => {}));
        } catch (error) {
            this.unconfirmed(error);
        }
    }
    private unconfirmed(error: unknown): void {
        this.phase = 'unconfirmed';
        this.message = `Duplication outcome unconfirmed${this.jobId === null ? '; do not repeat the write' : ''}: ${userFacingMessage(error)}`;
    }
    private async accept(job: JobState): Promise<void> {
        if (job.status === 'completed') await this.finish();
        else if (job.status === 'failed' || job.status === 'cancelled') {
            this.phase = 'editable';
            this.jobId = null;
            this.message = job.error ?? 'Sample was not duplicated';
        } else this.unconfirmed(new Error('The job has not completed'));
    }
    private async finish(): Promise<void> {
        this.phase = 'writing';
        this.message = 'Sample duplicated; refreshing workspace';
        try {
            await this.dependencies.refresh();
            await this.oncreated(this.submittedName);
            this.dependencies.status(`Duplicated Sample ${this.submittedName}`);
            this.phase = 'editable';
            this.close();
        } catch (error) {
            this.phase = 'refresh-failed';
            this.message = `Sample duplicated; refresh failed: ${userFacingMessage(error)}`;
        }
    }
}
