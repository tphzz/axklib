import { fireEvent, render } from '@testing-library/svelte';
import { tick } from 'svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';
import MappingOverview from './MappingOverview.svelte';
import { MappingGeometryCache } from './keyboardGeometry';

const geometry = new MappingGeometryCache().read([
    { id: 'one', label: 'One', low: 24, high: 60, velocityLow: 0, velocityHigh: 127 },
]);
function setup() {
    const onchange = vi.fn();
    const view = render(MappingOverview, {
        start: 32,
        span: 32,
        geometry,
        selected: new Set(['one']),
        formatNote: String,
        controls: 'keys',
        onchange,
    });
    const rail = view.getByRole('scrollbar');
    rail.setPointerCapture = vi.fn();
    rail.hasPointerCapture = () => false;
    vi.spyOn(rail, 'getBoundingClientRect').mockReturnValue({ width: 128, left: 0 } as DOMRect);
    return { view, rail, onchange };
}
function pointer(target: Element, type: string, x: number, id = 1) {
    const event = new Event(type, { bubbles: true });
    Object.assign(event, { pointerId: id, button: 0, clientX: x, clientY: 0 });
    return fireEvent(target, event);
}
describe('shared mapping overview rail', () => {
    afterEach(() => vi.unstubAllGlobals());
    it('pans the viewport without changing its span and rolls back on Escape', async () => {
        const { view, rail, onchange } = setup();
        let frame!: FrameRequestCallback;
        vi.stubGlobal('requestAnimationFrame', (callback: FrameRequestCallback) => {
            frame = callback;
            return 1;
        });
        vi.stubGlobal('cancelAnimationFrame', vi.fn());
        await pointer(view.container.querySelector('.viewport')!, 'pointerdown', 36);
        await pointer(rail, 'pointermove', 64);
        expect(onchange).toHaveBeenCalledTimes(1);
        frame(0);
        await tick();
        expect(onchange).toHaveBeenLastCalledWith({ start: 60, span: 32 });
        await fireEvent.keyDown(window, { key: 'Escape' });
        expect(onchange).toHaveBeenLastCalledWith({ start: 32, span: 32 });
    });
    it.each(['pointercancel', 'lostpointercapture'])('recenters rail clicks and restores on %s', async (type) => {
        const { rail, onchange } = setup();
        await pointer(rail, 'pointerdown', 127);
        expect(onchange).toHaveBeenLastCalledWith({ start: 96, span: 32 });
        await pointer(rail, type, 127);
        expect(onchange).toHaveBeenLastCalledWith({ start: 32, span: 32 });
    });
    it('flushes the final release, ignores foreign pointers, and keeps cached coverage nodes', async () => {
        const { view, rail, onchange } = setup();
        const rect = view.container.querySelector('rect');
        await pointer(view.container.querySelector('.viewport')!, 'pointerdown', 36);
        await pointer(rail, 'pointerup', 127, 2);
        expect(onchange).toHaveBeenCalledTimes(1);
        await pointer(rail, 'pointerup', 64);
        expect(onchange).toHaveBeenLastCalledWith({ start: 60, span: 32 });
        await view.rerender({ start: 60 });
        expect(view.container.querySelector('rect')).toBe(rect);
        expect(rail.getAttribute('aria-valuenow')).toBe('60');
    });
    it('supports keyboard navigation without accepting disabled interactions', async () => {
        const { view, rail, onchange } = setup();
        await fireEvent.keyDown(rail, { key: 'ArrowRight' });
        expect(onchange).toHaveBeenLastCalledWith({ start: 33, span: 32 });
        await view.rerender({ disabled: true });
        await pointer(rail, 'pointerdown', 100);
        await fireEvent.keyDown(rail, { key: 'End' });
        expect(onchange).toHaveBeenCalledTimes(1);
        expect(rail.getAttribute('tabindex')).toBe('-1');
    });
    it('cancels without retaining a drag on window blur or destruction', async () => {
        const { view, rail, onchange } = setup();
        await pointer(rail, 'pointerdown', 100);
        await fireEvent.blur(window);
        expect(onchange).toHaveBeenLastCalledWith({ start: 32, span: 32 });
        await pointer(rail, 'pointerdown', 100);
        view.unmount();
        expect(onchange).toHaveBeenLastCalledWith({ start: 32, span: 32 });
    });
    it('cancels a frozen pointer gesture when the rail resizes or loses focus', async () => {
        let resized!: () => void;
        vi.stubGlobal(
            'ResizeObserver',
            class {
                constructor(callback: () => void) {
                    resized = callback;
                }
                observe() {}
                disconnect() {}
            },
        );
        const { rail, onchange } = setup();
        await pointer(rail, 'pointerdown', 100);
        resized();
        expect(onchange).toHaveBeenLastCalledWith({ start: 32, span: 32 });
        await pointer(rail, 'pointerdown', 100);
        await fireEvent.blur(rail);
        expect(onchange).toHaveBeenLastCalledWith({ start: 32, span: 32 });
    });
});
