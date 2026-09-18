import type {
    FilesystemAccess,
    FilesystemEntry,
    FilesystemQuery,
    FilesystemRootCapabilities,
} from '../../lib/filesystem';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { ObjectSelectionMode } from '../../lib/objectSelection';
import { updateFilesSelection, type FilesSelection } from './selection';
import { relocateContext, type FilesRelocation } from './relocateContext';

interface RootState {
    focused: FilesystemEntry | null;
    selection: FilesSelection;
    expanded: FilesystemEntry[];
    loadedCounts: Record<string, number>;
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
    relocation?: FilesRelocation;
}

export class FilesController {
    roots = $state<FilesystemEntry[]>([]);
    rootId = $state('');
    available = $state(false);
    deviceView = $state<string | null>(null);
    filesystemName = $state('');
    revision = $state(0);
    initialized = $state(false);
    viewReady = $state(false);
    error = $state('');
    busy = $state(false);
    revealId = $state('');
    revealSequence = $state(0);
    private rootCapabilities = $state<FilesystemRootCapabilities[]>([]);
    private states = $state<Record<string, RootState>>({});
    private pendingStates: Record<string, RootState> = {};
    private children = $state<Record<string, FilesystemEntry[]>>({});
    private counts = $state<Record<string, number>>({});
    private loading = new Set<string>();
    private generation = 0;
    private searchGeneration = 0;
    private disposed = false;
    private relocation: FilesRelocation | undefined;

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
                if (state.expanded.some((item) => item.id === entry.id)) visit(entry.id, depth + 1);
            }
        };
        visit(this.rootId, 0);
        return rows;
    }
    expanded(id: string): boolean {
        return this.states[this.rootId]?.expanded.some((entry) => entry.id === id) ?? false;
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
        const states = JSON.parse(JSON.stringify({ ...this.states, ...this.pendingStates })) as Record<
            string,
            RootState
        >;
        const entries = new Map(
            [...this.roots, ...Object.values(this.children).flat()].map((entry) => [entry.id, entry]),
        );
        for (const [id, children] of Object.entries(this.children)) {
            const parent = entries.get(id);
            if (parent && states[parent.rootId] && !this.pendingStates[parent.rootId])
                states[parent.rootId].loadedCounts[parent.path] = children.length;
        }
        return {
            revision: this.revision,
            rootId: this.rootId,
            states,
            relocation: this.relocation,
        };
    }

    recordRename(fromRevision: number, entry: FilesystemEntry, name: string, toRevision: number): void {
        if (this.revision === fromRevision && toRevision === fromRevision + 1)
            this.relocation = {
                fromRevision,
                toRevision,
                entries: [
                    { entry: { ...entry }, name, path: entry.path.slice(0, entry.path.lastIndexOf('/') + 1) + name },
                ],
            };
    }

    recordMove(
        fromRevision: number,
        entries: FilesystemEntry[],
        destination: FilesystemEntry,
        toRevision: number,
    ): void {
        if (this.revision === fromRevision && toRevision === fromRevision + 1)
            this.relocation = {
                fromRevision,
                toRevision,
                destination,
                entries: entries.map((entry) => ({
                    entry: { ...entry },
                    name: entry.name,
                    path: destination.path.replace(/\/$/, '') + '/' + entry.name,
                })),
            };
    }

    async reviewChildren(parentId: string, revision: number): Promise<FilesystemEntry[]> {
        const result: FilesystemEntry[] = [];
        while (true) {
            const page = await this.access.inspect({ parentId, offset: result.length, limit: 200 });
            if (page.revision !== revision || this.revision !== revision || this.disposed)
                throw new Error('The image changed. Choose the entries again.');
            result.push(...page.items);
            if (result.length >= page.totalCount) return result;
            if (!page.items.length || result.length >= 100000)
                throw new Error('The destination exceeds the review limit.');
        }
    }

    async initialize(context = this.capture()): Promise<void> {
        const generation = ++this.generation;
        this.searchGeneration += 1;
        this.children = {};
        this.counts = {};
        this.loading.clear();
        this.initialized = false;
        this.viewReady = false;
        this.rootCapabilities = [];
        this.busy = true;
        this.error = '';
        try {
            const first = await this.access.inspect({ limit: 200 });
            if (!this.current(generation)) return;
            context = relocateContext(context, first.revision);
            this.relocation = undefined;
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
                            loadedCounts: saved?.loadedCounts ?? {},
                            query: saved?.query ?? '',
                            results: [],
                            resultCount: 0,
                            scrollTop: saved?.scrollTop ?? 0,
                        },
                    ];
                }),
            );
            this.pendingStates = Object.fromEntries(
                roots.filter((root) => context.states[root.id]).map((root) => [root.id, context.states[root.id]]),
            );
            const rootId = roots.find((root) => root.id === context.rootId)?.id ?? roots[0]?.id;
            if (rootId) {
                await this.chooseRoot(rootId);
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
        this.viewReady = false;
        this.busy = false;
        this.rootId = id;
        this.states[id] ??= {
            focused: null,
            selection: { items: [], anchorId: '' },
            expanded: [],
            loadedCounts: {},
            query: '',
            results: [],
            resultCount: 0,
            scrollTop: 0,
        };
        const generation = this.generation;
        const saved = this.pendingStates[id];
        if (saved) await this.restoreRoot(id, saved);
        else {
            if (!(id in this.children)) await this.loadChildren(id);
            if (this.current(generation) && this.rootId === id && this.states[id].query)
                await this.search(this.states[id].query);
        }
        if (this.current(generation) && this.rootId === id) this.viewReady = true;
    }

    private async restoreRoot(id: string, saved: RootState): Promise<void> {
        const generation = this.generation;
        const active = (): boolean => this.current(generation) && this.rootId === id;
        const state = this.states[id];
        const resolved = new Map<string, FilesystemEntry>();
        for (const entry of [...saved.expanded, ...saved.selection.items, ...(saved.focused ? [saved.focused] : [])]) {
            if (resolved.has(entry.id)) continue;
            const current = await this.restoreEntry(entry).catch(() => null);
            if (!active()) return;
            if (current) resolved.set(entry.id, current);
        }
        state.expanded = saved.expanded.flatMap((entry) => resolved.get(entry.id) ?? []);
        const restored = saved.selection.items.flatMap((entry) => resolved.get(entry.id) ?? []);
        const focused = saved.focused ? resolved.get(saved.focused.id) : undefined;
        const targets = [...restored, ...(focused ? [focused] : [])];
        await this.loadExpanded(
            this.roots.find((root) => root.id === id)!,
            state,
            targets,
        );
        if (!active()) return;
        if (saved.query) {
            await this.search(saved.query);
            if (!active()) return;
            const pending = new Set(targets.map((entry) => entry.id));
            let checked = 0;
            while (pending.size || state.results.length < saved.results.length) {
                for (const entry of state.results.slice(checked)) pending.delete(entry.id);
                if ((!pending.size && state.results.length >= saved.results.length) || !this.moreResults) break;
                checked = state.results.length;
                await this.search(saved.query, true);
                if (!active()) return;
                if (state.results.length === checked) break;
            }
        }
        const visible = new Set(this.rows.map((row) => row.entry.id));
        state.selection = {
            items: restored.filter((entry) => visible.has(entry.id)),
            anchorId: resolved.get(saved.selection.anchorId)?.id ?? '',
        };
        state.focused = focused && visible.has(focused.id) ? focused : (state.selection.items[0] ?? null);
        state.scrollTop = saved.scrollTop;
        delete this.pendingStates[id];
    }

    // Refill visible branches without changing expansion, selection, or navigation.
    private async loadExpanded(
        parent: FilesystemEntry,
        state: RootState,
        targets: FilesystemEntry[] = [],
    ): Promise<void> {
        if (parent.ancestorIds.length > 64) return;
        const generation = this.generation;
        const required = new Set<string>();
        for (const entry of [...state.expanded, ...targets]) {
            const chain = [...entry.ancestorIds, entry.id];
            const index = chain.indexOf(parent.id);
            if (index >= 0 && index + 1 < chain.length) required.add(chain[index + 1]);
        }
        if (!(parent.id in this.children)) await this.loadChildren(parent.id);
        while (this.current(generation)) {
            const children = this.children[parent.id] ?? [];
            for (const child of children) required.delete(child.id);
            if (
                !this.hasMore(parent.id) ||
                (!required.size && children.length >= (state.loadedCounts[parent.path] ?? 0))
            )
                break;
            await this.loadChildren(parent.id);
            if (children.length === this.children[parent.id]?.length) break;
        }
        if (!this.current(generation)) return;
        for (const child of this.children[parent.id] ?? []) {
            if (child.kind === 'directory' && state.expanded.some((entry) => entry.id === child.id))
                await this.loadExpanded(child, state, targets);
            if (!this.current(generation)) return;
        }
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
        if (state.expanded.some((item) => item.id === entry.id)) {
            state.expanded = state.expanded.filter((item) => item.id !== entry.id);
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
            state.expanded = [...state.expanded, entry];
            await this.loadExpanded(entry, state);
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
            const directory = this.children[chain[index - 1]]?.find((item) => item.id === parent);
            if (directory && !state.expanded.some((item) => item.id === parent)) state.expanded.push(directory);
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
