import { EditorDraft, type EditorDraftState } from './draft.svelte';
import { ProgramDraft } from '../devices/a-series/program/draft.svelte';
import type { ProgramEditorCatalog } from '../../lib/objectEditing';
import { BankDraft } from '../devices/a-series/bank/draft.svelte';
import { EditorNavigation } from './navigation.svelte';
import { EditorComparison } from './comparison.svelte';
import { SampleDuplication } from './duplication.svelte';
import { MemberEdits } from './memberEdits.svelte';
import { objectEditorAdapter } from './registry';
import { AxklibApiError } from '../../lib/httpErrors';
import { CapacityWriteRejected } from '../../lib/httpCapacityGate';
import { userFacingMessage } from '../../lib/userFacingMessage';
import { reportDiagnostic } from '../../lib/diagnostics';
import type { ImageTransport, ObjectDetail, JobState } from '../../lib/transport';
import type { ObjectStorageFormat, ObjectFormatConversionRequest } from '../../lib/objectEditing';
import { conversionFormat, conversionRequest, sameConversionIdentity } from './formatConversion';

export class ObjectEditorDocument {
    detail = $state.raw<ObjectDetail>();
    previewMemberId = $state<string | null | undefined>(undefined);
    previewDetail = $state.raw<ObjectDetail | null>(null);
    previewStatus = $state('');
    programAssignmentId = $state(0);
    draft: EditorDraftState;
    conflict = $state('');
    status = $state('');
    phase = $state<'editable' | 'saving' | 'unconfirmed' | 'refresh-failed'>('editable');
    inputErrors = $state<Record<string, string>>({});
    jobId: number | null = null;
    writeKind: 'save' | 'conversion' = 'save';
    conversionTarget: ObjectStorageFormat | null = null;
    constructor(
        readonly sessionId: number,
        detail: ObjectDetail,
        readonly preferencesScope: object = {},
        readonly programCatalog?: ProgramEditorCatalog,
    ) {
        this.detail = detail;
        const values = objectEditorAdapter(detail)?.values(detail.editing!) ?? {};
        this.draft =
            detail.editing?.profile === 'a-series/program'
                ? new ProgramDraft(values, detail.editing.assignments)
                : detail.editing?.bankOverrides
                  ? new BankDraft(values, detail.editing.bankOverrides.units)
                  : new EditorDraft(values);
    }
    get programFormat() {
        const editing = this.detail?.editing;
        return editing?.profile === 'a-series/program'
            ? this.programCatalog?.formats.find((item) => item.model === editing.model)
            : undefined;
    }
    private validated = $derived.by(() => {
        this.draft.revision;
        return (
            this.conflict ||
            Object.entries(this.inputErrors).find(([key, error]) => {
                const row = /^assignments\.(\d+)\./.exec(key);
                return (
                    error &&
                    (!(this.draft instanceof ProgramDraft) ||
                        !row ||
                        this.draft.assignments.some((item) => item.id === Number(row[1])))
                );
            })?.[1] ||
            objectEditorAdapter(this.detail!, this.draft)?.validate(
                this.draft.storedValues,
                this.draft.changes,
                this.detail!.editing!,
                this.programFormat,
            ) ||
            ''
        );
    });
    get validation(): string {
        return this.validated;
    }
    get canSave(): boolean {
        return !!objectEditorAdapter(this.detail!) && this.phase === 'editable' && this.draft.dirty && !this.validation;
    }
    get noun(): string {
        return this.detail?.object.type === 'PROG'
            ? 'Program'
            : this.detail?.object.type === 'SBAC'
              ? 'Sample Bank'
              : 'Sample';
    }
}

