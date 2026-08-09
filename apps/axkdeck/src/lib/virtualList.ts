export interface VirtualViewportState {
    scrollTop: number;
    height: number;
}

export interface FixedVirtualWindow {
    startIndex: number;
    endIndex: number;
    offset: number;
    totalHeight: number;
}

const initialVisibleRows = 24;

export function fixedVirtualWindow(
    itemCount: number,
    viewport: VirtualViewportState,
    itemExtent: number,
    overscan = 6,
): FixedVirtualWindow {
    if (itemCount <= 0) return { startIndex: 0, endIndex: 0, offset: 0, totalHeight: 0 };
    const visibleRows = viewport.height > 0 ? Math.ceil(viewport.height / itemExtent) : initialVisibleRows;
    const windowSize = visibleRows + overscan * 2;
    const firstVisibleIndex = Math.floor(Math.max(0, viewport.scrollTop) / itemExtent);
    const requestedStart = Math.max(0, firstVisibleIndex - overscan);
    const startIndex = Math.min(requestedStart, Math.max(0, itemCount - windowSize));
    const endIndex = Math.min(itemCount, startIndex + windowSize);
    return {
        startIndex,
        endIndex,
        offset: startIndex * itemExtent,
        totalHeight: itemCount * itemExtent,
    };
}

export function virtualViewport(
    node: HTMLElement,
    update: (viewport: VirtualViewportState) => void,
): { destroy: () => void } {
    let lastScrollTop = -1;
    let lastHeight = -1;
    const publish = () => {
        const scrollTop = node.scrollTop;
        const height = node.clientHeight;
        if (scrollTop === lastScrollTop && height === lastHeight) return;
        lastScrollTop = scrollTop;
        lastHeight = height;
        update({ scrollTop, height });
    };
    const resizeObserver = typeof ResizeObserver === 'undefined' ? null : new ResizeObserver(publish);
    node.addEventListener('scroll', publish, { passive: true });
    resizeObserver?.observe(node);
    publish();
    return {
        destroy: () => {
            node.removeEventListener('scroll', publish);
            resizeObserver?.disconnect();
        },
    };
}
