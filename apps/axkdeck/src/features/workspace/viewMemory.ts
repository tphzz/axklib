export type RememberView = <T>(key: string, initial: () => T) => T;

// Only presentation state is retained; native data always comes from the current revision.
export class ViewMemory {
    private session: number | null = null;
    private states = new Map<string, unknown>();

    forDocument(session: number | null, document: string | null): RememberView {
        return this.forScope(session, `document:${document ?? 'none'}`);
    }

    forSource(session: number | null, source: string | null): RememberView {
        return this.forScope(session, `source:${source ?? 'none'}`);
    }

    private forScope(session: number | null, scope: string): RememberView {
        if (session !== this.session) {
            this.session = session;
            this.states.clear();
        }
        const states = this.states;
        return <T>(key: string, initial: () => T): T => {
            const scoped = `${scope}:${key}`;
            if (!states.has(scoped)) states.set(scoped, initial());
            return states.get(scoped) as T;
        };
    }
}

export interface ShellViewState {
    sidebarOpen: boolean;
    splitRatio: number | null;
    scroll: Map<string, { top: number; left: number }>;
}

export function shellViewState(): ShellViewState {
    return { sidebarOpen: true, splitRatio: null, scroll: new Map() };
}

export function retainWorkspaceScroll(node: HTMLElement, state: ShellViewState) {
    type Position = { top: number; left: number };
    const restored = new WeakMap<HTMLElement, { key: string; pending?: Position; applied: Position }>();
    const lists = () =>
        Array.from(node.querySelectorAll<HTMLElement>('[data-navigation-list], .image-tree-scroll, .object-scroll'));
    const key = (element: HTMLElement, index: number) =>
        `${element.getAttribute('aria-label') ?? element.className}:${index}`;
    const position = (element: HTMLElement): Position => ({ top: element.scrollTop, left: element.scrollLeft });
    const matches = (left: Position, right: Position) =>
        Math.abs(left.top - right.top) < 1 && Math.abs(left.left - right.left) < 1;
    const restore = () =>
        lists().forEach((element, index) => {
            const id = key(element, index);
            let record = restored.get(element);
            if (record?.key !== id) {
                record = { key: id, pending: state.scroll.get(id) ?? { top: 0, left: 0 }, applied: position(element) };
                restored.set(element, record);
            }
            if (!record.pending) return;
            const current = position(element);
            if (!matches(current, record.applied)) {
                record.pending = undefined;
                state.scroll.set(id, current);
                return;
            }
            // Retry clamped restoration while rows load, but never replay it over live navigation.
            element.scrollTop = record.pending.top;
            element.scrollLeft = record.pending.left;
            record.applied = position(element);
            if (matches(record.applied, record.pending)) record.pending = undefined;
        });
    const capture = (event: Event) => {
        const index = lists().indexOf(event.target as HTMLElement);
        if (index < 0) return;
        const element = event.target as HTMLElement;
        const current = position(element);
        const record = restored.get(element);
        if (record?.pending && matches(current, record.applied)) return;
        if (record) record.pending = undefined;
        state.scroll.set(key(element, index), current);
    };
    const observer = new MutationObserver(restore);
    observer.observe(node, { childList: true, subtree: true, attributes: true, attributeFilter: ['aria-label'] });
    node.addEventListener('scroll', capture, true);
    restore();
    return {
        destroy() {
            observer.disconnect();
            node.removeEventListener('scroll', capture, true);
        },
    };
}
