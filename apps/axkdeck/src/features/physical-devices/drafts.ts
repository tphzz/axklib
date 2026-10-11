import type { DeviceLocation } from '../../lib/storageLocations';
import type { EditorValues } from '../object-editor/draft.svelte';
import type { ObjectEditorDocument } from '../object-editor/workflow.svelte';
import { ProgramDraft, type DraftAssignment } from '../devices/a-series/program/draft.svelte';
import { BankDraft } from '../devices/a-series/bank/draft.svelte';

export interface RetainedDeviceDraft {
    id: string;
    device: string;
    target: string;
    name: string;
    objectKey: string;
    objectType: string;
    profile: string;
    partition: number;
    volume: string;
    payloadSha256: string;
    revision: number;
    values: EditorValues;
    membershipChanged: boolean;
    assignments?: DraftAssignment[];
}
const key = 'axkdeck.device-drafts';
const maximumBytes = 1024 * 1024;
function identity(location: DeviceLocation, document: ObjectEditorDocument) {
    const detail = document.detail;
    const base = detail?.editing;
    if (!base || !detail) return null;
    return {
        device: location.displayName,
        target: location.reference.targetId,
        name: detail.object.name,
        objectKey: detail.object.key,
        objectType: detail.object.type,
        profile: base.profile,
        partition: base.partitionIndex,
        volume: base.volumeName,
        payloadSha256: base.payloadSha256,
        revision: detail.image.revision,
    };
}
function recordId(
    base: Pick<
        RetainedDeviceDraft,
        'device' | 'target' | 'partition' | 'volume' | 'objectKey' | 'objectType' | 'profile' | 'payloadSha256'
    >,
): string {
    return JSON.stringify([
        base.device,
        base.target,
        base.partition,
        base.volume,
        base.objectKey,
        base.objectType,
        base.profile,
        base.payloadSha256,
    ]);
}
function valid(value: unknown): value is RetainedDeviceDraft {
    if (!value || typeof value !== 'object') return false;
    const item = value as RetainedDeviceDraft;
    return (
        ['id', 'device', 'target', 'name', 'objectKey', 'objectType', 'profile', 'volume', 'payloadSha256'].every(
            (field) => typeof item[field as keyof RetainedDeviceDraft] === 'string',
        ) &&
        Number.isSafeInteger(item.partition) &&
        item.partition >= 0 &&
        Number.isSafeInteger(item.revision) &&
        typeof item.membershipChanged === 'boolean' &&
        !!item.values &&
        typeof item.values === 'object' &&
        !Array.isArray(item.values) &&
        Object.values(item.values).every(
            (leaf) => typeof leaf === 'boolean' || (typeof leaf === 'number' && Number.isFinite(leaf)),
        ) &&
        (item.assignments === undefined ||
            (Array.isArray(item.assignments) &&
                item.assignments.length <= 1024 &&
                item.assignments.every(
                    (row) =>
                        !!row &&
                        typeof row === 'object' &&
                        Number.isSafeInteger(row.id) &&
                        row.id >= 0 &&
                        (row.retainOrdinal === undefined ||
                            (Number.isSafeInteger(row.retainOrdinal) && row.retainOrdinal >= 0)) &&
                        typeof row.kind === 'string' &&
                        typeof row.name === 'string' &&
                        (row.targetObjectId === null || typeof row.targetObjectId === 'string'),
                ))) &&
        recordId(item) === item.id
    );
}

export class DeviceDraftStore {
    private records: RetainedDeviceDraft[];
    private observed = new WeakMap<ObjectEditorDocument, string>();
    private persistedText: string;
    constructor(private readonly storage: Storage | null) {
        const text = storage?.getItem(key) ?? '[]';
        if (text.length > maximumBytes)
            throw new Error(
                'Retained device drafts exceed the storage limit. Retain local application data for review.',
            );
        const parsed: unknown = JSON.parse(text);
        if (!Array.isArray(parsed) || parsed.length > 128 || !parsed.every(valid))
            throw new Error('Retained device drafts are invalid. Retain local application data for review.');
        this.records = parsed;
        this.persistedText = JSON.stringify(parsed);
    }
    list(): RetainedDeviceDraft[] {
        return structuredClone(this.records);
    }
    sync(location: DeviceLocation, documents: readonly ObjectEditorDocument[]): void {
        const next = new Map(this.records.map((record) => [record.id, record]));
        for (const document of documents) {
            const base = identity(location, document);
            if (!base) continue;
            const previous = this.observed.get(document);
            if (document.draft.dirty) {
                const id = recordId(base);
                if (previous && previous !== id) next.delete(previous);
                const program = document.draft instanceof ProgramDraft ? document.draft : null;
                next.set(id, {
                    ...base,
                    id,
                    values: { ...document.draft.storedValues },
                    membershipChanged: program?.membershipChanged ?? false,
                    ...(program ? { assignments: program.assignments.map((row) => ({ ...row })) } : {}),
                });
                this.observed.set(document, id);
            } else if (previous && document.phase === 'editable') {
                next.delete(previous);
                this.observed.delete(document);
            }
        }
        this.save([...next.values()]);
    }
    matching(
        record: RetainedDeviceDraft,
        location: DeviceLocation | null,
        documents: readonly ObjectEditorDocument[],
    ): ObjectEditorDocument | undefined {
        if (!location) return undefined;
        return documents.find((document) => {
            const base = identity(location, document);
            return !!base && recordId(base) === record.id && base.name === record.name;
        });
    }
    restore(record: RetainedDeviceDraft, location: DeviceLocation, documents: readonly ObjectEditorDocument[]): void {
        if (!valid(record)) throw new Error('Retained draft identity is invalid.');
        const document = this.matching(record, location, documents);
        if (!document || document.phase !== 'editable' || document.draft.dirty)
            throw new Error('Restore requires an unchanged base payload and a clean, editable document.');
        if (record.membershipChanged)
            throw new Error(
                'Assignment membership changes require manual review. The complete draft is retained for JSON export.',
            );
        if (document.draft instanceof BankDraft) document.draft.restore(record.values);
        else document.draft.patch(record.values);
    }
    discard(id: string): void {
        this.save(this.records.filter((record) => record.id !== id));
    }
    private save(records: RetainedDeviceDraft[]): void {
        const text = JSON.stringify(records);
        // Keep the latest edits exportable even when persistent storage refuses them.
        this.records = records;
        if (records.length > 128 || text.length > maximumBytes)
            throw new Error('Device draft retention capacity is exhausted. Export or discard retained drafts.');
        if (!this.storage) throw new Error('Persistent device draft storage is unavailable.');
        if (text !== this.persistedText) this.storage.setItem(key, text);
        this.persistedText = text;
    }
}
