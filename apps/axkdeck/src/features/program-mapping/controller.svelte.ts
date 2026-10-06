import { getContext, setContext } from 'svelte';
import type { ObjectEditorDocument } from '../object-editor/workflow.svelte';
import { aSeriesMapping } from '../devices/a-series/mapping';
import { rangeBoundaries } from '../object-editor/keyboardMapping';
import { mappingFingerprint, selectMapping, type MappingWorkflow, type MappingModel } from './model';
import {
    validRange,
    type MappingRole,
    type MappingCommand,
    type MappingHostAdapter,
    type MappingMessage,
    type MappingSnapshot,
} from './protocol';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { MappingAudio } from './audition';
import { MappingHeldNote } from './heldNote';

const key = Symbol('mapping-editors');
export function provideMappingEditor(controller: MappingController) {
    setContext(key, {
        ...getContext<Partial<Record<MappingRole, MappingController>>>(key),
        [controller.role]: controller,
    });
}
export function mappingEditor(role: MappingRole = 'program'): MappingController | undefined {
    return getContext<Partial<Record<MappingRole, MappingController>>>(key)?.[role];
}
const empty = (role: MappingRole): MappingSnapshot => ({
    role,
    owner: '',
    context: '',
    version: 0,
    editRevision: 0,
    imageRevision: 0,
    title: '',
    selectionId: null,
    selectionLabel: '',
    selections: [],
    targets: [],
    editableAxes: { keys: false, velocity: false },
    rootEditable: false,
    rangeLabel: 'Range',
    overrides: [],
    zones: [],
    limits: null,
    editable: false,
    canUndo: false,
    canRedo: false,
    canSave: false,
    canDiscard: false,
    recovery: null,
    status: role === 'program' ? 'Select a Program.' : role === 'sample' ? 'Select a Sample.' : 'Select a Sample Bank.',
});

