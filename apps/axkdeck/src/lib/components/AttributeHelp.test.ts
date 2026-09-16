import { cleanup, fireEvent, render, act } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';
import AttributeHelp from './AttributeHelp.svelte';

afterEach(() => {
    cleanup();
    vi.useRealTimers();
});

function setup() {
    return render(AttributeHelp, {
        label: 'Record state',
        description: 'Live: active. Inactive: marker clear.',
        contextKey: 'one',
    });
}

describe('Attribute label help', () => {
    it('preserves value lines and the blank line before a general explanation', async () => {
        const description = 'Live: active.\nInactive: marker clear.\n\nDescribes the record lifecycle.';
        const view = render(AttributeHelp, { label: 'Record state', description });
        await fireEvent.focus(view.getByRole('button'));
        expect(view.getByRole('tooltip').textContent?.trim()).toBe(description);
    });

    it('opens on keyboard focus and dismisses Escape without passing it to the workspace', async () => {
        const view = setup();
        const trigger = view.getByRole('button', { name: 'Record state' });
        expect(view.queryByRole('tooltip')).toBeNull();
        await fireEvent.focus(trigger);
        const tooltip = view.getByRole('tooltip');
        expect(trigger.getAttribute('aria-describedby')).toBe(tooltip.id);
        expect(tooltip.parentElement).toBe(document.body);
        const listener = vi.fn();
        window.addEventListener('keydown', listener);
        try {
            await fireEvent.keyDown(trigger, { key: 'Escape' });
            expect(view.queryByRole('tooltip')).toBeNull();
            expect(listener).not.toHaveBeenCalled();
        } finally {
            window.removeEventListener('keydown', listener);
        }
        await fireEvent.focus(trigger);
        await fireEvent.blur(trigger);
        expect(view.queryByRole('tooltip')).toBeNull();
    });

    it('delays hover, allows entering the tooltip, and cancels pending help on leave', async () => {
        vi.useFakeTimers();
        const view = setup();
        const trigger = view.getByRole('button');
        await fireEvent.pointerEnter(trigger);
        await act(() => vi.advanceTimersByTime(399));
        expect(view.queryByRole('tooltip')).toBeNull();
        await act(() => vi.advanceTimersByTime(1));
        const tooltip = view.getByRole('tooltip');
        await fireEvent.pointerLeave(trigger);
        await fireEvent.pointerEnter(tooltip);
        await act(() => vi.advanceTimersByTime(200));
        expect(view.getByRole('tooltip')).toBe(tooltip);
        await fireEvent.pointerLeave(tooltip);
        await act(() => vi.advanceTimersByTime(200));
        expect(view.queryByRole('tooltip')).toBeNull();
        await fireEvent.pointerEnter(trigger);
        await fireEvent.pointerLeave(trigger);
        await act(() => vi.advanceTimersByTime(500));
        expect(view.queryByRole('tooltip')).toBeNull();
    });

    it('pins on click, ignores internal pointers, and closes on repeat click or outside pointer', async () => {
        const view = setup();
        const trigger = view.getByRole('button');
        await fireEvent.focus(trigger);
        await fireEvent.click(trigger);
        await fireEvent.blur(trigger);
        expect(view.getByRole('tooltip')).toBeTruthy();
        await fireEvent.pointerDown(view.getByRole('tooltip'));
        expect(view.getByRole('tooltip')).toBeTruthy();
        await fireEvent.click(trigger);
        expect(view.queryByRole('tooltip')).toBeNull();
        await fireEvent.click(trigger);
        await fireEvent.pointerDown(document.body);
        expect(view.queryByRole('tooltip')).toBeNull();
        await fireEvent.click(trigger);
        await fireEvent.focusIn(document.body);
        expect(view.queryByRole('tooltip')).toBeNull();
    });

    it('cleans up after context changes, scrolling, resizing and unmounting', async () => {
        const view = setup();
        await fireEvent.click(view.getByRole('button'));
        await view.rerender({ label: 'Record state', description: 'Live: active.', contextKey: 'two' });
        expect(view.queryByRole('tooltip')).toBeNull();
        await fireEvent.click(view.getByRole('button'));
        await fireEvent.scroll(window);
        expect(view.queryByRole('tooltip')).toBeNull();
        await fireEvent.click(view.getByRole('button'));
        await fireEvent.resize(window);
        expect(view.queryByRole('tooltip')).toBeNull();
        await fireEvent.click(view.getByRole('button'));
        view.unmount();
        expect(document.querySelector('[role="tooltip"]')).toBeNull();
    });

    it('does not add an empty help control', () => {
        const view = render(AttributeHelp, { label: 'Type', description: '' });
        expect(view.getByText('Type')).toBeTruthy();
        expect(view.queryByRole('button')).toBeNull();
    });
});
