import type {
    FilesystemAccess,
    FilesystemEntry,
    FilesystemQuery,
    FilesystemRootCapabilities,
} from '../../lib/filesystem';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { ObjectSelectionMode } from '../../lib/objectSelection';
import { updateFilesSelection, type FilesSelection } from './selection';

interface RootState {
    focused: FilesystemEntry | null;
    selection: FilesSelection;
    expanded: string[];
    query: string;
    results: FilesystemEntry[];
    resultCount: number;
    scrollTop: number;
}
export interface FileRow {
    entry: FilesystemEntry;
    depth: number;
}
export interface FilesContext {
    revision: number;
    rootId: string;
    states: Record<string, RootState>;
}

export class FilesController {
    roots = $state<FilesystemEntry[]>([]);
    rootId = $state('');
    available = $state(false);
    deviceView = $state<string | null>(null);
    filesystemName = $state('');
    revision = $state(0);
    initialized = $state(false);
    error = $state('');
    busy = $state(false);
    revealId = $state('');
    revealSequence = $state(0);
    private rootCapabilities = $state<FilesystemRootCapabilities[]>([]);
    private states = $state<Record<string, RootState>>({});
    private children = $state<Record<string, FilesystemEntry[]>>({});
    private counts = $state<Record<string, number>>({});
    private loading = new Set<string>();
    private generation = 0;
    private searchGeneration = 0;
    private disposed = false;

    constructor(private readonly access: FilesystemAccess) {}

    get root(): FilesystemEntry | null {
        return this.roots.find((root) => root.id === this.rootId) ?? null;
    }
    get selected(): FilesystemEntry | null {
        return this.selection.find((entry) => entry.id === this.focused?.id) ?? this.selection[0] ?? null;
    }
    get focused(): FilesystemEntry | null {
        return this.states[this.rootId]?.focused ?? null;
    }
    get selection(): FilesystemEntry[] {
        return this.states[this.rootId]?.selection.items ?? [];
    }
    get capabilities(): FilesystemRootCapabilities | null {
        return this.capabilitiesFor(this.rootId);
    }
    capabilitiesFor(rootId: string): FilesystemRootCapabilities | null {
        if (!this.initialized || this.busy || this.error) return null;
        return this.rootCapabilities.find((root) => root.rootId === rootId) ?? null;
    }
    get query(): string {
        return this.states[this.rootId]?.query ?? '';
    }
    get scrollTop(): number {
        return this.states[this.rootId]?.scrollTop ?? 0;
    }
    set scrollTop(value: number) {
        if (this.states[this.rootId]) this.states[this.rootId].scrollTop = value;
    }
    get moreResults(): boolean {
        const state = this.states[this.rootId];
        return Boolean(state && state.results.length < state.resultCount);
    }
    get rows(): FileRow[] {
        const state = this.states[this.rootId];
        if (!state) return [];
        if (state.query) return state.results.map((entry) => ({ entry, depth: 0 }));
        const rows: FileRow[] = [];
        const visit = (parent: string, depth: number): void => {
            if (depth > 64) return;
            for (const entry of this.children[parent] ?? []) {
                rows.push({ entry, depth });
                if (state.expanded.includes(entry.id)) visit(entry.id, depth + 1);
            }
        };
        visit(this.rootId, 0);
        return rows;
    }
    expanded(id: string): boolean {
        return this.states[this.rootId]?.expanded.includes(id) ?? false;
    }
    hasMore(id: string): boolean {
        return (this.children[id]?.length ?? 0) < (this.counts[id] ?? 0);
    }
    focus(entry: FilesystemEntry): void {
        if (entry.rootId === this.rootId && this.states[this.rootId]) this.states[this.rootId].focused = entry;
    }
    select(entry: FilesystemEntry, mode: ObjectSelectionMode = 'replace'): void {
        const state = this.states[this.rootId];
        if (!state || entry.rootId !== this.rootId) return;
        state.selection = updateFilesSelection(
            state.selection,
            this.rows.map((row) => row.entry),
            entry,
            mode,
        );
        this.focus(entry);
    }
    selectForContext(entry: FilesystemEntry): void {
        if (this.selection.some((item) => item.id === entry.id)) this.focus(entry);
        else this.select(entry);
    }
    clearSelection(): void {
        const state = this.states[this.rootId];
        if (state) state.selection = { items: [], anchorId: '' };
    }

    capture(): FilesContext {
        return {
            revision: this.revision,
            rootId: this.rootId,
            states: JSON.parse(JSON.stringify(this.states)) as Record<string, RootState>,
        };
    }

