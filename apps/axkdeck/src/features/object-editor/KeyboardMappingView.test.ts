import { fireEvent, render } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';
import KeyboardMapping from './KeyboardMapping.svelte';
import { MappingGeometryCache } from './keyboardGeometry';

const limits = { low: 0, high: 127, velocityLow: 12, velocityHigh: 110 };
const formatNote = (note: number) => `C${note / 12 - 2}`;
function setup(props = {}) {
    const onchange = vi.fn();
    const view = render(KeyboardMapping, {
        zones: [{ ...limits, id: 'sample', label: 'Sample', selected: true }],
        limits,
        formatNote,
        onselect: vi.fn(),
        onchange,
        ...props,
    });
    return { view, onchange };
}

describe('shared keyboard mapping view', () => {
    afterEach(() => vi.unstubAllGlobals());
    it('does not rebuild geometry during focus, zoom, pan or local limits previews', async () => {
        const read = vi.spyOn(MappingGeometryCache.prototype, 'read');
        const { view } = setup({ mode: 'mapping' });
        const count = read.mock.calls.length;
        const geometry = read.mock.results[0]!.value;
        try {
            await fireEvent.focus(view.getByRole('button', { name: 'C3' }));
            await fireEvent.click(view.getByRole('button', { name: 'Zoom keyboard in' }));
            await fireEvent.click(view.getByRole('button', { name: 'Pan keyboard right' }));
            expect(read.mock.calls.length).toBe(count);
            await view.rerender({ limits: { ...limits, low: 24, high: 80 } });
            expect(read.mock.results.every((result) => result.type === 'return' && result.value === geometry)).toBe(
                true,
            );
        } finally {
            read.mockRestore();
        }
    });
    it('transforms the viewport without repositioning every static mapping hit target', async () => {
        const { view } = setup({ mode: 'mapping' });
        const zone = view.container.querySelector<HTMLElement>('.zone')!;
        const fills = view.container.querySelector<HTMLElement>('.region-fills')!;
        const position = [zone.style.left, zone.style.width];
        const transform = fills.style.transform;
        await fireEvent.click(view.getByRole('button', { name: 'Zoom keyboard in' }));
        await fireEvent.click(view.getByRole('button', { name: 'Pan keyboard right' }));
        expect([zone.style.left, zone.style.width]).toEqual(position);
        expect(fills.style.transform).not.toBe(transform);
    });
    it('restores a boundary when a pointer gesture returns to its origin', async () => {
        const { view, onchange } = setup();
        const surface = view.container.querySelector('.keyboard-surface')!;
        vi.spyOn(surface, 'getBoundingClientRect').mockReturnValue({ width: 128, height: 28 } as DOMRect);
        let frame!: FrameRequestCallback;
        vi.stubGlobal('requestAnimationFrame', (callback: FrameRequestCallback) => {
            frame = callback;
            return 1;
        });
        vi.stubGlobal('cancelAnimationFrame', vi.fn());
        const handle = view.getByRole('button', { name: 'Low key limit' });
        handle.setPointerCapture = vi.fn();
        handle.hasPointerCapture = () => false;
        const pointer = async (type: string, x: number) => {
            const event = new Event(type, { bubbles: true });
            Object.assign(event, { pointerId: 1, clientX: x, clientY: 14 });
            await fireEvent(handle, event);
        };
        await pointer('pointerdown', 0);
        await pointer('pointermove', 4);
        frame(0);
        expect(onchange).toHaveBeenLastCalledWith({ ...limits, low: 4 }, ['low']);
        await pointer('pointerup', 0);
        expect(onchange).toHaveBeenLastCalledWith(limits, ['low']);
    });
    it('reports only velocity boundaries for a purely vertical pointer move', async () => {
        const { view, onchange } = setup({ mode: 'mapping', limits: { ...limits, low: 60, high: 60 } });
        const plot = view.container.querySelector('.plot')!;
        vi.spyOn(plot, 'getBoundingClientRect').mockReturnValue({ width: 128, height: 128 } as DOMRect);
        const handle = view.getByRole('button', { name: 'Move Range' });
        handle.setPointerCapture = vi.fn();
        handle.hasPointerCapture = () => false;
        const pointer = (type: string, y: number) => {
            const event = new Event(type, { bubbles: true });
            Object.assign(event, { pointerId: 1, clientX: 60, clientY: y });
            return fireEvent(handle, event);
        };
        await pointer('pointerdown', 50);
        await pointer('pointerup', 49);
        expect(onchange).toHaveBeenLastCalledWith({ low: 60, high: 60, velocityLow: 13, velocityHigh: 111 }, [
            'velocityLow',
            'velocityHigh',
        ]);
    });
    it('uses only the keyboard in compact mode and labels every C', async () => {
        const { view, onchange } = setup();
        expect(view.container.querySelector('.plot')).toBeNull();
        expect(view.container.querySelector('.velocity-axis')).toBeNull();
        expect(view.container.querySelector('.legend')).toBeNull();
        expect(view.container.querySelectorAll('.key-labels span')).toHaveLength(11);
        const handle = view.getByRole('button', { name: 'Low key limit' });
        expect(handle.closest('.keyboard-surface')).not.toBeNull();
        await fireEvent.keyDown(handle, { key: 'ArrowRight' });
        expect(onchange).toHaveBeenCalledWith({ ...limits, low: 1 }, ['low']);
    });

    it('configures editable axes and labels without device-specific terminology', async () => {
        const { view, onchange } = setup({
            mode: 'mapping',
            editableAxes: { keys: false, velocity: true },
            rangeLabel: 'Bank overrides',
        });
        expect(view.queryByRole('button', { name: 'Low key limit' })).toBeNull();
        expect(view.getByRole('button', { name: 'Move Bank overrides' })).toBeTruthy();
        await fireEvent.keyDown(view.getByRole('button', { name: 'Low velocity limit' }), { key: 'ArrowUp' });
        expect(onchange).toHaveBeenCalledWith({ ...limits, velocityLow: 13 }, ['velocityLow']);
        await fireEvent.keyDown(view.getByRole('button', { name: 'Move Bank overrides' }), { key: 'ArrowRight' });
        expect(onchange).toHaveBeenCalledTimes(1);
    });

    it('marks unselected mappings and keeps editable limits independent of coverage', () => {
        const { view } = setup({
            zones: [
                { ...limits, low: 24, high: 36, id: 'one', label: 'One', selected: true },
                { ...limits, low: 60, high: 72, id: 'two', label: 'Two' },
            ],
        });
        expect(view.container.querySelector('[data-note="60"]')?.getAttribute('class')).toContain('marked');
        expect(view.container.querySelector('.compact-limits')).not.toBeNull();
        expect(view.container.querySelector('[data-note="24"]')?.getAttribute('fill')).not.toBe(
            view.container.querySelector('[data-note="60"]')?.getAttribute('fill'),
        );
    });

    it('guides the selected effective region rather than wider editable limits', async () => {
        const { view } = setup({
            mode: 'mapping',
            zones: [
                { ...limits, low: 24, high: 60, id: 'one', label: 'One', selected: true },
                { ...limits, low: 72, high: 100, id: 'two', label: 'Two' },
            ],
        });
        expect([...view.container.querySelectorAll('.mapping-label')].map((label) => label.textContent)).toEqual([
            'One',
            'Two',
        ]);
        expect(view.container.querySelector('.range-guide[data-boundary="low"]')?.getAttribute('x1')).toBe('24');
        expect(view.container.querySelector('.range-guide[data-boundary="high"]')?.getAttribute('x1')).toBe('61');
        await view.rerender({ limits: { ...limits, velocityLow: 0, velocityHigh: 49 } });
        expect(view.container.querySelector('.range-guide[data-boundary="velocityHigh"]')?.getAttribute('y1')).toBe(
            '17',
        );
        expect(view.container.querySelector('.range-guide[data-boundary="velocityLow"]')?.getAttribute('y1')).toBe(
            '116',
        );
        expect(view.container.querySelector('.zone')?.getAttribute('style')).toContain('13.28125%');
    });

    it('guides the outer envelope of a selected group without inventing covered gaps', () => {
        const { view } = setup({
            mode: 'mapping',
            zones: [
                {
                    ...limits,
                    low: 24,
                    high: 36,
                    velocityLow: 20,
                    velocityHigh: 70,
                    id: 'a',
                    label: 'A',
                    selected: true,
                },
                {
                    ...limits,
                    low: 60,
                    high: 72,
                    velocityLow: 40,
                    velocityHigh: 100,
                    id: 'b',
                    label: 'B',
                    selected: true,
                },
                { ...limits, id: 'empty', label: 'Empty', selected: true, empty: true },
            ],
        });
        expect(view.container.querySelectorAll('.range-guide')).toHaveLength(4);
        expect(view.container.querySelector('.range-guide[data-boundary="low"]')?.getAttribute('x1')).toBe('24');
        expect(view.container.querySelector('.range-guide[data-boundary="high"]')?.getAttribute('x1')).toBe('73');
        expect(view.container.querySelector('.range-guide[data-boundary="velocityHigh"]')?.getAttribute('y1')).toBe(
            '27',
        );
        expect(view.container.querySelector('.range-guide[data-boundary="velocityLow"]')?.getAttribute('y1')).toBe(
            '108',
        );
        expect(view.container.querySelectorAll('.selection-outline')).toHaveLength(2);
        expect(view.container.querySelector('[data-note="48"]')?.getAttribute('class')).not.toContain('marked');
    });

    it('omits coverage guides for an empty selection and batches the fixed grid', () => {
        const { view } = setup({ mode: 'mapping', zones: [] });
        expect(view.container.querySelectorAll('.range-guide')).toHaveLength(0);
        expect(view.container.querySelectorAll('.grid-overlay path')).toHaveLength(4);
        expect([...view.container.querySelectorAll('.velocity-axis span')].map((node) => node.textContent)).toEqual(
            Array.from({ length: 26 }, (_, index) => String(index * 5)),
        );
    });
    it('uses one shared overlap pattern while retaining every keyboard contributor', () => {
        const { view } = setup({
            zones: [
                { ...limits, id: 'a', label: 'A', selected: true },
                { ...limits, id: 'b', label: 'B' },
            ],
        });
        expect(view.container.querySelectorAll('pattern')).toHaveLength(1);
        expect(view.container.querySelector('[data-note="24"] title')?.textContent).toContain('A, B');
    });

    it('skips unchanged quantized pointer updates but flushes the final release', async () => {
        const { view, onchange } = setup();
        vi.spyOn(view.container.querySelector('.keyboard-surface')!, 'getBoundingClientRect').mockReturnValue({
            width: 1280,
            height: 28,
        } as DOMRect);
        let frame!: FrameRequestCallback;
        vi.stubGlobal('requestAnimationFrame', (callback: FrameRequestCallback) => {
            frame = callback;
            return 1;
        });
        vi.stubGlobal('cancelAnimationFrame', vi.fn());
        const handle = view.getByRole('button', { name: 'Low key limit' });
        handle.setPointerCapture = vi.fn();
        handle.hasPointerCapture = () => false;
        const pointer = (type: string, x: number) => {
            const event = new Event(type, { bubbles: true });
            Object.assign(event, { pointerId: 1, clientX: x, clientY: 14 });
            return fireEvent(handle, event);
        };
        await pointer('pointerdown', 0);
        await pointer('pointermove', 1);
        frame(0);
        await pointer('pointermove', 2);
        frame(0);
        expect(onchange).not.toHaveBeenCalled();
        await pointer('pointerup', 10);
        expect(onchange).toHaveBeenCalledExactlyOnceWith({ ...limits, low: 1 }, ['low']);
    });

    it('retains selection guides and source outlines in read-only full views', () => {
        const { view } = setup({
            mode: 'mapping',
            disabled: true,
            zones: [{ ...limits, id: 'one', label: 'One', source: limits, selected: true }],
        });
        expect(view.container.querySelectorAll('.range-guide')).toHaveLength(4);
        expect(view.container.querySelector('.source')).not.toBeNull();
        expect(view.container.querySelector('.chosen')).not.toBeNull();
        expect(view.getByRole('button', { name: 'Low key limit' }).hasAttribute('disabled')).toBe(true);
    });

    it('places handles on the exact rectangle edges and its velocity midpoint', () => {
        const { view } = setup({ mode: 'mapping', limits: { ...limits, velocityLow: 0, velocityHigh: 49 } });
        const low = view.getByRole('button', { name: 'Low key limit' });
        const high = view.getByRole('button', { name: 'High key limit' });
        expect(low.style.left).toBe('0%');
        expect(high.style.left).toBe('100%');
        expect(low.style.top).toBe('80.46875%');
        expect(view.getByRole('button', { name: 'Low velocity limit' }).style.top).toBe('100%');
        expect(view.getByRole('button', { name: 'High velocity limit' }).style.top).toBe('60.9375%');
    });
    it('keeps single-note, single-velocity handle centers within the rectangle', () => {
        const { view } = setup({ mode: 'mapping', limits: { low: 60, high: 60, velocityLow: 64, velocityHigh: 64 } });
        for (const label of ['Low velocity limit', 'High velocity limit']) {
            const handle = view.getByRole('button', { name: label });
            expect(handle.style.marginLeft).toBe('0px');
            expect(handle.style.left).toBe('47.265625%');
        }
        expect(view.getByRole('button', { name: 'Low velocity limit' }).style.top).toBe('50%');
        expect(view.getByRole('button', { name: 'High velocity limit' }).style.top).toBe('49.21875%');
    });
    it('offers keyboard-accessible root editing only when explicitly enabled in full mode', async () => {
        const onroot = vi.fn();
        const { view } = setup({ mode: 'mapping', rootEditable: true, onroot, rootIdentity: 'one' });
        const key = view.getByRole('button', { name: 'C3' });
        await fireEvent.keyDown(key, { key: 'F10', shiftKey: true });
        expect(view.getByRole('menu', { name: 'Root key' })).toBeTruthy();
        await fireEvent.click(view.getByRole('menuitem', { name: 'Set root to C3' }));
        expect(onroot).toHaveBeenCalledExactlyOnceWith(60);
        expect(view.queryByRole('menu')).toBeNull();
        await fireEvent.keyDown(key, { key: 'ContextMenu' });
        await fireEvent.keyDown(view.getByRole('menu'), { key: 'Escape' });
        expect(document.activeElement).toBe(key);
        await view.rerender({ rootIdentity: 'two', rootEditable: false });
        await fireEvent.keyDown(key, { key: 'ContextMenu' });
        expect(view.queryByRole('menu')).toBeNull();
    });
    it('closes root commands on identity changes and outside clicks', async () => {
        const { view } = setup({ mode: 'mapping', rootEditable: true, rootIdentity: 'one' });
        const key = view.getByRole('button', { name: 'C3' });
        await fireEvent.contextMenu(key, { clientX: 30, clientY: 40 });
        expect(view.queryByRole('menu')).not.toBeNull();
        await view.rerender({ rootIdentity: 'two' });
        expect(view.queryByRole('menu')).toBeNull();
        await fireEvent.contextMenu(key, { clientX: 30, clientY: 40 });
        await fireEvent.pointerDown(document.body);
        expect(view.queryByRole('menu')).toBeNull();
    });
});
