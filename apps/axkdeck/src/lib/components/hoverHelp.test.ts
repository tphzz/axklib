import { act, fireEvent, within } from '@testing-library/svelte';
import { flushSync } from 'svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { hoverHelp } from './hoverHelp.svelte';

const cleanups: (() => void)[] = [];
const text = 'Drag to change cutoff. Alt-drag or mouse wheel changes Q / Width. Shift gives finer control.';

function setup() {
    const button = document.createElement('button');
    button.textContent = 'Cutoff';
    document.body.appendChild(button);
    const help = hoverHelp(button, text);
    let disposed = false;
    const dispose = () => {
        if (disposed) return;
        disposed = true;
        help?.destroy?.();
        button.remove();
    };
    cleanups.push(dispose);
    flushSync();
    return { button, dispose, view: within(document.body) };
}

afterEach(async () => {
    for (const dispose of cleanups.splice(0)) dispose();
    await act(() => Promise.resolve());
    vi.useRealTimers();
});

describe('Shared graph control help', () => {
    it('starts closed and opens hover help only after 400 milliseconds', async () => {
        vi.useFakeTimers();
        const { button, view } = setup();
        expect(view.queryByRole('tooltip')).toBeNull();
        await fireEvent.pointerEnter(button);
        await act(() => vi.advanceTimersByTime(399));
        expect(view.queryByRole('tooltip')).toBeNull();
        await act(() => vi.advanceTimersByTime(1));
        expect(view.getByRole('tooltip').textContent?.trim()).toBe(text);
        expect(view.getByRole('tooltip').parentElement).toBe(document.body);
        expect(button.getAttribute('aria-describedby')).toBe(view.getByRole('tooltip').id);
    });

    it('opens on keyboard focus and closes on Escape', async () => {
        const { button, view } = setup();
        await fireEvent.focus(button);
        expect(view.getByRole('tooltip').textContent?.trim()).toBe(text);
        await fireEvent.keyDown(button, { key: 'Escape' });
        expect(view.queryByRole('tooltip')).toBeNull();
    });

    it('hides help immediately when a pointer gesture starts', async () => {
        const { button, view } = setup();
        await fireEvent.focus(button);
        expect(view.getByRole('tooltip')).toBeTruthy();
        await fireEvent.pointerDown(button);
        expect(view.queryByRole('tooltip')).toBeNull();
    });

    it('cancels pending hover help when a pointer gesture starts', async () => {
        vi.useFakeTimers();
        const { button, view } = setup();
        await fireEvent.pointerEnter(button);
        await act(() => vi.advanceTimersByTime(300));
        await fireEvent.pointerDown(button);
        await act(() => vi.advanceTimersByTime(500));
        expect(view.queryByRole('tooltip')).toBeNull();
    });

    it.each([false, true])('removes open or pending help on destroy (open: %s)', async (open) => {
        vi.useFakeTimers();
        const { button, dispose, view } = setup();
        if (open) await fireEvent.focus(button);
        else await fireEvent.pointerEnter(button);
        await act(dispose);
        await act(() => vi.advanceTimersByTime(500));
        expect(view.queryByRole('tooltip')).toBeNull();
    });
});