    async initialize(context = this.capture()): Promise<void> {
        const generation = ++this.generation;
        this.searchGeneration += 1;
        this.children = {};
        this.counts = {};
        this.loading.clear();
        this.initialized = false;
        this.rootCapabilities = [];
        this.busy = true;
        this.error = '';
        try {
            const first = await this.access.inspect({ limit: 200 });
            if (!this.current(generation)) return;
            const roots = [...first.items];
            while (roots.length < first.totalCount) {
                const page = await this.access.inspect({ offset: roots.length, limit: 200 });
                if (!this.current(generation)) return;
                if (!page.items.length) throw new Error('Filesystem root listing stopped before completion');
                roots.push(...page.items);
            }
            this.roots = roots;
            this.available = first.available;
            this.deviceView = first.deviceView;
            this.filesystemName = first.filesystemName;
            this.revision = first.revision;
            this.rootCapabilities = first.rootCapabilities;
            this.states = Object.fromEntries(
                roots.map((root) => {
                    const saved = context.states[root.id];
                    return [
                        root.id,
                        {
                            focused: null,
                            selection: { items: [], anchorId: '' },
                            expanded: [],
                            query: saved?.query ?? '',
                            results: [],
                            resultCount: 0,
                            scrollTop: saved?.scrollTop ?? 0,
                        },
                    ];
                }),
            );
            const rootId = roots.find((root) => root.id === context.rootId)?.id ?? roots[0]?.id;
            if (rootId) {
                await this.chooseRoot(rootId);
                if (!this.current(generation)) return;
                const saved = context.states[rootId];
                // Opaque identities can be reused after directory records change.
                for (const id of context.revision === first.revision ? (saved?.expanded ?? []) : []) {
                    const entry = await this.lookup({ entryId: id }).catch(() => null);
                    if (!this.current(generation)) return;
                    if (entry?.rootId === rootId) {
                        await this.reveal(entry);
                        if (!this.expanded(id)) await this.toggle(entry);
                    }
                }
                const restored: FilesystemEntry[] = [];
                const restoredById = new Map<string, FilesystemEntry>();
                for (const entry of saved?.selection.items ?? []) {
                    const current = await this.restoreEntry(entry).catch(() => null);
                    if (!this.current(generation)) return;
                    if (current?.rootId === rootId && (await this.reveal(current))) {
                        restored.push(current);
                        restoredById.set(entry.id, current);
                    }
                }
                const selected = saved?.focused
                    ? (restoredById.get(saved.focused.id) ?? (await this.restoreEntry(saved.focused).catch(() => null)))
                    : null;
                if (!this.current(generation)) return;
                if (selected?.rootId === rootId) await this.reveal(selected);
                else this.states[rootId].focused = null;
                if (!this.current(generation)) return;
                if (saved?.query) await this.search(saved.query);
                if (!this.current(generation)) return;
                const pending = new Set([...restored, ...(selected ? [selected] : [])].map((entry) => entry.id));
                let checked = 0;
                while (saved?.query && pending.size) {
                    const results = this.states[rootId].results;
                    for (const entry of results.slice(checked)) pending.delete(entry.id);
                    if (!pending.size || !this.moreResults) break;
                    checked = results.length;
                    await this.search(saved.query, true);
                    if (!this.current(generation)) return;
                    if (this.states[rootId].results.length === checked) break;
                }
                const visible = new Set(this.rows.map((row) => row.entry.id));
                this.states[rootId].selection = {
                    items: restored.filter((entry) => visible.has(entry.id)),
                    anchorId: restoredById.get(saved?.selection.anchorId ?? '')?.id ?? selected?.id ?? '',
                };
                this.states[rootId].focused =
                    selected && visible.has(selected.id)
                        ? selected
                        : (restored.find((entry) => visible.has(entry.id)) ?? null);
                this.states[rootId].scrollTop = saved?.scrollTop ?? 0;
                this.revealSequence = 0;
            }
            if (this.current(generation)) this.initialized = true;
        } catch (error) {
            if (this.current(generation)) this.error = userFacingMessage(error);
        } finally {
            if (this.current(generation)) this.busy = false;
        }
    }

    async chooseRoot(id: string): Promise<void> {
        if (!this.roots.some((root) => root.id === id) || this.disposed) return;
        this.searchGeneration += 1;
        this.busy = false;
        this.rootId = id;
        this.states[id] ??= {
            focused: null,
            selection: { items: [], anchorId: '' },
            expanded: [],
            query: '',
            results: [],
            resultCount: 0,
            scrollTop: 0,
        };
        if (!(id in this.children)) await this.loadChildren(id);
        if (this.states[id].query) await this.search(this.states[id].query);
    }

    async loadChildren(id: string): Promise<void> {
        if (this.disposed || this.loading.has(id)) return;
        this.loading.add(id);
        const generation = this.generation;
        try {
            const previous = this.children[id] ?? [];
            const page = await this.access.inspect({ parentId: id, offset: previous.length, limit: 200 });
            if (!this.current(generation)) return;
            if (!page.items.length && previous.length < page.totalCount)
                throw new Error('Filesystem listing stopped before completion');
            this.children[id] = [...previous, ...page.items];
            this.counts[id] = page.totalCount;
        } catch (error) {
            if (this.current(generation)) this.error = userFacingMessage(error);
        } finally {
            this.loading.delete(id);
        }
    }

