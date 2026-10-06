import type { ImageTransport, JobState, ObjectDetail } from '../../lib/transport';
import type { ObjectParameterEdit } from '../../lib/objectEditing';
import { AxklibApiError } from '../../lib/httpErrors';
import { CapacityWriteRejected } from '../../lib/httpCapacityGate';
import { reportDiagnostic } from '../../lib/diagnostics';
import { userFacingMessage } from '../../lib/userFacingMessage';
import { mappingMembers } from '../devices/a-series/bank/mappingMembers';
import type { ObjectEditorDocument } from './workflow.svelte';
import { objectEditorAdapter } from './registry';

interface Dependencies {
    transport: Pick<ImageTransport, 'objectDetail' | 'startObjectParameterEdit' | 'waitForJob'>;
    documents: () => ObjectEditorDocument[];
    locked: () => boolean;
    check: (document: ObjectEditorDocument) => Promise<boolean>;
    accept: (document: ObjectEditorDocument, detail: ObjectDetail) => void;
    refresh: () => Promise<void>;
    stop: () => void;
    status: (message: string) => void;
}
interface Transaction {
    bank: ObjectEditorDocument;
    members: ObjectEditorDocument[];
    jobId: number | null;
    recovering: boolean;
}

export class MemberEdits {
    private active: Transaction | null = null;
    constructor(private readonly dependencies: Dependencies) {}
    documents(bank: ObjectEditorDocument): ObjectEditorDocument[] {
        const ids = new Set(bank.detail ? mappingMembers(bank.detail).map((member) => member.id) : []);
        return this.dependencies
            .documents()
            .filter(
                (document) =>
                    document.sessionId === bank.sessionId &&
                    document.detail?.editing?.profile === 'a-series/sample' &&
                    ids.has(document.detail.object.id),
            );
    }
    changed(bank: ObjectEditorDocument): ObjectEditorDocument[] {
        return this.documents(bank).filter((document) => document.draft.dirty);
    }
    canSave(bank: ObjectEditorDocument): boolean {
        const changed = this.changed(bank);
        return !this.dependencies.locked() && changed.length > 0 && changed.every((document) => document.canSave);
    }
    contains(document: ObjectEditorDocument): boolean {
        return !!this.active && (this.active.bank === document || this.active.members.includes(document));
    }
    clear(): void {
        this.active = null;
    }
    private state(transaction: Transaction, phase: ObjectEditorDocument['phase'], status: string): void {
        if (this.active !== transaction) return;
        for (const document of [transaction.bank, ...transaction.members]) {
            document.phase = phase;
            document.status = status;
            document.jobId = transaction.jobId;
        }
    }
    private release(transaction: Transaction, status: string): void {
        transaction.jobId = null;
        this.state(transaction, 'editable', status);
        if (this.active === transaction) this.active = null;
    }
    async save(bank: ObjectEditorDocument): Promise<void> {
        if (!this.canSave(bank)) return;
        const transaction: Transaction = { bank, members: this.changed(bank), jobId: null, recovering: false };
        this.active = transaction;
        const membership = mappingMembers(bank.detail!)
            .map((member) => member.id)
            .sort()
            .join('\n');
        this.state(transaction, 'saving', 'Checking member changes');
        this.dependencies.stop();
        let revision: number;
        let operations: ObjectParameterEdit['operations'];
        try {
            if (!(await this.dependencies.check(bank)))
                throw new Error(bank.conflict || 'Bank changed during checking');
            if (
                membership !==
                mappingMembers(bank.detail!)
                    .map((member) => member.id)
                    .sort()
                    .join('\n')
            )
                throw new Error('Bank membership changed. Refresh the mapping before saving.');
            revision = bank.detail!.image.revision;
            for (const document of transaction.members) {
                if (!(await this.dependencies.check(document)))
                    throw new Error(document.conflict || 'Member changed during checking');
                if (document.detail!.image.revision !== revision)
                    throw new Error('The image revision changed during checking. Review the mapping and retry.');
                if (document.validation) throw new Error(`${document.detail!.object.name}: ${document.validation}`);
            }
            operations = transaction.members.map((document, index) => ({
                ...objectEditorAdapter(document.detail!, document.draft)!.edit(
                    document.detail!,
                    document.draft.changes,
                    document.draft.storedValues,
                ).operation,
                id: `member-edit-${index + 1}`,
            }));
        } catch (error) {
            this.release(transaction, userFacingMessage(error));
            return;
        }
        if (this.active !== transaction) return;
        this.state(transaction, 'saving', `Saving ${transaction.members.length} members`);
        try {
            const job = await this.dependencies.transport.startObjectParameterEdit(bank.sessionId, {
                expectedRevision: revision,
                operations,
            });
            transaction.jobId = job.jobId;
            this.state(transaction, 'saving', 'Saving members');
            await this.accept(transaction, await this.dependencies.transport.waitForJob(job.jobId, () => {}));
        } catch (error) {
            if (
                transaction.jobId === null &&
                (error instanceof CapacityWriteRejected ||
                    (error instanceof AxklibApiError &&
                        error.status >= 400 &&
                        error.status < 500 &&
                        error.status !== 408))
            )
                this.release(transaction, `Save failed: ${userFacingMessage(error)}`);
            else
                this.state(
                    transaction,
                    'unconfirmed',
                    `Write outcome unconfirmed${transaction.jobId === null ? '; do not repeat the write' : ''}: ${userFacingMessage(error)}`,
                );
            this.report(transaction, 'submission', error);
        }
    }
    async recover(document: ObjectEditorDocument): Promise<void> {
        const transaction = this.active;
        if (!transaction || !this.contains(document) || transaction.recovering) return;
        transaction.recovering = true;
        try {
            if (document.phase === 'refresh-failed') await this.finish(transaction);
            else if (document.phase === 'unconfirmed' && transaction.jobId !== null) {
                this.state(transaction, 'saving', 'Checking member save');
                try {
                    await this.accept(
                        transaction,
                        await this.dependencies.transport.waitForJob(transaction.jobId, () => {}),
                    );
                } catch (error) {
                    this.state(transaction, 'unconfirmed', userFacingMessage(error));
                }
            }
        } finally {
            transaction.recovering = false;
        }
    }
    private async accept(transaction: Transaction, job: JobState): Promise<void> {
        if (this.active !== transaction) return;
        if (job.status === 'completed') await this.finish(transaction);
        else if (job.status === 'failed' || job.status === 'cancelled')
            this.release(transaction, `Save failed: ${job.error ?? 'The operation did not complete'}`);
        else this.state(transaction, 'unconfirmed', 'Member save is still pending');
    }
    private async finish(transaction: Transaction): Promise<void> {
        this.state(transaction, 'saving', 'Members saved; refreshing workspace');
        try {
            await this.dependencies.refresh();
            const documents = [transaction.bank, ...transaction.members];
            const details: ObjectDetail[] = [];
            for (const document of documents) {
                const detail = await this.dependencies.transport.objectDetail(
                    document.sessionId,
                    document.detail!.object.id,
                );
                if (
                    !objectEditorAdapter(detail) ||
                    detail.object.key !== document.detail!.object.key ||
                    detail.object.name !== document.detail!.object.name ||
                    detail.object.type !== document.detail!.object.type ||
                    detail.editing?.volumeName !== document.detail!.editing?.volumeName ||
                    (details[0] && detail.image.revision !== details[0].image.revision)
                )
                    throw new Error('The refreshed member identity or image revision changed');
                details.push(detail);
            }
            if (details[0]!.editing?.payloadSha256 !== transaction.bank.detail!.editing?.payloadSha256)
                throw new Error('The bank changed outside the editor');
            if (this.active !== transaction) return;
            // Publish only after every detail was retrieved and validated. Bank edits are not part of this write.
            transaction.bank.detail = details[0]!;
            transaction.members.forEach((document, index) => this.dependencies.accept(document, details[index + 1]!));
            this.release(transaction, `${transaction.members.length} members saved`);
            this.dependencies.status(
                `Saved ${transaction.members.length} members of ${transaction.bank.detail.object.name}`,
            );
        } catch (error) {
            this.state(transaction, 'refresh-failed', `Members saved; refresh failed: ${userFacingMessage(error)}`);
            this.report(transaction, 'refresh', error);
        }
    }
    async discard(bank: ObjectEditorDocument): Promise<void> {
        if (this.dependencies.locked()) return;
        const transaction: Transaction = { bank, members: this.changed(bank), jobId: null, recovering: false };
        if (!transaction.members.length) return;
        this.active = transaction;
        this.state(transaction, 'saving', 'Discarding member changes');
        try {
            const details: ObjectDetail[] = [];
            for (const document of transaction.members) {
                const detail = await this.dependencies.transport.objectDetail(
                    document.sessionId,
                    document.detail!.object.id,
                );
                if (!objectEditorAdapter(detail) || detail.object.id !== document.detail!.object.id)
                    throw new Error('A member is no longer available');
                details.push(detail);
            }
            if (this.active !== transaction) return;
            transaction.members.forEach((document, index) => this.dependencies.accept(document, details[index]!));
            this.release(transaction, 'Member changes discarded');
        } catch (error) {
            this.release(transaction, userFacingMessage(error));
        }
    }
    private report(transaction: Transaction, stage: string, error: unknown): void {
        reportDiagnostic(
            'member_editor_write_failed',
            {
                sessionId: transaction.bank.sessionId,
                objectId: transaction.bank.detail?.object.id,
                jobId: transaction.jobId,
                memberCount: transaction.members.length,
                stage,
                message: userFacingMessage(error),
            },
            'warn',
        );
    }
}
