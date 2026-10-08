export interface MappingViewport {
    start: number;
    span: number;
}

export const compactKeyboardWidth = 1280;
export function minimumKeyboardSpan(width: number, compact: boolean): number {
    return compact ? Math.min(128, Math.max(12, Math.ceil(Math.min(width, compactKeyboardWidth) / 10))) : 12;
}
export function clampViewport(view: MappingViewport, minimum = 12): MappingViewport {
    const span = Math.max(minimum, Math.min(128, Math.round(view.span)));
    return { start: Math.max(0, Math.min(128 - span, Math.round(view.start))), span };
}
function centered(view: MappingViewport, span: number, minimum: number): MappingViewport {
    const next = clampViewport({ start: 0, span }, minimum).span;
    return clampViewport({ start: view.start + (view.span - next) / 2, span: next }, minimum);
}
export const resizeViewport = (view: MappingViewport, minimum: number) => centered(view, view.span, minimum);
export const zoomViewport = (view: MappingViewport, factor: number, minimum: number) =>
    centered(view, view.span * factor, minimum);
export function fitViewport(low: number, high: number, minimum: number): MappingViewport {
    const span = clampViewport({ start: 0, span: high - low + 13 }, minimum).span;
    return clampViewport({ start: (low + high + 1 - span) / 2, span }, minimum);
}
export function viewportKey(view: MappingViewport, key: string, shift = false): MappingViewport | null {
    const step = shift ? 12 : 1;
    const offsets: Record<string, number> = {
        ArrowLeft: -step,
        ArrowRight: step,
        PageUp: -view.span,
        PageDown: view.span,
        Home: -128,
        End: 128,
    };
    return key in offsets ? clampViewport({ ...view, start: view.start + offsets[key]! }) : null;
}
