import { on } from 'svelte/events';

export function popupAnchorAvailable(anchor: HTMLElement): boolean {
    if (!anchor.isConnected) return false;
    for (let element: HTMLElement | null = anchor; element; element = element.parentElement) {
        if (element.matches(':disabled') || element.inert || element.hidden) return false;
    }
    return true;
}

export function mountSelectPopup(node: HTMLElement, anchor: HTMLElement, close: () => void) {
    const host = anchor.closest<HTMLElement>('[role="dialog"][aria-modal="true"]') ?? document.body;
    host.append(node);
    const rect = anchor.getBoundingClientRect();
    const scale = anchor.offsetWidth ? rect.width / anchor.offsetWidth || 1 : 1;
    const hostScale = host.offsetWidth ? host.getBoundingClientRect().width / host.offsetWidth || 1 : 1;
    node.style.zoom = String(scale / hostScale);
    node.style.width = `${Math.min(Math.max(rect.width, 200 * scale), window.innerWidth - 16) / scale}px`;
    const below = Math.max(0, window.innerHeight - rect.bottom - 12);
    const above = Math.max(0, rect.top - 12);
    node.style.maxHeight = `${Math.min(260 * scale, Math.max(below, above)) / scale}px`;
    const upwards = below < node.getBoundingClientRect().height && above > below;
    node.style.maxHeight = `${Math.min(260 * scale, upwards ? above : below) / scale}px`;
    function position() {
        const bounds = node.getBoundingClientRect();
        node.style.left = `${Math.max(8, Math.min(rect.left, window.innerWidth - bounds.width - 8)) / scale}px`;
        node.style.top = `${(upwards ? rect.top - bounds.height - 4 : rect.bottom + 4) / scale}px`;
    }
    position();
    const observer = new ResizeObserver(position);
    observer.observe(node);
    const outside = (event: Event) => {
        if (event.target instanceof Node && !anchor.contains(event.target) && !node.contains(event.target)) close();
    };
    const stateObserver = new MutationObserver(() => {
        if (!popupAnchorAvailable(anchor)) close();
    });
    for (let element: HTMLElement | null = anchor; element; element = element.parentElement) {
        stateObserver.observe(element, { attributes: true, attributeFilter: ['disabled', 'inert', 'hidden'] });
    }
    const remove = [
        on(window, 'pointerdown', outside, { capture: true }),
        on(window, 'focusin', outside),
        on(window, 'resize', close),
        on(
            window,
            'scroll',
            (event) => {
                if (event.target instanceof Node && node.contains(event.target)) return;
                const current = anchor.getBoundingClientRect();
                if (current.top !== rect.top || current.left !== rect.left) close();
            },
            { capture: true },
        ),
    ];
    return {
        destroy() {
            observer.disconnect();
            stateObserver.disconnect();
            remove.forEach((dispose) => dispose());
            node.remove();
        },
    };
}
