import { on } from 'svelte/events';

export interface ModalOptions {
    onescape?: () => void;
}

interface ModalEntry {
    node: HTMLElement;
    backdrop: HTMLElement | null;
    layer: string;
    layerPriority: string;
    previousFocus: HTMLElement | null;
}

const modalStack: ModalEntry[] = [];
const inertBackground = new Map<HTMLElement, boolean>();
let activeModalCount = 0;

function retainScrollbarMode(): () => void {
    if (activeModalCount === 0) {
        const measurement = document.createElement('div');
        measurement.className = 'modal-scrollbar-measurement';
        document.body.append(measurement);
        // Remove GTK's painted thumb outline only when even a stable gutter consumes no space.
        const overlays = measurement.offsetWidth > 0 && measurement.offsetWidth === measurement.clientWidth;
        measurement.remove();
        document.documentElement.classList.toggle('modal-overlay-scrollbars', overlays);
    }
    activeModalCount += 1;
    return () => {
        activeModalCount -= 1;
        if (activeModalCount === 0) document.documentElement.classList.remove('modal-overlay-scrollbars');
    };
}

const focusableSelector =
    'button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), ' +
    'a[href], [tabindex]:not([tabindex="-1"])';

function synchronizeModals(): void {
    const top = modalStack.at(-1);
    const background = new Set(top ? backgroundElements(top.node) : []);
    // Only the top modal defines the inert boundary; older snapshots can contain its new host.
    for (const [element, previous] of inertBackground) {
        if (background.has(element)) continue;
        element.inert = previous;
        inertBackground.delete(element);
    }
    for (const element of background) {
        if (inertBackground.has(element)) continue;
        inertBackground.set(element, Boolean(element.inert));
        element.inert = true;
    }
    modalStack.forEach((entry, index) => entry.backdrop?.style.setProperty('--modal-layer', String(50 + index * 10)));
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
    return [...node.querySelectorAll<HTMLElement>(focusableSelector)].filter(canFocus);
}

function canFocus(element: HTMLElement): boolean {
    if (!element.isConnected || element.matches(':disabled')) return false;
    for (let ancestor: HTMLElement | null = element; ancestor; ancestor = ancestor.parentElement) {
        if (ancestor.hidden || ancestor.inert) return false;
    }
    return true;
}

function initialFocusElement(node: HTMLElement): HTMLElement | null {
    return node.querySelector<HTMLElement>('[data-dialog-initial-focus]:not(:disabled)');
}

function focusInitialElement(element: HTMLElement): void {
    element.focus({ preventScroll: true });
    if (element.dataset.dialogInitialFocus === 'select' && element instanceof HTMLInputElement) element.select();
}

export function modal(node: HTMLElement, initialOptions: ModalOptions = {}) {
    let options = initialOptions;
    const backdrop = node.closest<HTMLElement>('.dialog-backdrop');
    const entry: ModalEntry = {
        node,
        backdrop,
        layer: backdrop?.style.getPropertyValue('--modal-layer') ?? '',
        layerPriority: backdrop?.style.getPropertyPriority('--modal-layer') ?? '',
        previousFocus: document.activeElement instanceof HTMLElement ? document.activeElement : null,
    };
    const releaseScrollbarMode = retainScrollbarMode();
    modalStack.push(entry);
    synchronizeModals();
    if (!node.hasAttribute('tabindex')) node.tabIndex = -1;

    const keydown = (event: KeyboardEvent): void => {
        if (modalStack.at(-1) !== entry || event.defaultPrevented) return;
        if (event.key === 'Escape' && options.onescape) {
            event.preventDefault();
            event.stopPropagation();
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
    const removeKeydown = on(node, 'keydown', keydown);
    let userInteracted = false;
    const markInteraction = (): void => {
        if (modalStack.at(-1) === entry) userInteracted = true;
    };
    node.addEventListener('pointerdown', markInteraction);
    node.addEventListener('input', markInteraction);
    node.addEventListener('keydown', markInteraction);
    const observer = new MutationObserver(() => {
        if (userInteracted || modalStack.at(-1) !== entry) return;
        const initial = initialFocusElement(node);
        if (!initial) return;
        focusInitialElement(initial);
        observer.disconnect();
    });
    observer.observe(node, { childList: true, subtree: true, attributes: true, attributeFilter: ['disabled'] });
    queueMicrotask(() => {
        if (!node.isConnected || modalStack.at(-1) !== entry) return;
        const initial = initialFocusElement(node);
        if (initial) {
            focusInitialElement(initial);
            observer.disconnect();
            return;
        }
        const autofocus = node.querySelector<HTMLElement>('[autofocus]');
        (autofocus ?? focusableElements(node)[0] ?? node).focus();
    });

    return {
        update(next: ModalOptions) {
            options = next;
        },
        destroy() {
            const index = modalStack.indexOf(entry);
            if (index === -1) return;
            const wasTop = modalStack.at(-1) === entry;
            removeKeydown();
            node.removeEventListener('pointerdown', markInteraction);
            node.removeEventListener('input', markInteraction);
            node.removeEventListener('keydown', markInteraction);
            observer.disconnect();
            modalStack.splice(index, 1);
            for (const remaining of modalStack) {
                if (remaining.previousFocus && node.contains(remaining.previousFocus)) {
                    remaining.previousFocus = entry.previousFocus;
                }
            }
            if (entry.layer) backdrop?.style.setProperty('--modal-layer', entry.layer, entry.layerPriority);
            else backdrop?.style.removeProperty('--modal-layer');
            synchronizeModals();
            releaseScrollbarMode();
            if (!wasTop) return;
            const top = modalStack.at(-1)?.node;
            const previous = entry.previousFocus;
            if (previous && canFocus(previous) && (!top || top.contains(previous))) {
                previous.focus({ preventScroll: true });
            } else if (top) {
                (focusableElements(top)[0] ?? top).focus({ preventScroll: true });
            }
        },
    };
}
