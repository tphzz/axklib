import { getContext, setContext } from 'svelte';
import type { ImageTransport, VolumeCapacityInspection } from './transport';
import type { DiskTreeItem } from './types';
import { userFacingMessage } from './userFacingMessage';

export type VolumeCapacityState =
    | { status: 'loading' }
    | { status: 'ready'; inspection: VolumeCapacityInspection }
    | { status: 'error'; message: string };

interface Context {
    sessionId: number | null;
    revision: number;
    enabled: boolean;
    transport: Pick<ImageTransport, 'inspectVolumeCapacity'>;
}

const contextKey = Symbol('volume-capacity');

export class VolumeCapacityController {
    private selection = $state<DiskTreeItem | null>(null);
    private selectionSession = $state<number | null>(null);
    private entries = $state<Record<string, VolumeCapacityState>>({});
    private pending = new Map<string, Promise<void>>();
    private generation = '';

    constructor(private readonly context: () => Context) {}

    get enabled(): boolean {
        return this.context().enabled;
    }

    get selectedVolume(): DiskTreeItem | null {
        return this.enabled && this.selectionSession === this.context().sessionId ? this.selection : null;
    }

    private key(scopeId: string): string {
        const context = this.context();
        return `${context.sessionId}:${context.revision}:${scopeId}`;
    }

    state(scopeId: string): VolumeCapacityState | undefined {
        return this.entries[this.key(scopeId)];
    }

    inspectionKey(scopeId: string): string {
        return this.key(scopeId);
    }

    selectVolume(item: DiskTreeItem): void {
        if (!this.enabled || item.kind !== 'volume') return;
        this.selection = item;
        this.selectionSession = this.context().sessionId;
        void this.inspect(item.id);
    }

    showObject(): void {
        this.selection = null;
    }

    async inspect(scopeId: string, retry = false): Promise<void> {
        const context = this.context();
        if (!context.enabled || context.sessionId === null) return;
        const generation = `${context.sessionId}:${context.revision}`;
        if (this.generation !== generation) {
            this.entries = {};
            this.pending.clear();
            this.generation = generation;
        }
        const key = this.key(scopeId);
        const pending = this.pending.get(key);
        if (pending) return pending;
        if (this.entries[key] && !retry) return;
        this.entries[key] = { status: 'loading' };
        const request = Promise.resolve().then(async () => {
            try {
                const inspection = await context.transport.inspectVolumeCapacity(context.sessionId!, scopeId);
                if (this.key(scopeId) !== key) return;
                if (inspection.revision !== context.revision || inspection.contentScopeId !== scopeId)
                    throw new Error('Capacity inspection does not match the current volume revision');
                this.entries[key] = { status: 'ready', inspection };
            } catch (error) {
                if (this.key(scopeId) === key)
                    this.entries[key] = { status: 'error', message: userFacingMessage(error) };
            } finally {
                if (this.pending.get(key) === request) this.pending.delete(key);
            }
        });
        this.pending.set(key, request);
        await request;
    }
}

export function provideVolumeCapacity(context: () => Context): VolumeCapacityController {
    return setContext(contextKey, new VolumeCapacityController(context));
}

export function useVolumeCapacity(): VolumeCapacityController | undefined {
    return getContext<VolumeCapacityController | undefined>(contextKey);
}
