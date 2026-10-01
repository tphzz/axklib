const breakpoints = [240, 360, 420, 500, 600, 650, 700, 850, 900, 1100, 1200];

// WebKitGTK native zoom can evaluate container queries in a different coordinate
// space from layout. Keep all editor breakpoints in measured CSS layout pixels.
export function measureWidth(
    node: HTMLElement,
    { scope, change }: { scope: string; change?: (width: number) => void },
) {
    let previous = -1;
    function measure() {
        const width = node.clientWidth;
        if (width === previous) return;
        previous = width;
        node.setAttribute(`data-${scope}-under`, breakpoints.filter((point) => width < point).join(' '));
        node.setAttribute(`data-${scope}-over`, breakpoints.filter((point) => width >= point).join(' '));
        change?.(width);
    }
    const observer = new ResizeObserver(measure);
    observer.observe(node);
    measure();
    return { destroy: () => observer.disconnect() };
}
