import { getContext, setContext } from 'svelte';
import type { ObjectEditorDocument, ObjectEditorWorkflow } from '../object-editor/workflow.svelte';
import { ProgramDraft } from '../devices/a-series/program/draft.svelte';
import { mappingLimits, mappingPatch, mappingZones } from '../devices/a-series/program/mappingModel';
import {
    validRange,
    type MappingCommand,
    type MappingHostAdapter,
    type MappingMessage,
    type MappingSnapshot,
} from './protocol';
import { userFacingMessage } from '../../lib/userFacingMessage';

const key = Symbol('program-mapping');
export function provideMappingEditor(controller: MappingController) {
    setContext(key, controller);
}
export function mappingEditor(): MappingController | undefined {
    return getContext(key);
}
const empty = (): MappingSnapshot => ({
    owner: '',
    context: '',
    version: 0,
    title: '',
    assignmentId: null,
    assignments: [],
    zones: [],
    limits: null,
    editable: false,
    canUndo: false,
    canRedo: false,
    canSave: false,
    canDiscard: false,
    recovery: null,
    status: 'Select a Program.',
});

export class MappingController {
    available = $state(false);
    opened = $state(false);
    retryVersion = $state(0);
    private loadError = '';
    state = $state.raw<MappingSnapshot>(empty());
    private document: ObjectEditorDocument | null = null;
    private fingerprint = '';
    private epoch = 0;
    private readonly identity = crypto.randomUUID();
    private version = 0;
    private busy = false;
    private adapter: MappingHostAdapter | undefined;
    private stop: (() => void) | undefined;
    constructor(private readonly workflow: Pick<ObjectEditorWorkflow, 'locked' | 'save' | 'discard' | 'recover'>) {}
    async connect(adapter: MappingHostAdapter): Promise<void> {
        this.adapter = adapter;
        this.stop = await adapter.listen((command) => {
            void this.receive(command).then((reply) => this.publish(reply));
        });
        this.available = true;
    }
    dispose(): void {
        this.refresh(null);
        this.stop?.();
        this.available = false;
        this.opened = false;
        this.adapter = undefined;
    }
    async open(): Promise<void> {
        if (!this.adapter) return;
        this.opened = true;
        await this.adapter.open();
    }
    loadFailed(message: string): void {
        this.loadError = message;
        this.state = { ...this.state, owner: this.identity, version: ++this.version, status: message };
        if (this.opened) void this.publish({ state: this.state });
    }
    beginSelection(): void {
        this.loadError = '';
        this.fingerprint = '';
        this.refresh(null);
    }
    private async publish(message: MappingMessage): Promise<void> {
        try {
            await this.adapter?.publish(message);
        } catch (error) {
            if (this.document) this.document.status = `Mapping Editor: ${userFacingMessage(error)}`;
        }
    }
    refresh(document: ObjectEditorDocument | null): void {
        if (document !== this.document) {
            this.document = document;
            this.epoch++;
            this.fingerprint = '';
        }
        const snapshot = document?.detail?.editing;
        if (!document || snapshot?.profile !== 'a-series/program' || !(document.draft instanceof ProgramDraft)) {
            if (this.fingerprint === 'empty') return;
            this.fingerprint = 'empty';
            this.state = {
                ...empty(),
                owner: this.identity,
                version: ++this.version,
                status: this.loadError || 'Select a Program.',
            };
        } else {
            const draft = document.draft;
            const fingerprint = JSON.stringify([
                document.detail?.image.revision,
                snapshot.payloadSha256,
                snapshot.editable,
                snapshot.reason,
                draft.values,
                draft.assignments,
                document.programAssignmentId,
                document.phase,
                document.validation,
                document.status,
                draft.canUndo,
                draft.canRedo,
                draft.dirty,
                this.workflow.locked,
            ]);
            if (fingerprint === this.fingerprint) return;
            this.fingerprint = fingerprint;
            this.loadError = '';
            const assignments = draft.assignments.filter((row) => row.name);
            const assignment = assignments.find((row) => row.id === document.programAssignmentId) ?? assignments[0];
            const limits = assignment ? mappingLimits(draft.values, assignment.id) : null;
            const editable =
                snapshot.editable && document.phase === 'editable' && !document.conflict && !this.workflow.locked;
            this.state = {
                owner: this.identity,
                context: `${this.identity}:${this.epoch}`,
                version: ++this.version,
                title: snapshot.programName,
                assignmentId: assignment?.id ?? null,
                assignments: assignments.map((row) => ({
                    value: row.id,
                    label: `${row.name} (${row.kind === 'SBAC' ? 'Bank' : 'Sample'})${assignments.some((other) => other.id !== row.id && other.kind === row.kind && other.name === row.name) ? ` · ${row.id + 1}` : ''}`,
                })),
                zones: mappingZones(snapshot, assignments, draft.values, assignment?.id ?? -1),
                limits: limits && validRange(limits) ? limits : null,
                editable: editable && assignment?.kind !== 'UNKNOWN' && !!limits && validRange(limits),
                canUndo: editable && draft.canUndo,
                canRedo: editable && draft.canRedo,
                canSave: editable && document.canSave,
                canDiscard:
                    document.phase === 'editable' && !this.workflow.locked && (draft.dirty || !!document.conflict),
                recovery:
                    document.phase === 'refresh-failed'
                        ? 'Refresh'
                        : document.phase === 'unconfirmed' && document.jobId !== null
                          ? 'Check status'
                          : null,
                status:
                    document.validation ||
                    document.status ||
                    snapshot.reason ||
                    (draft.dirty ? 'Unsaved changes' : 'Ready'),
            };
        }
        if (this.opened) void this.publish({ state: this.state });
    }
    async receive(command: MappingCommand): Promise<MappingMessage> {
        this.refresh(this.document);
        let error: string | undefined;
        try {
            if (command.action.kind === 'ready') {
                this.opened = true;
                if (this.loadError) {
                    this.loadError = '';
                    this.retryVersion++;
                }
            } else {
                const document = this.document;
                if (
                    this.busy ||
                    !document ||
                    command.context !== this.state.context ||
                    command.version !== this.state.version
                )
                    throw new Error('The Program or its draft changed. Review the current mapping and try again.');
                const action = command.action;
                if (action.kind === 'select') {
                    if (!this.state.assignments.some((row) => row.value === action.assignmentId))
                        throw new Error('Assignment is no longer available.');
                    document.programAssignmentId = action.assignmentId;
                } else if (action.kind === 'range') {
                    if (
                        !this.state.editable ||
                        action.assignmentId !== this.state.assignmentId ||
                        !validRange(action.range)
                    )
                        throw new Error('This mapping range cannot be edited.');
                    document.draft.patch(mappingPatch(action.assignmentId, action.range));
                } else if (action.kind === 'undo' && this.state.canUndo) document.draft.undo();
                else if (action.kind === 'redo' && this.state.canRedo) document.draft.redo();
                else if (
                    (action.kind === 'save' && this.state.canSave) ||
                    (action.kind === 'discard' && this.state.canDiscard) ||
                    (action.kind === 'recover' && this.state.recovery)
                ) {
                    this.busy = true;
                    try {
                        await this.workflow[action.kind](document);
                    } finally {
                        this.busy = false;
                    }
                } else throw new Error('This action is not available for the current Program.');
            }
        } catch (reason) {
            error = userFacingMessage(reason);
        }
        this.refresh(this.document);
        return { state: this.state, requestId: command.requestId, error };
    }
}
