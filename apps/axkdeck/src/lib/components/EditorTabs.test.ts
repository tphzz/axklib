import { act, fireEvent, render } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import EditorTabs from './EditorTabs.svelte';

const tabs = [
    { id: 'first', label: 'First' },
    { id: 'middle', label: 'Middle' },
    { id: 'last', label: 'Last' },
];

describe('EditorTabs', () => {
    it('reveals programmatic selection and resizing within the tab strip without stealing focus', async () => {
        let resize!: () => void;
        const disconnect = vi.fn();
        vi.stubGlobal(
            'ResizeObserver',
            class {
                constructor(callback: () => void) {
                    resize = callback;
                }
                observe() {}
                disconnect = disconnect;
            },
        );
        const view = render(EditorTabs, { tabs, active: 'first', label: 'Sections', onselect: vi.fn() });
        const strip = view.getByRole('tablist');
        Object.defineProperty(strip, 'clientWidth', { configurable: true, value: 100 });
        const buttons = view.getAllByRole('tab');
        buttons.forEach((button, index) => {
            Object.defineProperty(button, 'offsetLeft', { value: index * 80 });
            Object.defineProperty(button, 'offsetWidth', { value: 80 });
        });
        buttons[0]!.focus();
        await view.rerender({ active: 'last' });
        await act(() => {});
        expect(strip.scrollLeft).toBe(140);
        expect(document.activeElement).toBe(buttons[0]);
        Object.defineProperty(strip, 'clientWidth', { value: 80 });
        resize();
        expect(strip.scrollLeft).toBe(160);
        await view.rerender({ active: 'first' });
        await act(() => {});
        expect(strip.scrollLeft).toBe(0);
        view.unmount();
        expect(disconnect).toHaveBeenCalledOnce();
    });

    it.each([false, true])(
        'preserves primary/subpage semantics and ignores modified arrows (secondary=%s)',
        async (secondary) => {
            const onselect = vi.fn();
            const view = render(EditorTabs, { tabs, active: 'first', label: 'Sections', onselect, secondary });
            const buttons = view.getAllByRole(secondary ? 'button' : 'tab');
            expect(buttons[0]!.getAttribute(secondary ? 'aria-pressed' : 'aria-selected')).toBe('true');
            expect(buttons[1]!.tabIndex).toBe(secondary ? 0 : -1);
            buttons[0]!.focus();
            await fireEvent.keyDown(buttons[0]!, { key: 'ArrowRight', altKey: true });
            expect(onselect).not.toHaveBeenCalled();
            await fireEvent.keyDown(buttons[0]!, { key: 'End' });
            expect(onselect).toHaveBeenLastCalledWith('last');
            expect(document.activeElement).toBe(buttons[2]);
            await fireEvent.keyDown(buttons[2]!, { key: 'ArrowRight' });
            expect(onselect).toHaveBeenLastCalledWith('first');
            expect(document.activeElement).toBe(buttons[0]);
        },
    );
});
