import type { MappingAudio } from './audition';
import type { ObjectEditorDocument } from '../object-editor/workflow.svelte';
import type { MappingAction, MappingRole } from './protocol';

export class MappingHeldNote {
    private readonly sequences = new Map<string, number>();
    private readonly retired = new Set<string>();
    private active: { clientId: string; sequence: number; token: string } | null = null;
    private clientId = '';
    private timer: ReturnType<typeof setTimeout> | undefined;
    constructor(
        private readonly audio: MappingAudio,
        private readonly role: MappingRole,
    ) {}
    handle(
        action: Extract<MappingAction, { clientId: string }>,
        document: ObjectEditorDocument | null,
        allowed: boolean,
    ): Promise<void> {
        if (
            !action.clientId ||
            action.clientId.length > 128 ||
            !Number.isSafeInteger(action.sequence) ||
            action.sequence < 1
        )
            return Promise.reject(new Error('Invalid held-note identity.'));
        if (action.kind === 'lease') {
            if (this.active?.clientId === action.clientId && this.active.sequence === action.sequence) this.renew();
            return Promise.resolve();
        }
        if (action.kind === 'release') {
            if (!this.sequences.has(action.clientId) && this.sequences.size >= 128) return Promise.resolve();
            this.sequences.set(action.clientId, Math.max(action.sequence, this.sequences.get(action.clientId) ?? 0));
            if (this.active?.clientId === action.clientId && action.sequence >= this.active.sequence) this.stop();
            return Promise.resolve();
        }
        if (!allowed || !document)
            return Promise.reject(new Error('The mapping changed or is busy. Press the key again.'));
        if (this.retired.has(action.clientId) || action.sequence <= (this.sequences.get(action.clientId) ?? 0))
            return Promise.resolve();
        if (
            !Number.isInteger(action.note) ||
            action.note < 0 ||
            action.note > 127 ||
            !Number.isInteger(action.velocity) ||
            action.velocity < 1 ||
            action.velocity > 127
        )
            return Promise.reject(new Error('Invalid audition note or velocity.'));
        if (!this.sequences.has(action.clientId) && this.sequences.size >= 128)
            return Promise.reject(new Error('Reopen the image to reset held-note identities.'));
        this.stop();
        if (this.clientId && this.clientId !== action.clientId) this.retired.add(this.clientId);
        this.clientId = action.clientId;
        this.sequences.set(action.clientId, action.sequence);
        const token = `${action.clientId}:${action.sequence}`;
        this.active = { clientId: action.clientId, sequence: action.sequence, token };
        this.renew();
        return this.audio.play(document, this.role, token, action.note, action.velocity);
    }
    private renew(): void {
        clearTimeout(this.timer);
        this.timer = setTimeout(() => this.stop(), 5000);
    }
    stop(): void {
        clearTimeout(this.timer);
        if (this.active) this.audio.release(this.active.token);
        this.active = null;
    }
    reset(): void {
        this.stop();
        this.sequences.clear();
        this.retired.clear();
        this.clientId = '';
    }
}