export class MappingController {
    available = $state(false);
    opened = $state(false);
    retryVersion = $state(0);
    private loadError = '';
    state: MappingSnapshot = $state.raw(empty('program'));
    private document: ObjectEditorDocument | null = null;
    private fingerprint = '';
    private epoch = 0;
    private readonly identity = crypto.randomUUID();
    private version = 0;
    private editRevision = 0;
    private model: MappingModel | null = null;
    private contentKey = '';
    private busy = false;
    private adapter: MappingHostAdapter | undefined;
    private stop: (() => void) | undefined;
    private readonly note?: MappingHeldNote;
    constructor(
        private readonly workflow: MappingWorkflow,
        readonly role: MappingRole = 'program',
        audio?: MappingAudio,
    ) {
        this.state = empty(role);
        if (audio) this.note = new MappingHeldNote(audio, role);
    }
    async connect(adapter: MappingHostAdapter): Promise<void> {
        this.adapter = adapter;
        this.stop = await adapter.listen((command) => {
            if (command.role === this.role) void this.receive(command).then((reply) => this.publish(reply));
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
    refresh(document: ObjectEditorDocument | null, publish = true): MappingModel | null {
        if (document !== this.document) {
            this.note?.reset();
            this.document = document;
            this.epoch++;
            this.fingerprint = '';
            this.contentKey = '';
            this.model = null;
        }
        const documents = document
            ? [
                  document,
                  ...(this.role === 'program'
                      ? (this.workflow.documents?.filter(
                            (item) => item !== document && item.sessionId === document.sessionId,
                        ) ?? [])
                      : (this.workflow.members?.documents(document) ?? [])),
              ]
            : [];
        const contentKey =
            mappingFingerprint(documents) +
            `:${this.workflow.locked}:${document ? (this.workflow.members?.contains(document) ?? false) : false}`;
        const contentChanged = contentKey !== this.contentKey;
        if (contentChanged) {
            this.model = document ? aSeriesMapping(this.role, document, this.workflow) : null;
            this.contentKey = contentKey;
            this.editRevision++;
        }
        const model = this.model;
        const selectionId = document
            ? this.role === 'program'
                ? document.programAssignmentId
                : this.role === 'sample'
                  ? 0
                  : model?.data.zones.find((zone) => zone.id === document.previewMemberId)?.selectionId
            : undefined;
        if (model && selectionId !== undefined) model.data = selectMapping(model.data, selectionId);
        const fingerprint = `${contentKey}:${selectionId}:${this.loadError}`;
        if (fingerprint === this.fingerprint) return model;
        this.fingerprint = fingerprint;
        this.state = {
            ...empty(this.role),
            ...model?.data,
            owner: this.identity,
            context: model ? `${this.identity}:${this.epoch}` : '',
            version: ++this.version,
            editRevision: this.editRevision,
            imageRevision: document?.detail?.image.revision ?? 0,
            ...(this.loadError ? { status: this.loadError } : {}),
        };
        if (this.opened && publish) void this.publish({ state: this.state });
        return model;
    }
    async receive(command: MappingCommand): Promise<MappingMessage> {
        if (command.action.kind === 'note' || command.action.kind === 'release' || command.action.kind === 'lease') {
            try {
                if (command.role !== this.role) throw new Error('Wrong Mapping Editor.');
                const allowed =
                    command.context === this.state.context &&
                    (command.action.kind !== 'note' ||
                        command.action.imageRevision === this.document?.detail?.image.revision) &&
                    !this.busy &&
                    !this.workflow.locked &&
                    !!this.document &&
                    this.document.phase === 'editable' &&
                    !this.document.conflict &&
                    !(this.document.draft.dirty
                        ? this.document.validation
                        : Object.values(this.document.inputErrors).find(Boolean));
                await this.note?.handle(command.action, this.document, allowed);
                return { state: this.state, requestId: command.requestId };
            } catch (error) {
                return { state: this.state, requestId: command.requestId, error: userFacingMessage(error) };
            }
        }
        const model = this.refresh(this.document, false);
        let error: string | undefined;
        try {
            if (command.role !== this.role) throw new Error('This command belongs to a different Mapping Editor.');
            if (command.action.kind === 'ready') {
                this.opened = true;
                if (this.loadError) {
                    this.loadError = '';
                    this.retryVersion++;
                }
            } else {
                if (
                    this.busy ||
                    !model ||
                    command.context !== this.state.context ||
                    command.version !== this.state.version
                )
                    throw new Error('The selection or its draft changed. Review the current mapping and try again.');
                const action = command.action;
                if (action.kind === 'select') {
                    if (!this.state.selections.some((row) => row.value === action.selectionId && !row.disabled))
                        throw new Error('This selection is no longer available.');
                    model.select(action.selectionId);
                } else if (action.kind === 'range') {
                    const allowed = rangeBoundaries('move', this.state.editableAxes);
                    if (
                        !this.state.editable ||
                        action.selectionId !== this.state.selectionId ||
                        !validRange(action.range) ||
                        !Array.isArray(action.boundaries) ||
                        !action.boundaries.length ||
                        new Set(action.boundaries).size !== action.boundaries.length ||
                        action.boundaries.some((boundary) => !allowed.includes(boundary)) ||
                        (Object.keys(action.range) as (keyof typeof action.range)[]).some(
                            (key) => !action.boundaries.includes(key) && action.range[key] !== this.state.limits?.[key],
                        )
                    )
                        throw new Error('This mapping range cannot be edited.');
                    model.patch(action.range, action.boundaries, action.selectionId);
                } else if (action.kind === 'move') {
                    const target = this.state.targets.find((row) => row.id === action.selectionId);
                    const allowed = target ? rangeBoundaries('move', target.editableAxes) : [];
                    if (
                        !target?.editable ||
                        action.editRevision !== this.state.editRevision ||
                        !validRange(action.original) ||
                        !validRange(action.range) ||
                        !action.boundaries.length ||
                        new Set(action.boundaries).size !== action.boundaries.length ||
                        action.boundaries.some((boundary) => !allowed.includes(boundary)) ||
                        (Object.keys(action.original) as (keyof typeof action.original)[]).some(
                            (boundary) =>
                                action.original[boundary] !== target.limits?.[boundary] ||
                                (!action.boundaries.includes(boundary) &&
                                    action.range[boundary] !== action.original[boundary]),
                        ) ||
                        action.range.high - action.range.low !== action.original.high - action.original.low ||
                        action.range.velocityHigh - action.range.velocityLow !==
                            action.original.velocityHigh - action.original.velocityLow
                    )
                        throw new Error('This mapping moved or changed. Review the current range and try again.');
                    model.patch(action.range, action.boundaries, action.selectionId);
                    model.select(action.selectionId);
                } else if (action.kind === 'root') {
                    if (
                        !this.state.editable ||
                        !this.state.rootEditable ||
                        action.selectionId !== this.state.selectionId ||
                        !Number.isInteger(action.note) ||
                        action.note < 0 ||
                        action.note > 127
                    )
                        throw new Error('The root key cannot be edited in this mapping context.');
                    model.root(action.note);
                } else if (
                    action.kind === 'inherit' &&
                    this.state.editable &&
                    this.state.overrides.some((control) => control.boundary === action.boundary && !control.inherited)
                )
                    model.inherit(action.boundary);
                else if (action.kind === 'undo' && this.state.canUndo) model.undo();
                else if (action.kind === 'redo' && this.state.canRedo) model.redo();
                else if (
                    (action.kind === 'save' && this.state.canSave) ||
                    (action.kind === 'discard' && this.state.canDiscard) ||
                    (action.kind === 'recover' && this.state.recovery)
                ) {
                    this.busy = true;
                    try {
                        await model[action.kind]();
                    } finally {
                        this.busy = false;
                    }
                } else throw new Error('This action is not available for the current selection.');
            }
        } catch (reason) {
            error = userFacingMessage(reason);
        }
        this.refresh(this.document, false);
        return { state: this.state, requestId: command.requestId, error };
    }
}
