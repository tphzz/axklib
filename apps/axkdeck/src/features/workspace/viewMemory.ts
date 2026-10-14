export type RememberView = <T>(key: string, initial: () => T) => T;

// Only presentation state is retained; native data always comes from the current revision.
export class ViewMemory {
    private session: number | null = null;
    private states = new Map<string, unknown>();

    forDocument(session: number | null, document: string | null): RememberView {
        if (session !== this.session) {
            this.session = session;
            this.states.clear();
        }
        const states = this.states;
        return <T>(key: string, initial: () => T): T => {
            const scoped = `${document ?? 'files'}:${key}`;
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
    const lists = () =>
        Array.from(node.querySelectorAll<HTMLElement>('[data-navigation-list], .image-tree-scroll, .object-scroll'));
    const key = (element: HTMLElement, index: number) =>
        `${element.getAttribute('aria-label') ?? element.className}:${index}`;
    const restore = () =>
        lists().forEach((element, index) => {
            const position = state.scroll.get(key(element, index));
            if (position) {
                element.scrollTop = position.top;
                element.scrollLeft = position.left;
            }
        });
    const capture = (event: Event) => {
        const index = lists().indexOf(event.target as HTMLElement);
        if (index < 0) return;
        const element = event.target as HTMLElement;
        state.scroll.set(key(element, index), { top: element.scrollTop, left: element.scrollLeft });
    };
    const observer = new MutationObserver(restore);
    observer.observe(node, { childList: true, subtree: true });
    node.addEventListener('scroll', capture, true);
    restore();
    return {
        destroy() {
            observer.disconnect();
            node.removeEventListener('scroll', capture, true);
        },
    };
}
