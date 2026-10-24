import type { AudioIdentity } from '../transport';
import { sameAudioIdentity } from './audioIdentity';

interface SelectedRead extends AudioIdentity {
    sessionId: number;
    objectId: string;
    metadata: boolean;
    preview: boolean;
    drawn: boolean;
    prefetch: boolean;
    prefetched: boolean;
}

// Functional selection state must not depend on whether diagnostic capture is enabled.
export class SelectionReadScheduler {
    private selected?: SelectedRead;
    private playing = false;
    private mapping?: () => void;

    constructor(
        private readonly startPrefetch: (sessionId: number, objectId: string) => void,
        private readonly cancelPrefetch: () => void,
    ) {}

    select(sessionId: number, objectId: string, identity: AudioIdentity, requiresPreview: boolean): void {
        if (this.matches(sessionId, objectId, identity)) return;
        this.cancelPrefetch();
        this.mapping = undefined;
        this.selected = {
            ...identity,
            sessionId,
            objectId,
            metadata: true,
            preview: !requiresPreview,
            drawn: false,
            prefetch: false,
            prefetched: false,
        };
    }

    clear(): void {
        this.cancelPrefetch();
        this.selected = undefined;
        this.mapping = undefined;
    }

    metadataCompletion(sessionId: number, objectId: string): () => void {
        const selected = this.selected;
        if (selected?.sessionId === sessionId && selected.objectId === objectId) selected.metadata = false;
        return () => {
            if (
                selected &&
                selected === this.selected &&
                selected.sessionId === sessionId &&
                selected.objectId === objectId
            ) {
                selected.metadata = true;
                this.drain();
            }
        };
    }

    previewReady(sessionId: number, objectId: string, identity: AudioIdentity): void {
        if (this.matches(sessionId, objectId, identity)) {
            this.selected!.preview = true;
            this.drain();
        }
    }

    drawn(
        sessionId: number,
        objectId: string,
        identity: AudioIdentity,
        draw: { bins: number; width: number; height: number },
    ): void {
        if (!this.matches(sessionId, objectId, identity) || draw.bins <= 0 || draw.width <= 1 || draw.height <= 1)
            return;
        this.selected!.drawn = true;
        this.drain();
    }

    prefetch(sessionId: number, objectId: string): void {
        if (this.selected?.sessionId !== sessionId || this.selected.objectId !== objectId) return;
        this.selected.prefetch = true;
        this.drain();
    }

    playbackPending(pending: boolean): void {
        this.playing = pending;
        if (pending) this.cancelPrefetch();
        this.drain();
    }

    afterForeground(action: () => void): () => void {
        this.mapping = action;
        this.drain();
        return () => {
            if (this.mapping === action) this.mapping = undefined;
        };
    }

    private matches(sessionId: number, objectId: string, identity: AudioIdentity): boolean {
        return (
            !!this.selected &&
            this.selected.sessionId === sessionId &&
            this.selected.objectId === objectId &&
            sameAudioIdentity(this.selected, identity)
        );
    }

    private drain(): void {
        if (this.playing) return;
        const selected = this.selected;
        if ((!selected || (selected.metadata && selected.preview)) && this.mapping) {
            const action = this.mapping;
            this.mapping = undefined;
            action();
        }
        if (selected?.drawn && selected.prefetch && !selected.prefetched) {
            selected.prefetched = true;
            this.startPrefetch(selected.sessionId, selected.objectId);
        }
    }
}
