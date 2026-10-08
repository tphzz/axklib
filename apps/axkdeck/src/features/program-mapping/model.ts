import type { ObjectEditorDocument, ObjectEditorWorkflow } from '../object-editor/workflow.svelte';
import type { KeyboardRange, RangeBoundary } from '../object-editor/keyboardMapping';
import type { MappingSnapshot } from './protocol';

export type MappingWorkflow = Pick<ObjectEditorWorkflow, 'locked' | 'save' | 'discard' | 'recover'> &
    Partial<Pick<ObjectEditorWorkflow, 'members' | 'find' | 'documents'>>;
export interface MappingModel {
    data: Omit<MappingSnapshot, 'owner' | 'context' | 'version' | 'editRevision' | 'imageRevision' | 'role'>;
    fingerprint: string;
    select(id: number): void;
    patch(range: KeyboardRange, boundaries: RangeBoundary[], selectionId?: number): void;
    root(note: number): void;
    inherit(boundary: RangeBoundary): void;
    undo(): void;
    redo(): void;
    save(): Promise<void>;
    discard(): Promise<void>;
    recover(): Promise<void>;
}
const detailIds = new WeakMap<object, number>();
let nextDetailId = 0;
function detailId(detail: object | undefined) {
    if (!detail) return 0;
    if (!detailIds.has(detail)) detailIds.set(detail, ++nextDetailId);
    return detailIds.get(detail);
}
export function documentMappingState(document: ObjectEditorDocument, workflow: MappingWorkflow) {
    const draft = document.draft;
    const editable =
        !!document.detail?.editing?.editable && document.phase === 'editable' && !document.conflict && !workflow.locked;
    return {
        editable,
        canUndo: editable && draft.canUndo,
        canRedo: editable && draft.canRedo,
        canSave: editable && document.canSave,
        canDiscard: document.phase === 'editable' && !workflow.locked && (draft.dirty || !!document.conflict),
        recovery:
            document.phase === 'refresh-failed'
                ? ('Refresh' as const)
                : document.phase === 'unconfirmed' && document.jobId !== null
                  ? ('Check status' as const)
                  : null,
        status:
            document.validation ||
            document.status ||
            document.detail?.editing?.reason ||
            (draft.dirty ? 'Unsaved changes' : 'Ready'),
    };
}
export function mappingFingerprint(documents: ObjectEditorDocument[]): string {
    return JSON.stringify(
        documents.map((document) => [
            detailId(document.detail),
            document.detail?.image.revision,
            document.detail?.editing?.payloadSha256,
            document.detail?.editing?.editable,
            document.detail?.editing?.reason,
            document.detail?.editing?.profile === 'a-series/program'
                ? document.detail.editing.model
                : document.detail?.editing?.blockedParameters,
            document.draft.revision,
            document.draft.canUndo,
            document.draft.canRedo,
            document.phase,
            document.conflict,
            document.inputErrors,
            document.status,
            document.jobId,
        ]),
    );
}

export function selectMapping(data: MappingModel['data'], id: number): MappingModel['data'] {
    const target = data.targets.find((row) => row.id === id);
    if (!target) return data;
    const { id: _id, ...state } = target;
    return {
        ...data,
        ...state,
        selectionId: id,
        zones: data.zones.map((zone) => ({ ...zone, selected: zone.selectionId === id })),
    };
}
