import { EditorDraft } from './draft.svelte';
import { EditorNavigation } from './navigation.svelte';
import { EditorComparison } from './comparison.svelte';
import { SampleDuplication } from './duplication.svelte';
import { objectEditorAdapter } from './registry';
import { AxklibApiError } from '../../lib/httpErrors';
import { userFacingMessage } from '../../lib/userFacingMessage';
import { reportDiagnostic } from '../../lib/diagnostics';
import type { ImageTransport, ObjectDetail, JobState } from '../../lib/transport';
import type { SampleStorageFormat } from '../../lib/objectEditing';

export class ObjectEditorDocument {
    detail = $state.raw<ObjectDetail>();
    draft: EditorDraft;
    conflict = $state('');
    status = $state('');
    phase = $state<'editable' | 'saving' | 'unconfirmed' | 'refresh-failed'>('editable');
    inputErrors = $state<Record<string, string>>({});
    jobId: number | null = null;
    writeKind: 'save' | 'conversion' = 'save';
    conversionTarget: SampleStorageFormat | null = null;
    constructor(
        readonly sessionId: number,
        detail: ObjectDetail,
        readonly preferencesScope: object = {},
    ) {
        this.detail = detail;
        this.draft = new EditorDraft(objectEditorAdapter(detail)!.values(detail.editing!));
    }
    get validation(): string {
        return (
            this.conflict ||
            Object.values(this.inputErrors).find(Boolean) ||
            objectEditorAdapter(this.detail!)!.validate(this.draft.values, this.draft.changes, this.detail!.editing!)
        );
    }
    get canSave(): boolean {
        return this.phase === 'editable' && this.draft.dirty && !this.validation;
    }
}

type Dependencies = {
    transport: Pick<ImageTransport, 'objectDetail' | 'startObjectParameterEdit' | 'waitForJob'> &
        Partial<Pick<ImageTransport, 'startSampleDuplication' | 'startSampleFormatConversion'>>;
    refresh: () => Promise<void>;
    stopPlayback: () => void;
    status: (message: string) => void;
    audition?: (document: ObjectEditorDocument, note: number) => Promise<void>;
};

