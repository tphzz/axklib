import type { ImageTransport } from '../../lib/transport';
import { userFacingMessage } from '../../lib/userFacingMessage';
import { objectEditorAdapter } from './registry';
import type { EditorValue, EditorValues } from './draft.svelte';
import type { ObjectEditorDocument } from './workflow.svelte';

interface ComparisonEntry {
    id: string;
    name: string;
    values?: EditorValues;
    error?: string;
}

export class EditorComparison {
    entries = $state.raw<ComparisonEntry[]>([]);
    private sessionId = 0;
    private scope = '';
    private generation = 0;
    private ids: string[] = [];
    private cache = new Map<string, ComparisonEntry>();
    private pending = new Map<string, Promise<ComparisonEntry>>();
    private active = 0;
    private waiting: (() => void)[] = [];

    constructor(
        private readonly transport: Pick<ImageTransport, 'objectDetail'>,
        private readonly getDocument: (sessionId: number, id: string) => ObjectEditorDocument | undefined,
    ) {}

    get count(): number {
        return this.entries.length;
    }
    get status(): string {
        if (this.count < 2) return '';
        const errors = this.entries.filter(
            (entry) => entry.error || this.getDocument(this.sessionId, entry.id)?.conflict,
        ).length;
        const pending = this.entries.filter((entry) => !entry.values && !entry.error).length;
        return pending
            ? `Loading comparison (${pending} remaining)`
            : errors
              ? `Comparison incomplete: ${errors} Sample${errors === 1 ? '' : 's'} unavailable`
              : `Comparing ${this.count} Samples`;
    }
    values(entry: ComparisonEntry): EditorValues | undefined {
        const document = this.getDocument(this.sessionId, entry.id);
        return document?.conflict ? undefined : (document?.draft.values ?? entry.values);
    }
    differing(key: string, read: (values: EditorValues) => EditorValue | undefined = (values) => values[key]): boolean {
        const values = this.entries
            .filter((entry) => !entry.error)
            .map((entry) => this.values(entry))
            .filter(Boolean);
        return values.length > 1 && values.some((value) => read(value!) !== read(values[0]!));
    }
    description(
        key: string,
        format: (value: EditorValue | undefined) => string = (value) =>
            value === undefined ? 'Unavailable' : String(value),
        read: (values: EditorValues) => EditorValue | undefined = (values) => values[key],
    ): string {
        return this.entries
            .map((entry) => {
                const document = this.getDocument(this.sessionId, entry.id);
                const values = this.values(entry);
                return `${entry.name}: ${document?.conflict || entry.error || (values ? format(read(values)) : 'Loading')}`;
            })
            .join('\n');
    }
    async select(sessionId: number, revision: number, ids: string[]): Promise<void> {
        const generation = ++this.generation;
        const scope = `${sessionId}:${revision}`;
        if (this.scope !== scope) this.cache.clear();
        this.scope = scope;
        this.sessionId = sessionId;
        this.ids = [...new Set(ids)];
        if (this.ids.length < 2) {
            this.entries = [];
            return;
        }
        this.entries = this.ids.map((id) => this.cache.get(id) ?? { id, name: id });
        await Promise.all(
            this.ids.map(async (id) => {
                const entry = this.cache.get(id) ?? (await this.load(scope, sessionId, id));
                if (generation !== this.generation) return;
                this.entries = this.entries.map((previous) => (previous.id === id ? entry : previous));
            }),
        );
    }
    clear(): void {
        this.generation++;
        this.scope = '';
        this.ids = [];
        this.entries = [];
        this.cache.clear();
    }
    private load(scope: string, sessionId: number, id: string): Promise<ComparisonEntry> {
        const key = `${scope}:${id}`;
        const pending = this.pending.get(key);
        if (pending) return pending;
        const task = this.fetch(scope, sessionId, id).finally(() => this.pending.delete(key));
        this.pending.set(key, task);
        return task;
    }
    private async fetch(scope: string, sessionId: number, id: string): Promise<ComparisonEntry> {
        if (this.active >= 4) await new Promise<void>((resolve) => this.waiting.push(resolve));
        else this.active++;
        let entry: ComparisonEntry = { id, name: id };
        try {
            if (scope !== this.scope || !this.ids.includes(id)) return entry;
            const detail = await this.transport.objectDetail(sessionId, id);
            const adapter = objectEditorAdapter(detail);
            entry = {
                id,
                name: detail.object.name,
                ...(adapter
                    ? { values: adapter.values(detail.editing!) }
                    : { error: 'Parameter comparison is unavailable for this Sample format' }),
            };
        } catch (error) {
            entry = { ...entry, error: userFacingMessage(error) };
        } finally {
            const next = this.waiting.shift();
            if (next) next();
            else this.active--;
        }
        if (scope === this.scope) this.cache.set(id, entry);
        return entry;
    }
}
