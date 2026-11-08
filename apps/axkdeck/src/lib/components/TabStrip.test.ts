import { act, fireEvent, render, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import TabStrip from './TabStrip.svelte';

const tabs = [
    { id: 'first', label: 'First' },
    { id: 'middle', label: 'Middle', disabled: true, reason: 'Unavailable' },
    { id: 'last', label: 'Last', icon: 'music' as const },
];

function deferred() {
    let resolve!: () => void;
    const promise = new Promise<void>((done) => (resolve = done));
    return { promise, resolve };
}

describe('TabStrip', () => {
    it('opts into equal-width distribution without changing the default or selection semantics', async () => {
        const view = render(TabStrip, { tabs, active: 'first', label: 'Sections', onselect: vi.fn() });
        expect(view.getByRole('tablist').classList.contains('fill')).toBe(false);
        await view.rerender({ fill: true });
        expect(view.getByRole('tablist').classList.contains('fill')).toBe(true);
        expect(view.getByRole('tab', { name: 'First' }).getAttribute('aria-selected')).toBe('true');
    });
    it.each(['tablist', 'navigation'] as const)(
        'preserves %s semantics and skips disabled choices',
        async (semantics) => {
            const onselect = vi.fn((active: string) => view.rerender({ active }));
            const view = render(TabStrip, { tabs, active: 'first', label: 'Sections', onselect, semantics });
            const buttons = view.getAllByRole(semantics === 'tablist' ? 'tab' : 'button');
            const selected = semantics === 'tablist' ? 'aria-selected' : 'aria-pressed';
            expect(buttons[0]!.getAttribute(selected)).toBe('true');
            expect(buttons[2]!.tabIndex).toBe(semantics === 'tablist' ? -1 : 0);
            expect(buttons[1]!.matches(':disabled')).toBe(true);
            expect(buttons[1]!.getAttribute('title')).toBe('Unavailable');
            buttons[0]!.focus();
            await fireEvent.keyDown(buttons[0]!, { key: 'ArrowRight', altKey: true });
            await fireEvent.keyDown(buttons[0]!, { key: 'End', ctrlKey: true });
            expect(onselect).not.toHaveBeenCalled();
            for (const [index, key, next] of [
                [0, 'End', 2],
                [2, 'ArrowRight', 0],
                [0, 'ArrowLeft', 2],
                [2, 'Home', 0],
            ] as const) {
                await fireEvent.keyDown(buttons[index]!, { key });
                await act(() => {});
                expect(document.activeElement).toBe(buttons[next]);
                expect(buttons[next]!.getAttribute(selected)).toBe('true');
            }
            expect(
                view.getByRole(semantics === 'tablist' ? 'tablist' : 'navigation', { name: 'Sections' }),
            ).toBeTruthy();
            expect(view.getByRole(semantics === 'tablist' ? 'tab' : 'button', { name: 'Last' })).toBe(buttons[2]);
        },
    );

    it('links tabs to their panels and leaves selection controlled by the owner', async () => {
        const onselect = vi.fn();
        const view = render(TabStrip, {
            tabs,
            active: 'first',
            label: 'Sections',
            idPrefix: 'editor',
            panelId: 'panel',
            onselect,
        });
        const first = view.getByRole('tab', { name: 'First' });
        const last = view.getByRole('tab', { name: 'Last' });
        expect(first.getAttribute('id')).toBe('editor-first');
        expect(first.getAttribute('aria-controls')).toBe('panel');
        first.focus();
        await fireEvent.keyDown(first, { key: 'End' });
        await act(() => {});
        expect(onselect).toHaveBeenCalledWith('last');
        expect(first.getAttribute('aria-selected')).toBe('true');
        expect(last.getAttribute('aria-selected')).toBe('false');
        expect(document.activeElement).toBe(first);
        last.focus();
        expect(onselect).toHaveBeenCalledOnce();
    });

    it('does not select through clicks or keyboard in a disabled fieldset', async () => {
        const onselect = vi.fn();
        const view = render(TabStrip, { tabs, active: 'first', label: 'Sections', onselect });
        const fieldset = document.createElement('fieldset');
        fieldset.disabled = true;
        document.body.append(fieldset);
        fieldset.append(view.container);
        try {
            const first = view.getByRole('tab', { name: 'First' });
            await fireEvent.click(first);
            await fireEvent.keyDown(first, { key: 'End' });
            expect(onselect).not.toHaveBeenCalled();
        } finally {
            view.unmount();
            fieldset.remove();
        }
    });

    it('offers an enabled tab stop when the active tab is disabled', () => {
        const view = render(TabStrip, { tabs, active: 'middle', label: 'Sections', onselect: vi.fn() });
        expect(view.getByRole('tab', { name: 'First' }).tabIndex).toBe(0);
        expect(view.getByRole('tab', { name: 'Middle' }).matches(':disabled')).toBe(true);
    });

    it('keeps the startup strip entirely disabled with no selection', () => {
        const view = render(TabStrip, {
            tabs,
            label: 'Sections',
            disabled: true,
            appearance: 'workspace',
            semantics: 'navigation',
            onselect: vi.fn(),
        });
        for (const button of view.getAllByRole('button')) {
            expect(button.matches(':disabled')).toBe(true);
            expect(button.getAttribute('aria-pressed')).toBe('false');
        }
    });

    it('does not move focus for refused asynchronous selection', async () => {
        const pending = deferred();
        const view = render(TabStrip, { tabs, active: 'first', label: 'Sections', onselect: () => pending.promise });
        const first = view.getByRole('tab', { name: 'First' });
        first.focus();
        await fireEvent.keyDown(first, { key: 'End' });
        pending.resolve();
        await act(() => pending.promise);
        expect(document.activeElement).toBe(first);
        expect(first.getAttribute('aria-selected')).toBe('true');
    });

    it('does not steal focus after the user leaves an asynchronous transition', async () => {
        const pending = deferred();
        const view = render(TabStrip, {
            tabs,
            active: 'first',
            label: 'Sections',
            onselect: async (active) => {
                await pending.promise;
                await view.rerender({ active });
            },
        });
        const first = view.getByRole('tab', { name: 'First' });
        first.focus();
        await fireEvent.keyDown(first, { key: 'End' });
        const outside = document.createElement('button');
        document.body.append(outside);
        outside.focus();
        pending.resolve();
        await act(() => pending.promise);
        expect(view.getByRole('tab', { name: 'Last' }).getAttribute('aria-selected')).toBe('true');
        expect(document.activeElement).toBe(outside);
        outside.remove();
    });

    it('restores accepted keyboard focus after temporary busy disabling blurs the origin', async () => {
        const pending = deferred();
        const view = render(TabStrip, {
            tabs,
            active: 'first',
            label: 'Sections',
            onselect: async (active) => {
                await view.rerender({ disabled: true });
                await pending.promise;
                await view.rerender({ active, disabled: false });
            },
        });
        const first = view.getByRole('tab', { name: 'First' });
        first.focus();
        await fireEvent.keyDown(first, { key: 'End' });
        expect(first.matches(':disabled')).toBe(true);
        first.blur();
        pending.resolve();
        await act(() => pending.promise);
        await waitFor(() => expect(document.activeElement).toBe(view.getByRole('tab', { name: 'Last' })));
    });

    it('does not restore disabled-origin focus after the user clicks elsewhere', async () => {
        const pending = deferred();
        const view = render(TabStrip, {
            tabs,
            active: 'first',
            label: 'Sections',
            onselect: async (active) => {
                await view.rerender({ disabled: true });
                await pending.promise;
                await view.rerender({ active, disabled: false });
            },
        });
        const first = view.getByRole('tab', { name: 'First' });
        first.focus();
        await fireEvent.keyDown(first, { key: 'End' });
        first.blur();
        await fireEvent.pointerDown(document.body);
        pending.resolve();
        await act(() => pending.promise);
        expect(document.activeElement).toBe(document.body);
    });

    it('ignores obsolete keyboard continuations after a newer request', async () => {
        const pending = deferred();
        let count = 0;
        const onselect = vi.fn(async (active: string) => {
            if (++count === 1) await pending.promise;
            await view.rerender({ active });
        });
        const view = render(TabStrip, { tabs, active: 'first', label: 'Sections', onselect });
        const first = view.getByRole('tab', { name: 'First' });
        first.focus();
        await fireEvent.keyDown(first, { key: 'End' });
        await fireEvent.keyDown(first, { key: 'Home' });
        pending.resolve();
        await act(() => pending.promise);
        expect(document.activeElement).toBe(first);
    });

    it('advances from committed selection while earlier enumeration is still pending', async () => {
        const pending = deferred();
        const onselect = vi.fn(async (active: string) => {
            await view.rerender({ active });
            if (active === 'middle') await pending.promise;
        });
        const view = render(TabStrip, {
            tabs: tabs.map((tab) => ({ ...tab, disabled: false })),
            active: 'first',
            label: 'Sections',
            onselect,
        });
        const first = view.getByRole('tab', { name: 'First' });
        first.focus();
        await fireEvent.keyDown(first, { key: 'ArrowRight' });
        expect(view.getByRole('tab', { name: 'Middle' }).getAttribute('aria-selected')).toBe('true');
        await fireEvent.keyDown(first, { key: 'ArrowRight' });
        await waitFor(() => expect(document.activeElement).toBe(view.getByRole('tab', { name: 'Last' })));
        expect(onselect.mock.calls.map(([id]) => id)).toEqual(['middle', 'last']);
        pending.resolve();
        await act(() => pending.promise);
        expect(document.activeElement).toBe(view.getByRole('tab', { name: 'Last' }));
    });

    it('does not focus detached buttons when unmounted during a transition', async () => {
        const pending = deferred();
        const view = render(TabStrip, { tabs, active: 'first', label: 'Sections', onselect: () => pending.promise });
        const first = view.getByRole('tab', { name: 'First' });
        const last = view.getByRole('tab', { name: 'Last' });
        const focus = vi.spyOn(last, 'focus');
        first.focus();
        await fireEvent.keyDown(first, { key: 'End' });
        view.unmount();
        pending.resolve();
        await act(() => pending.promise);
        expect(focus).not.toHaveBeenCalled();
    });

    it('reveals programmatic selection and resize without stealing focus', async () => {
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
        const view = render(TabStrip, { tabs, active: 'first', label: 'Sections', onselect: vi.fn() });
        const strip = view.getByRole('tablist');
        Object.defineProperty(strip, 'clientWidth', { configurable: true, value: 100 });
        vi.spyOn(strip, 'getBoundingClientRect').mockImplementation(
            () => ({ left: 0, right: strip.clientWidth, width: strip.clientWidth }) as DOMRect,
        );
        const buttons = view.getAllByRole('tab');
        buttons.forEach((button, index) => {
            vi.spyOn(button, 'getBoundingClientRect').mockImplementation(
                () =>
                    ({
                        left: index * 80 - strip.scrollLeft,
                        right: (index + 1) * 80 - strip.scrollLeft,
                        width: 80,
                    }) as DOMRect,
            );
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

    it('reveals fractional tab edges at 125 percent scale', async () => {
        const view = render(TabStrip, { tabs, active: 'first', label: 'Sections', onselect: vi.fn() });
        await act(() => {});
        const strip = view.getByRole('tablist');
        strip.scrollLeft = 76.8;
        Object.defineProperty(strip, 'clientWidth', { value: 122 });
        vi.spyOn(strip, 'getBoundingClientRect').mockReturnValue({ left: 20, right: 172.5, width: 152.5 } as DOMRect);
        const button = view.getAllByRole('tab')[2]!;
        vi.spyOn(button, 'getBoundingClientRect').mockReturnValue({
            left: 110.78125,
            right: 173.796875,
            width: 63.015625,
        } as DOMRect);
        await fireEvent.focus(button);
        expect(strip.scrollLeft).toBe(78.8);
    });
});