export class ObjectEditorWorkflow {
    visible = $state(false);
    documents = $state.raw<ObjectEditorDocument[]>([]);
    conversionDocument = $state.raw<ObjectEditorDocument | null>(null);
    private loading = new Map<string, Promise<ObjectEditorDocument | null>>();
    private generation = 0;
    private navigations = new Map<string, EditorNavigation>();
    private preferenceScopes = new Map<number, object>();
    readonly comparison: EditorComparison;
    readonly duplication: SampleDuplication;
    constructor(private readonly dependencies: Dependencies) {
        this.comparison = new EditorComparison(dependencies.transport, (sessionId, id) => this.find(sessionId, id));
        this.duplication = new SampleDuplication({
            transport: dependencies.transport,
            load: (sessionId, id) => this.load(sessionId, id),
            check: (document) => this.check(document),
            refresh: dependencies.refresh,
            stop: dependencies.stopPlayback,
            status: dependencies.status,
            otherWritePending: () => this.documents.some((item) => item.phase !== 'editable'),
        });
    }
    navigation(profile: string): EditorNavigation {
        let navigation = this.navigations.get(profile);
        if (!navigation) {
            navigation = new EditorNavigation();
            this.navigations.set(profile, navigation);
        }
        return navigation;
    }
    get dirtyCount(): number {
        return this.documents.filter((item) => item.draft.dirty).length;
    }
    get locked(): boolean {
        return this.duplication.locked || this.documents.some((item) => item.phase !== 'editable');
    }
    play(document: ObjectEditorDocument, note: number): void {
        if (document.validation || document.phase !== 'editable') return;
        void this.dependencies
            .audition?.(document, note)
            .catch((error) => (document.status = userFacingMessage(error)));
    }
    stop(): void {
        this.dependencies.stopPlayback();
    }
    find(sessionId: number, objectId: string): ObjectEditorDocument | undefined {
        return this.documents.find((item) => item.sessionId === sessionId && item.detail?.object.id === objectId);
    }
    async load(sessionId: number, objectId: string): Promise<ObjectEditorDocument | null> {
        const existing = this.find(sessionId, objectId);
        if (existing) return existing;
        const key = `${sessionId}:${objectId}`;
        const pending = this.loading.get(key);
        if (pending) return pending;
        const generation = this.generation;
        const task = this.dependencies.transport
            .objectDetail(sessionId, objectId)
            .then((detail) => {
                if (generation !== this.generation) return null;
                if (!objectEditorAdapter(detail)) return null;
                const scope = this.preferenceScopes.get(sessionId) ?? {};
                this.preferenceScopes.set(sessionId, scope);
                const document = new ObjectEditorDocument(sessionId, detail, scope);
                this.documents = [...this.documents, document];
                return document;
            })
            .finally(() => {
                if (this.loading.get(key) === task) this.loading.delete(key);
            });
        this.loading.set(key, task);
        return task;
    }
    async check(document: ObjectEditorDocument): Promise<boolean> {
        const current = await this.dependencies.transport.objectDetail(document.sessionId, document.detail!.object.id);
        const old = document.detail!;
        if (
            !objectEditorAdapter(current) ||
            current.object.key !== old.object.key ||
            current.editing?.payloadSha256 !== old.editing?.payloadSha256 ||
            current.editing?.volumeName !== old.editing?.volumeName ||
            current.object.name !== old.object.name
        ) {
            document.conflict = 'This Sample changed outside the editor. Discard the draft to reload it.';
            return false;
        }
        document.detail = current;
        return true;
    }
    async revalidate(sessionId: number): Promise<void> {
        for (const document of this.documents.filter(
            (item) => item.sessionId === sessionId && item.phase === 'editable',
        )) {
            try {
                await this.check(document);
            } catch (error) {
                document.conflict = userFacingMessage(error);
            }
        }
    }
    async discard(document: ObjectEditorDocument): Promise<void> {
        if (document.phase !== 'editable') return;
        const detail = await this.dependencies.transport.objectDetail(document.sessionId, document.detail!.object.id);
        const adapter = objectEditorAdapter(detail);
        if (!adapter) throw new Error('This object no longer supports this editor');
        document.detail = detail;
        document.draft.accept(adapter.values(detail.editing!));
        document.conflict = '';
        document.status = '';
        this.dependencies.stopPlayback();
    }
    async save(document: ObjectEditorDocument): Promise<void> {
        if (!document.canSave || this.locked) return;
        document.phase = 'saving';
        document.status = 'Checking Sample';
        this.dependencies.stopPlayback();
        try {
            if (!(await this.check(document)) || document.validation) {
                document.phase = 'editable';
                return;
            }
        } catch (error) {
            document.phase = 'editable';
            document.status = userFacingMessage(error);
            return;
        }
        document.writeKind = 'save';
        document.conversionTarget = null;
        const edit = objectEditorAdapter(document.detail!)!.edit(
            document.detail!,
            document.draft.changes,
            document.draft.values,
        );
        await this.submit(document, () =>
            this.dependencies.transport.startObjectParameterEdit(document.sessionId, edit),
        );
    }
    conversionReason(document: ObjectEditorDocument): string {
        return document.draft.dirty
            ? 'Save or discard this Sample draft before converting.'
            : document.conflict ||
                  (!document.detail?.editing?.canConvertFormat
                      ? 'This Sample cannot be converted in this image.'
                      : !this.dependencies.transport.startSampleFormatConversion
                        ? 'Sample conversion is unavailable.'
                        : '');
    }
    async openConversion(sessionId: number, id: string): Promise<void> {
        if (this.locked) return;
        try {
            const document = await this.load(sessionId, id);
            if (document && !this.locked) {
                this.conversionDocument = document;
                document.status = '';
            }
        } catch (error) {
            this.dependencies.status(userFacingMessage(error));
        }
    }
    closeConversion(): void {
        if (this.conversionDocument?.phase === 'editable') this.conversionDocument = null;
    }
    async convert(document: ObjectEditorDocument, target: Exclude<SampleStorageFormat, 'UNKNOWN'>): Promise<void> {
        if (this.locked || this.conversionReason(document)) return;
        document.phase = 'saving';
        document.status = 'Checking Sample format';
        this.dependencies.stopPlayback();
        try {
            if (!(await this.check(document))) {
                document.phase = 'editable';
                return;
            }
            const snapshot = document.detail!.editing!;
            const preview = snapshot.formatConversions.find((item) => item.targetFormat === target);
            if (this.conversionReason(document) || !preview?.allowed) {
                document.phase = 'editable';
                document.status =
                    this.conversionReason(document) || 'Resolve the conversion blockers before converting.';
                return;
            }
        } catch (error) {
            document.phase = 'editable';
            document.status = userFacingMessage(error);
            return;
        }
        document.writeKind = 'conversion';
        document.conversionTarget = target;
        const detail = document.detail!;
        const snapshot = detail.editing!;
        await this.submit(document, () =>
            this.dependencies.transport.startSampleFormatConversion!(document.sessionId, {
                expectedRevision: detail.image.revision,
                operation: {
                    id: 'sample-format',
                    type: 'convert_sbnk_format',
                    target_format: target === 'A3000_188' ? 'a3000_188' : 'a4000_a5000_224',
                    partition_index: snapshot.partitionIndex,
                    volume_name: snapshot.volumeName,
                    sample_name: detail.object.name,
                    expected_payload_sha256: snapshot.payloadSha256,
                },
            }),
        );
    }
    private async submit(document: ObjectEditorDocument, start: () => Promise<JobState>): Promise<void> {
        try {
            document.status = document.writeKind === 'conversion' ? 'Converting Sample' : 'Saving Sample';
            const job = await start();
            document.jobId = job.jobId;
            await this.accept(document, await this.dependencies.transport.waitForJob(job.jobId, () => undefined));
        } catch (error) {
            if (
                document.jobId === null &&
                error instanceof AxklibApiError &&
                error.status >= 400 &&
                error.status < 500 &&
                error.status !== 408
            ) {
                this.rejectWrite(document, userFacingMessage(error), error.code);
            } else {
                document.phase = 'unconfirmed';
                document.status = `Write outcome unconfirmed${document.jobId === null ? '; do not repeat the write' : ''}: ${userFacingMessage(error)}`;
                this.reportWriteFailure(document, 'unconfirmed', error);
            }
        }
    }
    async recover(document: ObjectEditorDocument): Promise<void> {
        if (document.phase === 'refresh-failed') {
            await this.finish(document);
            return;
        }
        if (document.phase !== 'unconfirmed' || document.jobId === null) return;
        document.phase = 'saving';
        try {
            await this.accept(document, await this.dependencies.transport.waitForJob(document.jobId, () => undefined));
        } catch (error) {
            document.phase = 'unconfirmed';
            document.status = userFacingMessage(error);
        }
    }
    clear(): void {
        this.generation++;
        this.navigations.clear();
        this.preferenceScopes.clear();
        this.comparison.clear();
        this.duplication.close();
        this.conversionDocument = null;
        this.loading.clear();
        this.documents = [];
    }
    private async accept(document: ObjectEditorDocument, job: JobState): Promise<void> {
        if (job.status === 'failed' || job.status === 'cancelled') {
            this.rejectWrite(document, job.error ?? 'The operation did not complete', job.errorCode);
        } else if (job.status === 'completed') await this.finish(document);
        else {
            document.phase = 'unconfirmed';
            document.status = 'Save is still pending';
        }
    }
    private rejectWrite(document: ObjectEditorDocument, message: string, code?: string): void {
        this.reportWriteFailure(document, 'rejected', message, code);
        const action = document.writeKind === 'conversion' ? 'Conversion' : 'Save';
        document.status =
            code === 'entry_in_use'
                ? `${action} not started. Another image session or file operation is using this image. Close the other session or wait, then try again.`
                : `${action} failed: ${userFacingMessage(message)}`;
        document.jobId = null;
        document.conversionTarget = null;
        document.phase = 'editable';
    }
    private reportWriteFailure(document: ObjectEditorDocument, stage: string, error: unknown, code?: string): void {
        reportDiagnostic(
            'sample_editor_write_failed',
            {
                sessionId: document.sessionId,
                objectId: document.detail?.object.id,
                jobId: document.jobId,
                operation: document.writeKind,
                target: document.conversionTarget,
                stage,
                code: code ?? (error instanceof AxklibApiError ? error.code : undefined),
                requestId: error instanceof AxklibApiError ? error.requestId : undefined,
                message: userFacingMessage(error),
            },
            'warn',
        );
    }
    private async finish(document: ObjectEditorDocument): Promise<void> {
        document.phase = 'saving';
        const completed = document.writeKind === 'conversion' ? 'Sample converted' : 'Sample saved';
        document.status = `${completed}; refreshing workspace`;
        let stage = 'workspace refresh';
        try {
            await this.dependencies.refresh();
            stage = 'sample verification';
            const detail = await this.dependencies.transport.objectDetail(
                document.sessionId,
                document.detail!.object.id,
            );
            const adapter = objectEditorAdapter(detail);
            if (!adapter) throw new Error('Saved Sample is not available');
            if (document.conversionTarget && detail.editing?.sampleFormat.format !== document.conversionTarget)
                throw new Error('The refreshed Sample does not have the confirmed target format');
            document.detail = detail;
            document.draft.accept(adapter.values(detail.editing!));
            document.conflict = '';
            document.status = 'Sample saved';
            document.jobId = null;
            document.phase = 'editable';
            if (document.writeKind === 'conversion') {
                document.status = 'Sample format converted';
                if (this.conversionDocument === document) this.conversionDocument = null;
            }
            document.conversionTarget = null;
            this.dependencies.status(
                `${document.writeKind === 'conversion' ? 'Converted' : 'Saved'} Sample ${detail.object.name}`,
            );
        } catch (error) {
            document.phase = 'refresh-failed';
            document.status = `${completed}; ${stage} failed: ${userFacingMessage(error)}`;
            this.reportWriteFailure(document, stage, error);
        }
    }
}