    async toggle(entry: FilesystemEntry): Promise<void> {
        const state = this.states[this.rootId];
        if (!state || entry.kind === 'file') return;
        if (state.expanded.includes(entry.id)) {
            state.expanded = state.expanded.filter((id) => id !== entry.id);
            const hidden = state.selection.items.some((item) => item.ancestorIds.includes(entry.id));
            state.selection.items = state.selection.items.filter((item) => !item.ancestorIds.includes(entry.id));
            if (hidden) {
                const retained = new Set(state.selection.items.map((item) => item.id));
                retained.add(entry.id);
                state.selection.items = this.rows.map((row) => row.entry).filter((item) => retained.has(item.id));
            }
            if (hidden) state.selection.anchorId = entry.id;
            if (state.focused?.ancestorIds.includes(entry.id)) state.focused = entry;
        } else {
            state.expanded = [...state.expanded, entry.id];
            if (!(entry.id in this.children)) await this.loadChildren(entry.id);
        }
    }

    async search(query: string, append = false): Promise<void> {
        const state = this.states[this.rootId];
        if (!state) return;
        if (query !== state.query) {
            state.focused = null;
            state.selection = { items: [], anchorId: '' };
        }
        state.query = query;
        if (!append) {
            state.results = [];
            state.resultCount = 0;
        }
        const token = ++this.searchGeneration;
        const generation = this.generation;
        if (!query) {
            this.busy = false;
            return;
        }
        this.busy = true;
        try {
            const page = await this.access.inspect({
                rootId: this.rootId,
                query,
                offset: state.results.length,
                limit: 200,
            });
            if (!this.current(generation) || token !== this.searchGeneration) return;
            state.results = [...state.results, ...page.items];
            state.resultCount = page.totalCount;
        } catch (error) {
            if (this.current(generation) && token === this.searchGeneration) this.error = userFacingMessage(error);
        } finally {
            if (this.current(generation) && token === this.searchGeneration) this.busy = false;
        }
    }

    async lookup(query: FilesystemQuery): Promise<FilesystemEntry | null> {
        const generation = this.generation;
        const page = await this.access.inspect({ ...query, limit: 2 });
        return this.current(generation) && page.totalCount === 1 ? (page.items[0] ?? null) : null;
    }

    private async restoreEntry(saved: FilesystemEntry): Promise<FilesystemEntry | null> {
        const samePath = (entry: FilesystemEntry): boolean =>
            entry.rootId === saved.rootId && entry.path === saved.path && entry.kind === saved.kind;
        const byId = await this.lookup({ entryId: saved.id }).catch(() => null);
        if (byId && samePath(byId)) return byId;
        const generation = this.generation;
        let match: FilesystemEntry | null = null;
        let offset = 0;
        while (this.current(generation)) {
            const page = await this.access.inspect({ rootId: saved.rootId, query: saved.name, offset, limit: 200 });
            if (!this.current(generation)) return null;
            for (const entry of page.items) {
                if (!samePath(entry)) continue;
                if (match) return null;
                match = entry;
            }
            offset += page.items.length;
            if (offset >= page.totalCount) return match;
            if (!page.items.length) return null;
        }
        return null;
    }

    async reveal(entry: FilesystemEntry): Promise<boolean> {
        const generation = this.generation;
        await this.chooseRoot(entry.rootId);
        if (!this.current(generation)) return false;
        const state = this.states[entry.rootId];
        if (!state) return false;
        this.searchGeneration += 1;
        state.query = '';
        const chain = [...entry.ancestorIds, entry.id];
        for (let index = 0; index < chain.length - 1; index += 1) {
            const parent = chain[index];
            const child = chain[index + 1];
            if (!this.children[parent]) await this.loadChildren(parent);
            while (
                this.current(generation) &&
                !this.children[parent]?.some((item) => item.id === child) &&
                this.hasMore(parent)
            ) {
                const count = this.children[parent]?.length;
                await this.loadChildren(parent);
                if (count === this.children[parent]?.length) break;
            }
            if (!this.current(generation)) return false;
            if (!this.children[parent]?.some((item) => item.id === child)) {
                this.error ||= 'The entry could not be located in its directory';
                return false;
            }
            if (!state.expanded.includes(parent)) state.expanded.push(parent);
        }
        this.select(entry);
        this.revealId = entry.id;
        this.revealSequence += 1;
        return true;
    }

    dispose(): void {
        this.disposed = true;
        this.generation += 1;
        this.searchGeneration += 1;
    }
    private current(generation: number): boolean {
        return !this.disposed && generation === this.generation;
    }
}
