import { fireEvent, render } from '@testing-library/svelte';
import { tick } from 'svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';
import KeyboardMapping from './KeyboardMapping.svelte';

const limits = { low: 24, high: 60, velocityLow: 12, velocityHigh: 110 };
const formatNote = (note: number) => `N${note}`;
function setup(mode: 'mapping' | 'ranges' = 'mapping') {
    const onchange = vi.fn(),
        onbegin = vi.fn(),
        onend = vi.fn(),
        onselect = vi.fn();
    const view = render(KeyboardMapping, {
        zones: [{ ...limits, id: 'sample', label: 'Sample', selected: true }],
        limits,
        formatNote,
        mode,
        onchange,
        onbegin,
        onend,
        onselect,
    });
    vi.spyOn(
        view.container.querySelector(mode === 'mapping' ? '.plot' : '.keyboard-surface')!,
        'getBoundingClientRect',
    ).mockReturnValue({ width: 128, height: 128, left: 0, top: 0 } as DOMRect);
    return { view, onchange, onbegin, onend, onselect };
}
function pointer(target: HTMLElement, type: string, x = 0, y = 0) {
    const event = new Event(type, { bubbles: true });
    Object.assign(event, { pointerId: 1, clientX: x, clientY: y, button: 0 });
    target.setPointerCapture = vi.fn();
    target.hasPointerCapture = () => false;
    return fireEvent(target, event);
}
describe('mapping drag feedback and navigation', () => {
    afterEach(() => vi.unstubAllGlobals());
    it('retains accessible handle names without native hover tooltips', () => {
        const { view } = setup();
        for (const name of ['Low key limit', 'High key limit', 'Low velocity limit', 'High velocity limit'])
            expect(view.getByRole('button', { name }).hasAttribute('title')).toBe(false);
    });
    it('shows the quantized, clamped velocity range only during the gesture', async () => {
        const { view, onchange, onend } = setup();
        let frame!: FrameRequestCallback;
        vi.stubGlobal('requestAnimationFrame', (callback: FrameRequestCallback) => {
            frame = callback;
            return 1;
        });
        vi.stubGlobal('cancelAnimationFrame', vi.fn());
        const handle = view.getByRole('button', { name: 'High velocity limit' });
        await pointer(handle, 'pointerdown');
        expect(view.container.querySelector('[data-drag-readout]')?.textContent).toBe('Velocity: 12 - 110');
        expect(view.getByRole('button', { name: 'Zoom keyboard in' }).hasAttribute('disabled')).toBe(true);
        await pointer(handle, 'pointermove', 0, 300);
        frame(0);
        await tick();
        expect(view.container.querySelector('[data-drag-readout]')?.textContent).toBe('Velocity: 12 - 12');
        expect(onchange).toHaveBeenLastCalledWith({ ...limits, velocityHigh: 12 }, ['velocityHigh']);
        await pointer(handle, 'pointerup', 0, 300);
        expect(view.container.querySelector('[data-drag-readout]')).toBeNull();
        expect(onend).toHaveBeenCalledExactlyOnceWith(false, false);
    });
    it('uses the persistent compact summary slot and clears feedback on cancellation', async () => {
        const { view, onend } = setup('ranges');
        const summary = view.container.querySelector('.summary');
        const handle = view.getByRole('button', { name: 'Low key limit' });
        await pointer(handle, 'pointerdown');
        expect(view.container.querySelector('[data-drag-readout]')).toBe(summary);
        expect(summary?.textContent).toBe('Keys: 24 (N24) - 60 (N60)');
        await fireEvent.keyDown(window, { key: 'Escape' });
        expect(view.container.querySelector('[data-drag-readout]')).toBeNull();
        expect(onend).toHaveBeenCalledExactlyOnceWith(true, false);
    });
    it('shows keyboard edits until key release, and clears on blur or selection replacement', async () => {
        const { view } = setup();
        const handle = view.getByRole('button', { name: 'High key limit' });
        await fireEvent.keyDown(handle, { key: 'ArrowLeft', shiftKey: true });
        expect(view.container.querySelector('[data-drag-readout]')?.textContent).toBe('Keys: 24 (N24) - 48 (N48)');
        await fireEvent.keyUp(handle, { key: 'ArrowLeft' });
        expect(view.container.querySelector('[data-drag-readout]')).toBeNull();
        await fireEvent.keyDown(handle, { key: 'ArrowLeft' });
        await fireEvent.blur(handle);
        expect(view.container.querySelector('[data-drag-readout]')).toBeNull();
        await pointer(handle, 'pointerdown');
        await fireEvent.blur(handle);
        expect(view.container.querySelector('[data-drag-readout]')).toBeNull();
        await pointer(handle, 'pointerdown');
        await view.rerender({ zones: [{ ...limits, id: 'other', label: 'Other', selected: true }] });
        expect(view.container.querySelector('[data-drag-readout]')).toBeNull();
    });
    it('pans a zoomed read-only viewport without editing, selecting or auditioning anything', async () => {
        const { view, onchange, onbegin, onend, onselect } = setup();
        expect(view.queryByRole('scrollbar')).toBeNull();
        await view.rerender({ disabled: true });
        await fireEvent.click(view.getByRole('button', { name: 'Zoom keyboard in' }));
        const rail = view.getByRole('scrollbar', { name: 'Keyboard viewport' });
        expect(rail.getAttribute('aria-valuenow')).toBe('32');
        await fireEvent.keyDown(rail, { key: 'ArrowRight', shiftKey: true });
        expect(rail.getAttribute('aria-valuenow')).toBe('44');
        await fireEvent.keyDown(rail, { key: 'End' });
        expect(rail.getAttribute('aria-valuenow')).toBe('64');
        expect(onchange).not.toHaveBeenCalled();
        expect(onbegin).not.toHaveBeenCalled();
        expect(onend).not.toHaveBeenCalled();
        expect(onselect).not.toHaveBeenCalled();
    });
    it('keeps movement readouts active for an already-selected group', async () => {
        const { view, onend } = setup();
        await view.rerender({
            zones: [
                { ...limits, id: 'sample', label: 'Sample', selected: true },
                { ...limits, low: 72, high: 80, id: 'second', label: 'Second', selected: true },
            ],
        });
        let frame!: FrameRequestCallback;
        vi.stubGlobal('requestAnimationFrame', (callback: FrameRequestCallback) => {
            frame = callback;
            return 1;
        });
        vi.stubGlobal('cancelAnimationFrame', vi.fn());
        const block = view.getByRole('button', { name: 'Select mapping Sample' });
        await pointer(block, 'pointerdown');
        await pointer(block, 'pointermove', 10);
        frame(0);
        await tick();
        expect(view.container.querySelector('[data-drag-readout]')?.textContent).toBe(
            'Keys: 34 (N34) - 70 (N70) / Velocity: 12 - 110',
        );
        expect(onend).not.toHaveBeenCalled();
        await pointer(block, 'pointerup', 10);
        expect(onend).toHaveBeenCalledExactlyOnceWith(false, true);
    });
    it('restores the original center when a compact pan is canceled by a wider sizing cap', async () => {
        const observers: Array<(entries: { contentRect: { width: number; height: number } }[]) => void> = [];
        vi.stubGlobal(
            'ResizeObserver',
            class {
                constructor(callback: (entries: { contentRect: { width: number; height: number } }[]) => void) {
                    observers.push(callback);
                }
                observe() {}
                disconnect() {}
            },
        );
        const { view, onchange } = setup('ranges');
        observers[0]!([{ contentRect: { width: 500, height: 56 } }]);
        await tick();
        await fireEvent.click(view.getByRole('button', { name: 'Zoom keyboard in' }));
        const rail = view.getByRole('scrollbar');
        rail.setPointerCapture = vi.fn();
        rail.hasPointerCapture = () => false;
        vi.spyOn(rail, 'getBoundingClientRect').mockReturnValue({ width: 500, left: 0 } as DOMRect);
        expect(rail.getAttribute('aria-valuenow')).toBe('32');
        await pointer(view.container.querySelector('.viewport')! as HTMLElement, 'pointerdown', 160);
        observers[0]!([{ contentRect: { width: 700, height: 56 } }]);
        await tick();
        observers[1]!([]);
        await tick();
        expect(rail.getAttribute('aria-valuenow')).toBe('29');
        expect(view.container.querySelector('.keys')?.getAttribute('viewBox')).toBe('29 0 70 28');
        expect(onchange).not.toHaveBeenCalled();
    });
});
