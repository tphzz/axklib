import type { KeyboardRange, RangeBoundary } from '../object-editor/keyboardMapping';
import { selectMapping } from './model';
import type { MappingAction, MappingCommand, MappingMessage, MappingRole, MappingSnapshot } from './protocol';

export interface MappingWindowAdapter {
    send(command: MappingCommand): Promise<void>;
    listen(callback: (message: MappingMessage) => void): Promise<() => void>;
}
type Gesture = { context: string; version: number; editRevision: number; selectionId: number; original: KeyboardRange };
type CompletedGesture = { identity: Gesture; range: KeyboardRange; boundaries: RangeBoundary[]; move: boolean };
export class MappingClient {
    snapshot = $state.raw<MappingSnapshot | null>(null);
    status = $state('Connecting to the main editor');
    pending = $state('');
    selection = $state<number | null>(null);
    preview = $state.raw<KeyboardRange | null>(null);
    private request: MappingAction | null = null;
    private gesture: Gesture | null = null;
    private completed: CompletedGesture | null = null;
    private boundaries: RangeBoundary[] = [];
    private timeout: ReturnType<typeof setTimeout> | undefined;
    private mounted = true;
    private readonly retired = new Set<string>();
    private readonly audioClient = crypto.randomUUID();
    private sequence = 0;
    private held = 0;
    private lease: ReturnType<typeof setInterval> | undefined;
    private audioRequest = '';
    constructor(
        readonly role: MappingRole,
        private readonly adapter: MappingWindowAdapter,
    ) {}
    get state(): MappingSnapshot | null {
        return this.snapshot && this.selection !== null
            ? { ...this.snapshot, ...selectMapping(this.snapshot, this.selection) }
            : this.snapshot;
    }
    get locked(): boolean {
        return !!this.pending && this.request?.kind !== 'select';
    }
    select(id: number): void {
        if (this.locked || !this.snapshot?.targets.some((target) => target.id === id)) return;
        this.selection = id;
        if (!this.pending && !this.gesture) void this.flushSelection();
    }
    private async flushSelection() {
        if (this.selection === null || this.selection === this.snapshot?.selectionId) {
            this.selection = null;
            return;
        }
        await this.send({ kind: 'select', selectionId: this.selection });
    }
    async send(action: MappingAction, identity = this.snapshot): Promise<void> {
        if (this.pending) return;
        const requestId = crypto.randomUUID();
        this.pending = requestId;
        this.request = action;
        this.status = '';
        this.timeout = setTimeout(() => {
            if (this.pending === requestId) {
                this.pending = '';
                this.request = null;
                this.selection = null;
                this.cancel();
                this.status = 'The main editor did not respond. Refresh to check its current snapshot.';
            }
        }, 10000);
        try {
            await this.adapter.send({
                role: this.role,
                requestId,
                context: identity?.context ?? '',
                version: identity?.version ?? 0,
                action,
            });
        } catch (error) {
            if (this.mounted && this.pending === requestId) {
                clearTimeout(this.timeout);
                this.pending = '';
                this.request = null;
                this.selection = null;
                this.cancel();
                this.status = String(error);
            }
        }
    }
    begin(id?: string): void {
        const state = this.state;
        const selectionId = id ? state?.zones.find((zone) => zone.id === id)?.selectionId : state?.selectionId;
        const target = state?.targets.find((target) => target.id === selectionId);
        if (!state || this.locked || !target?.editable || !target.limits) return;
        this.selection = target.id;
        this.gesture = {
            context: state.context,
            version: state.version,
            editRevision: state.editRevision,
            selectionId: target.id,
            original: { ...target.limits },
        };
        this.boundaries = [];
        this.preview = { ...target.limits };
    }
    change(range: KeyboardRange, boundaries: RangeBoundary[]): void {
        if (!this.gesture) return;
        this.preview = range;
        this.boundaries = [...new Set([...this.boundaries, ...boundaries])];
    }
    end(cancelled = false, move = false): void {
        const gesture = this.gesture,
            range = this.preview;
        const boundaries = this.boundaries.filter((boundary) => range?.[boundary] !== gesture?.original[boundary]);
        this.cancel();
        if (
            cancelled ||
            !gesture ||
            !range ||
            !boundaries.length ||
            !this.snapshot ||
            gesture.context !== this.snapshot.context ||
            gesture.version !== this.snapshot.version ||
            gesture.editRevision !== this.snapshot.editRevision
        ) {
            this.selection = null;
            return;
        }
        if (this.pending) {
            this.completed = { identity: gesture, range, boundaries, move };
            return;
        }
        this.commit({ identity: gesture, range, boundaries, move });
    }
    private commit({ identity: gesture, range, boundaries, move }: CompletedGesture): void {
        void this.send(
            move
                ? {
                      kind: 'move',
                      selectionId: gesture.selectionId,
                      editRevision: gesture.editRevision,
                      original: gesture.original,
                      range,
                      boundaries,
                  }
                : { kind: 'range', selectionId: gesture.selectionId, range, boundaries },
        );
    }
    cancel(): void {
        this.gesture = null;
        this.preview = null;
        this.boundaries = [];
        this.completed = null;
    }
    press(note: number, velocity: number): void {
        this.release();
        if (!this.snapshot?.context || this.locked) return;
        this.status = '';
        this.held = ++this.sequence;
        this.audioRequest = crypto.randomUUID();
        void this.audio(
            {
                kind: 'note',
                clientId: this.audioClient,
                sequence: this.held,
                note,
                velocity,
                imageRevision: this.snapshot.imageRevision,
            },
            this.audioRequest,
        );
        this.lease = setInterval(() => {
            if (this.held) void this.audio({ kind: 'lease', clientId: this.audioClient, sequence: this.held });
        }, 2000);
    }
    release(): void {
        clearInterval(this.lease);
        if (!this.held) return;
        const sequence = this.held;
        this.held = 0;
        this.audioRequest = '';
        void this.audio({ kind: 'release', clientId: this.audioClient, sequence });
    }
    private async audio(
        action: Extract<MappingAction, { clientId: string }>,
        requestId: string = crypto.randomUUID(),
    ): Promise<void> {
        try {
            await this.adapter.send({
                role: this.role,
                requestId,
                context: this.snapshot?.context ?? '',
                version: this.snapshot?.version ?? 0,
                action,
            });
        } catch (error) {
            if (this.mounted && this.sequence === action.sequence) {
                this.status = String(error);
                this.release();
            }
        }
    }
    receive(message: MappingMessage): void {
        if (!this.mounted || message.state.role !== this.role || this.retired.has(message.state.owner)) return;
        if (message.requestId === this.audioRequest && message.error) {
            this.status = message.error;
            this.release();
        }
        const ownSelection =
            this.request?.kind === 'select' &&
            !message.error &&
            message.state.context === this.snapshot?.context &&
            message.state.editRevision === this.snapshot.editRevision &&
            message.state.selectionId === this.request.selectionId;
        if (
            !this.snapshot ||
            message.state.owner !== this.snapshot.owner ||
            message.state.version > this.snapshot.version
        ) {
            if (this.snapshot && message.state.owner !== this.snapshot.owner) this.retired.add(this.snapshot.owner);
            if (this.snapshot && message.state.context !== this.snapshot.context) this.release();
            if (
                this.gesture &&
                (this.gesture.context !== message.state.context || this.gesture.version !== message.state.version)
            ) {
                if (ownSelection) this.gesture.version = message.state.version;
                else this.cancel();
            }
            if (this.completed && this.completed.identity.version !== message.state.version) {
                if (ownSelection) this.completed.identity.version = message.state.version;
                else this.cancel();
            }
            if (
                !ownSelection &&
                (message.state.context !== this.snapshot?.context ||
                    message.state.editRevision !== this.snapshot?.editRevision)
            )
                this.selection = null;
            this.snapshot = message.state;
        }
        if (message.requestId === this.pending) {
            const selected = this.request?.kind === 'select';
            clearTimeout(this.timeout);
            this.pending = '';
            this.request = null;
            this.status = message.error ?? '';
            if (message.error) {
                this.selection = null;
                this.cancel();
            } else if (this.completed && ownSelection) {
                const completed = this.completed;
                this.completed = null;
                this.commit(completed);
            } else if (selected && !this.gesture) void this.flushSelection();
            else if (!this.gesture) this.selection = null;
        }
    }
    async connect(): Promise<() => void> {
        const stop = await this.adapter.listen((message) => this.receive(message));
        if (!this.mounted) {
            stop();
            return () => {};
        }
        void this.send({ kind: 'ready' });
        const heartbeat = setInterval(() => {
            if (!this.gesture)
                void this.adapter
                    .send({
                        role: this.role,
                        requestId: crypto.randomUUID(),
                        context: this.snapshot?.context ?? '',
                        version: this.snapshot?.version ?? 0,
                        action: { kind: 'ready' },
                    })
                    .catch((error) => {
                        if (this.mounted) this.status = String(error);
                    });
        }, 5000);
        return () => {
            this.release();
            this.mounted = false;
            clearTimeout(this.timeout);
            clearInterval(heartbeat);
            this.cancel();
            stop();
        };
    }
}
