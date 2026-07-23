export interface ModalOptions {
    onescape?: () => void;
}

interface InertState {
    count: number;
    previous: boolean;
}

const inertStates = new WeakMap<HTMLElement, InertState>();
const focusableSelector =
    'button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), ' +
    'a[href], [tabindex]:not([tabindex="-1"])';

function retainInert(element: HTMLElement): void {
    const state = inertStates.get(element);
    if (state) {
        state.count += 1;
        return;
    }
    inertStates.set(element, { count: 1, previous: Boolean(element.inert) });
    element.inert = true;
}

function releaseInert(element: HTMLElement): void {
    const state = inertStates.get(element);
    if (!state) return;
    state.count -= 1;
    if (state.count > 0) return;
    element.inert = state.previous;
    inertStates.delete(element);
}

function backgroundElements(node: HTMLElement): HTMLElement[] {
    const result: HTMLElement[] = [];
    let current: HTMLElement = node;
    while (current.parentElement) {
        for (const sibling of current.parentElement.children) {
            if (sibling !== current && sibling instanceof HTMLElement) result.push(sibling);
        }
        if (current.parentElement === document.body) break;
        current = current.parentElement;
    }
    return result;
}

function focusableElements(node: HTMLElement): HTMLElement[] {
    return [...node.querySelectorAll<HTMLElement>(focusableSelector)].filter(
        (element) => !element.hidden && !element.inert,
    );
}

export function modal(node: HTMLElement, initialOptions: ModalOptions = {}) {
    let options = initialOptions;
    const previousFocus = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    const background = backgroundElements(node);
    background.forEach(retainInert);
    if (!node.hasAttribute('tabindex')) node.tabIndex = -1;

    const keydown = (event: KeyboardEvent): void => {
        if (event.key === 'Escape' && options.onescape) {
            event.preventDefault();
            options.onescape();
            return;
        }
        if (event.key !== 'Tab') return;
        const focusable = focusableElements(node);
        if (focusable.length === 0) {
            event.preventDefault();
            node.focus();
            return;
        }
        const first = focusable[0];
        const last = focusable.at(-1)!;
        if (event.shiftKey && (document.activeElement === first || !node.contains(document.activeElement))) {
            event.preventDefault();
            last.focus();
        } else if (!event.shiftKey && document.activeElement === last) {
            event.preventDefault();
            first.focus();
        }
    };
    node.addEventListener('keydown', keydown);
    queueMicrotask(() => {
        if (!node.isConnected) return;
        const autofocus = node.querySelector<HTMLElement>('[autofocus]');
        (autofocus ?? focusableElements(node)[0] ?? node).focus();
    });

    return {
        update(next: ModalOptions) {
            options = next;
        },
        destroy() {
            node.removeEventListener('keydown', keydown);
            background.forEach(releaseInert);
            if (previousFocus?.isConnected) previousFocus.focus();
        },
    };
}
