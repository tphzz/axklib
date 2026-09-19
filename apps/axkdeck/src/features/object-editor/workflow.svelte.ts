import { EditorDraft } from './draft.svelte';
import { objectEditorAdapter } from './registry';
import { AxklibApiError } from '../../lib/httpErrors';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { ImageTransport, ObjectDetail, JobState } from '../../lib/transport';

export class ObjectEditorDocument {
    detail = $state.raw<ObjectDetail>();
    draft: EditorDraft;
    conflict = $state('');
    status = $state('');
    phase = $state<'editable' | 'saving' | 'unconfirmed' | 'refresh-failed'>('editable');
    tab = $state('trim-loop');
    page = $state('');
    inputErrors = $state<Record<string, string>>({});
    jobId: number | null = null;
    constructor(
        readonly sessionId: number,
        detail: ObjectDetail,
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
    transport: Pick<ImageTransport, 'objectDetail' | 'startObjectParameterEdit' | 'waitForJob'>;
    refresh: () => Promise<void>;
    stopPlayback: () => void;
    status: (message: string) => void;
    audition?: (document: ObjectEditorDocument, note: number) => Promise<void>;
};

export class ObjectEditorWorkflow {
    visible = $state(false);
    documents = $state.raw<ObjectEditorDocument[]>([]);
    private loading = new Map<string, Promise<ObjectEditorDocument | null>>();
    private generation = 0;
    constructor(private readonly dependencies: Dependencies) {}
    get dirtyCount(): number {
        return this.documents.filter((item) => item.draft.dirty).length;
    }
    get locked(): boolean {
        return this.documents.some((item) => item.phase !== 'editable');
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
                const document = new ObjectEditorDocument(sessionId, detail);
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
        try {
            document.status = 'Saving Sample';
            const edit = objectEditorAdapter(document.detail!)!.edit(
                document.detail!,
                document.draft.changes,
                document.draft.values,
            );
            const job = await this.dependencies.transport.startObjectParameterEdit(document.sessionId, edit);
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
                document.phase = 'editable';
                document.status = userFacingMessage(error);
            } else {
                document.phase = 'unconfirmed';
                document.status = `Save outcome unconfirmed${document.jobId === null ? '; do not repeat the write' : ''}: ${userFacingMessage(error)}`;
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
        this.loading.clear();
        this.documents = [];
    }
    private async accept(document: ObjectEditorDocument, job: JobState): Promise<void> {
        if (job.status === 'failed' || job.status === 'cancelled') {
            document.phase = 'editable';
            document.status = job.error ?? 'Sample was not saved';
            document.jobId = null;
        } else if (job.status === 'completed') await this.finish(document);
        else {
            document.phase = 'unconfirmed';
            document.status = 'Save is still pending';
        }
    }
    private async finish(document: ObjectEditorDocument): Promise<void> {
        document.phase = 'saving';
        document.status = 'Sample saved; refreshing workspace';
        try {
            await this.dependencies.refresh();
            const detail = await this.dependencies.transport.objectDetail(
                document.sessionId,
                document.detail!.object.id,
            );
            const adapter = objectEditorAdapter(detail);
            if (!adapter) throw new Error('Saved Sample is not available');
            document.detail = detail;
            document.draft.accept(adapter.values(detail.editing!));
            document.conflict = '';
            document.status = 'Sample saved';
            document.jobId = null;
            document.phase = 'editable';
            this.dependencies.status(`Saved Sample ${detail.object.name}`);
        } catch (error) {
            document.phase = 'refresh-failed';
            document.status = `Sample saved; refresh failed: ${userFacingMessage(error)}`;
        }
    }
}