type Dependencies = {
    transport: Pick<ImageTransport, 'objectDetail' | 'startObjectParameterEdit' | 'waitForJob'> &
        Partial<
            Pick<ImageTransport, 'startSampleDuplication' | 'startObjectFormatConversion' | 'programEditorCatalog'>
        >;
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
    private conversionOpenRequest = 0;
    private navigations = new Map<string, EditorNavigation>();
    private preferenceScopes = new Map<number, object>();
    private checks = new WeakMap<ObjectEditorDocument, number>();
    readonly comparison: EditorComparison;
    readonly duplication: SampleDuplication;
    readonly members: MemberEdits;
    constructor(private readonly dependencies: Dependencies) {
        this.members = new MemberEdits({
            ...dependencies,
            documents: () => this.documents,
            locked: () => this.locked,
            check: (document) => this.check(document),
            accept: (document, detail) => this.acceptReload(document, detail, true),
            stop: dependencies.stopPlayback,
        });
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
            if (profile === 'a-series/program') navigation.tab = 'sample-select';
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
            .then(async (detail) => {
                if (generation !== this.generation) return null;
                if (!objectEditorAdapter(detail) && !detail.formatConversion) return null;
                const scope = this.preferenceScopes.get(sessionId) ?? {};
                this.preferenceScopes.set(sessionId, scope);
                const catalog =
                    detail.editing?.profile === 'a-series/program'
                        ? await this.dependencies.transport.programEditorCatalog?.()
                        : undefined;
                if (generation !== this.generation) return null;
                const document = new ObjectEditorDocument(sessionId, detail, scope, catalog);
                if (detail.editing?.profile === 'a-series/program' && !document.programFormat)
                    throw new Error('Program parameter catalog is unavailable for this format.');
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
        const old = document.detail!,
            phase = document.phase,
            generation = this.generation;
        const request = (this.checks.get(document) ?? 0) + 1;
        this.checks.set(document, request);
        const current = await this.dependencies.transport.objectDetail(document.sessionId, document.detail!.object.id);
        if (
            generation !== this.generation ||
            request !== this.checks.get(document) ||
            document.phase !== phase ||
            document.detail !== old
        )
            return false;
        if (
            (!objectEditorAdapter(current) && !current.formatConversion) ||
            current.object.key !== old.object.key ||
            current.object.type !== old.object.type ||
            current.editing?.payloadSha256 !== old.editing?.payloadSha256 ||
            current.formatConversion?.payloadSha256 !== old.formatConversion?.payloadSha256 ||
            !sameConversionIdentity(current.formatConversion, old.formatConversion) ||
            current.editing?.volumeName !== old.editing?.volumeName ||
            current.formatConversion?.volumeName !== old.formatConversion?.volumeName ||
            current.object.name !== old.object.name
        ) {
            document.conflict = `This ${document.noun} changed outside the editor. ${old.object.type !== 'SBNK' ? 'Close and reopen conversion to reload it.' : 'Discard the draft to reload it.'}`;
            return false;
        }
        document.detail = current;
        return true;
    }
    async revalidate(sessionId: number): Promise<void> {
        for (const document of this.documents.filter(
            (item) => item.sessionId === sessionId && item.phase === 'editable',
        )) {
            if (document.phase !== 'editable') continue;
            const detail = document.detail;
            try {
                await this.check(document);
            } catch (error) {
                if (document.phase === 'editable' && document.detail === detail && this.documents.includes(document))
                    document.conflict = userFacingMessage(error);
            }
        }
    }
    async discard(document: ObjectEditorDocument): Promise<void> {
        if (document.phase !== 'editable') return;
        const generation = this.generation;
        const detail = await this.dependencies.transport.objectDetail(document.sessionId, document.detail!.object.id);
        if (generation !== this.generation || document.phase !== 'editable') return;
        this.acceptReload(document, detail);
    }
    private acceptReload(document: ObjectEditorDocument, detail: ObjectDetail, committed = false): void {
        const adapter = objectEditorAdapter(detail);
        if (!adapter && !detail.formatConversion) throw new Error('This object is no longer available');
        document.detail = detail;
        if (
            document.draft instanceof BankDraft &&
            detail.editing?.profile !== 'a-series/program' &&
            detail.editing?.bankOverrides
        )
            document.draft.units = detail.editing.bankOverrides.units;
        const values = adapter?.values(detail.editing!) ?? {};
        if (document.draft instanceof ProgramDraft && detail.editing?.profile === 'a-series/program') {
            const rows = document.draft.assignments;
            const selected = rows.find((row) => row.id === document.programAssignmentId);
            const index = committed ? rows.indexOf(selected!) : (selected?.retainOrdinal ?? 0);
            document.draft.acceptProgram(values, detail.editing.assignments);
            document.programAssignmentId = detail.editing.assignments[Math.max(0, index)]?.ordinal ?? 0;
        } else document.draft.accept(values);
        document.inputErrors = {};
        document.conflict = '';
        document.status = '';
        this.dependencies.stopPlayback();
    }
    confirmStructuralChange: () => Promise<boolean> = async () => !this.dirtyCount && !this.locked;
    async discardAll(): Promise<boolean> {
        if (this.locked) return false;
        for (const document of this.documents.filter((item) => item.draft.dirty)) await this.discard(document);
        return !this.locked && !this.dirtyCount;
    }
    async saveAll(): Promise<boolean> {
        for (const document of this.documents.filter((item) => item.draft.dirty)) {
            await this.save(document);
            if (document.phase !== 'editable' || document.draft.dirty) return false;
        }
        return !this.locked && !this.dirtyCount;
    }
    async save(document: ObjectEditorDocument): Promise<void> {
        if (!document.canSave || this.locked) return;
        document.phase = 'saving';
        document.status = `Checking ${document.noun}`;
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
        const edit = objectEditorAdapter(document.detail!, document.draft)!.edit(
            document.detail!,
            document.draft.changes,
            document.draft.storedValues,
            document.programFormat,
        );
        await this.submit(document, () =>
            this.dependencies.transport.startObjectParameterEdit(document.sessionId, {
                expectedRevision: edit.expectedRevision,
                operations: [edit.operation],
            }),
        );
    }
    conversionReason(document: ObjectEditorDocument): string {
        return document.draft.dirty
            ? `Save or discard this ${document.noun} draft before converting.`
            : document.conflict ||
                  (!document.detail?.formatConversion?.canConvertFormat
                      ? document.detail?.formatConversion?.reason || 'This object cannot be converted in this image.'
                      : !this.dependencies.transport.startObjectFormatConversion
                        ? 'Format conversion is unavailable.'
                        : '');
    }
    async openConversion(sessionId: number, id: string): Promise<void> {
        if (this.locked) return;
        if (
            this.documents.some(
                (item) => item.sessionId === sessionId && item.detail?.object.id === id && item.draft.dirty,
            ) &&
            !(await this.confirmStructuralChange())
        )
            return;
        const request = ++this.conversionOpenRequest;
        const generation = this.generation;
        const current = () => request === this.conversionOpenRequest && generation === this.generation && !this.locked;
        try {
            const document = await this.load(sessionId, id);
            if (!document?.detail?.formatConversion || !current()) return;
            if (document.detail.object.type !== 'SBNK' && !document.draft.dirty) {
                const detail = await this.dependencies.transport.objectDetail(sessionId, id);
                if (!current()) return;
                this.acceptReload(document, detail);
            }
            if (!document.detail?.formatConversion) return;
            this.conversionDocument = document;
            document.status = '';
        } catch (error) {
            if (current()) this.dependencies.status(userFacingMessage(error));
        }
    }
    closeConversion(): void {
        if (!this.conversionDocument || this.conversionDocument.phase === 'editable') {
            this.conversionOpenRequest++;
            this.conversionDocument = null;
        }
    }
    async convert(document: ObjectEditorDocument, target: Exclude<ObjectStorageFormat, 'UNKNOWN'>): Promise<void> {
        if (this.locked || this.conversionReason(document)) return;
        document.phase = 'saving';
        document.status = `Checking ${document.noun} format`;
        this.dependencies.stopPlayback();
        let request: ObjectFormatConversionRequest | null = null;
        try {
            if (!(await this.check(document))) {
                document.phase = 'editable';
                return;
            }
            const snapshot = document.detail!.formatConversion!;
            const preview = snapshot.formatConversions.find((item) => item.targetFormat === target);
            request = conversionRequest(document.detail!, target);
            if (this.conversionReason(document) || !preview?.allowed || !request) {
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
        await this.submit(document, () =>
            this.dependencies.transport.startObjectFormatConversion!(document.sessionId, request!),
        );
    }
    private async submit(document: ObjectEditorDocument, start: () => Promise<JobState>): Promise<void> {
        try {
            document.status =
                document.writeKind === 'conversion' ? `Converting ${document.noun}` : `Saving ${document.noun}`;
            const job = await start();
            document.jobId = job.jobId;
            await this.accept(document, await this.dependencies.transport.waitForJob(job.jobId, () => undefined));
        } catch (error) {
            if (
                document.jobId === null &&
                (error instanceof CapacityWriteRejected ||
                    (error instanceof AxklibApiError &&
                        error.status >= 400 &&
                        error.status < 500 &&
                        error.status !== 408))
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
        if (this.members.contains(document)) return this.members.recover(document);
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
        this.members.clear();
        this.generation++;
        this.conversionOpenRequest++;
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
        const completed = document.writeKind === 'conversion' ? `${document.noun} converted` : `${document.noun} saved`;
        document.status = `${completed}; refreshing workspace`;
        let stage = 'workspace refresh';
        try {
            await this.dependencies.refresh();
            stage = 'object verification';
            const detail = await this.dependencies.transport.objectDetail(
                document.sessionId,
                document.detail!.object.id,
            );
            const adapter = objectEditorAdapter(detail);
            if (!adapter && !detail.formatConversion) throw new Error('Saved object is not available');
            if (
                detail.object.key !== document.detail!.object.key ||
                detail.object.type !== document.detail!.object.type ||
                detail.object.name !== document.detail!.object.name ||
                !sameConversionIdentity(detail.formatConversion, document.detail!.formatConversion)
            )
                throw new Error('The refreshed object does not match the saved identity');
            if (document.conversionTarget && conversionFormat(detail.formatConversion) !== document.conversionTarget)
                throw new Error('The refreshed object does not have the confirmed target format');
            this.acceptReload(document, detail, true);
            document.status = `${document.noun} saved`;
            document.jobId = null;
            document.phase = 'editable';
            if (document.writeKind === 'conversion') {
                document.status = `${document.noun} format converted`;
                if (this.conversionDocument === document) this.conversionDocument = null;
            }
            document.conversionTarget = null;
            this.dependencies.status(
                `${document.writeKind === 'conversion' ? 'Converted' : 'Saved'} ${document.noun} ${detail.object.name}`,
            );
        } catch (error) {
            document.phase = 'refresh-failed';
            document.status = `${completed}; ${stage} failed: ${userFacingMessage(error)}`;
            this.reportWriteFailure(document, stage, error);
        }
    }
}
