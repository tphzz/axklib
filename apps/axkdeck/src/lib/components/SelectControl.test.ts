import { fireEvent, render, screen } from '@testing-library/svelte';
import { beforeAll, afterAll, describe, expect, it, vi } from 'vitest';
import SelectControl from './SelectControl.svelte';
import { modal } from '../modal';

const options = [
    { value: '', label: 'Select a device' },
    { value: 'a', label: 'Device A' },
    { value: 'blocked', label: 'Unavailable device', disabled: true },
    { value: 'b', label: 'Device B' },
];
const originalScroll = Element.prototype.scrollIntoView;
beforeAll(() => {
    Element.prototype.scrollIntoView = vi.fn();
});
afterAll(() => {
    Element.prototype.scrollIntoView = originalScroll;
});

describe('shared single-choice control', () => {
    it('opens deliberately without moving DOM focus or changing the controlled value', async () => {
        const onchange = vi.fn();
        render(SelectControl, { label: 'Device', value: 'a', options, onchange });
        const trigger = screen.getByRole('combobox', { name: 'Device' });
        await fireEvent.focus(trigger);
        expect(screen.queryByRole('listbox')).toBeNull();
        await fireEvent.click(trigger);
        expect(document.activeElement).toBe(trigger);
        expect(trigger.getAttribute('aria-expanded')).toBe('true');
        expect(screen.getByRole('option', { name: 'Device A' }).getAttribute('aria-selected')).toBe('true');
        await fireEvent.keyDown(trigger, { key: 'ArrowDown' });
        expect(trigger.getAttribute('aria-activedescendant')).toBe(screen.getByRole('option', { name: 'Device B' }).id);
        expect(onchange).not.toHaveBeenCalled();
        await fireEvent.keyDown(trigger, { key: 'Enter' });
        expect(onchange).toHaveBeenCalledExactlyOnceWith('b');
        expect(screen.queryByRole('listbox')).toBeNull();
        expect(trigger.textContent).toContain('Device A');
    });

    it.each(['Escape', 'Tab'])('dismisses with %s without committing a highlighted choice', async (key) => {
        const onchange = vi.fn();
        render(SelectControl, { label: 'Device', value: 'a', options, onchange });
        const trigger = screen.getByRole('combobox');
        await fireEvent.keyDown(trigger, { key: 'ArrowDown' });
        await fireEvent.keyDown(trigger, { key: 'End' });
        await fireEvent.keyDown(trigger, { key });
        expect(onchange).not.toHaveBeenCalled();
        expect(trigger.getAttribute('aria-expanded')).toBe('false');
    });

    it('supports enabled type-ahead, outside cancellation, numeric zero and empty strings', async () => {
        const onchange = vi.fn();
        const view = render(SelectControl, { label: 'Device', value: 'a', options, onchange });
        const trigger = screen.getByRole('combobox');
        await fireEvent.keyDown(trigger, { key: 'd' });
        expect(screen.getByRole('listbox')).toBeTruthy();
        await fireEvent.pointerDown(document.body);
        expect(screen.queryByRole('listbox')).toBeNull();
        expect(onchange).not.toHaveBeenCalled();
        await fireEvent.click(trigger);
        await fireEvent.click(screen.getByRole('option', { name: 'Select a device' }));
        expect(onchange).toHaveBeenLastCalledWith('');
        view.unmount();
        const numeric = vi.fn();
        render(SelectControl, {
            label: 'Partition',
            value: 1,
            options: [
                { value: 0, label: 'First' },
                { value: 1, label: 'Second' },
            ],
            onchange: numeric,
        });
        await fireEvent.click(screen.getByRole('combobox'));
        await fireEvent.click(screen.getByRole('option', { name: 'First' }));
        expect(numeric).toHaveBeenCalledExactlyOnceWith(0);
    });

    it('never chooses implicitly when options refresh, and closes when disabled', async () => {
        const onchange = vi.fn();
        const view = render(SelectControl, { label: 'Device', value: 'a', options, onchange });
        await fireEvent.click(screen.getByRole('combobox'));
        await view.rerender({ options: [options[3]] });
        expect(onchange).not.toHaveBeenCalled();
        await view.rerender({ disabled: true });
        expect(screen.queryByRole('listbox')).toBeNull();
        await fireEvent.click(screen.getByRole('combobox'));
        expect(screen.queryByRole('listbox')).toBeNull();
    });

    it('does not let a deferred opening key override newer navigation', async () => {
        render(SelectControl, { label: 'Device', value: 'a', options, onchange: vi.fn() });
        const trigger = screen.getByRole('combobox');
        for (const key of ['End', 'Home'])
            trigger.dispatchEvent(new KeyboardEvent('keydown', { key, bubbles: true, cancelable: true }));
        await new Promise((resolve) => setTimeout(resolve, 0));
        expect(trigger.getAttribute('aria-activedescendant')).toBe(
            screen.getByRole('option', { name: 'Select a device' }).id,
        );
    });

    it('renders a non-selectable empty state without an active descendant', async () => {
        const onchange = vi.fn();
        render(SelectControl, {
            label: 'Device',
            value: '',
            options: [],
            emptyLabel: 'No devices available',
            onchange,
        });
        const trigger = screen.getByRole('combobox');
        await fireEvent.click(trigger);
        expect(screen.getByRole('status').textContent).toBe('No devices available');
        expect(trigger.hasAttribute('aria-activedescendant')).toBe(false);
        for (const key of ['ArrowDown', 'End', 'Enter']) await fireEvent.keyDown(trigger, { key });
        expect(onchange).not.toHaveBeenCalled();
    });

    it('honors fieldset disabling even when its popup is outside the fieldset', async () => {
        const fieldset = document.createElement('fieldset');
        document.body.append(fieldset);
        const onchange = vi.fn();
        const view = render(SelectControl, {
            target: fieldset,
            props: { label: 'Device', value: 'a', options, onchange },
        });
        try {
            const trigger = screen.getByRole('combobox');
            await fireEvent.click(trigger);
            const option = screen.getByRole('option', { name: 'Device B' });
            fieldset.disabled = true;
            await fireEvent.click(option);
            expect(onchange).not.toHaveBeenCalled();
            expect(screen.queryByRole('listbox')).toBeNull();
        } finally {
            view.unmount();
            fieldset.remove();
        }
    });

    it('cancels before forwarding Tab and never forwards popup navigation to the containing table', async () => {
        const ontriggerkeydown = vi.fn();
        const onchange = vi.fn();
        render(SelectControl, { label: 'Device', value: 'a', options, onchange, ontriggerkeydown });
        const trigger = screen.getByRole('combobox');
        await fireEvent.click(trigger);
        await fireEvent.keyDown(trigger, { key: 'ArrowDown' });
        expect(ontriggerkeydown).not.toHaveBeenCalled();
        await fireEvent.keyDown(trigger, { key: 'Tab', shiftKey: true });
        expect(ontriggerkeydown).toHaveBeenCalledOnce();
        expect(screen.queryByRole('listbox')).toBeNull();
        expect(onchange).not.toHaveBeenCalled();
    });

    it('preserves typed row identity across reordering and accepts duplicate submitted values', async () => {
        const onchange = vi.fn();
        const choices = [
            { key: 'first', value: 'a', label: 'First' },
            { key: 'second', value: 'b', label: 'Second' },
            { key: 'third', value: 'b', label: 'Third', disabled: true },
        ];
        const view = render(SelectControl, { label: 'Target', value: 'a', options: choices, onchange });
        const trigger = screen.getByRole('combobox');
        await fireEvent.click(trigger);
        await fireEvent.keyDown(trigger, { key: 'End' });
        await view.rerender({ options: [choices[1], choices[0], choices[2]] });
        expect(trigger.getAttribute('aria-activedescendant')).toBe(screen.getByRole('option', { name: 'Second' }).id);
        await fireEvent.keyDown(trigger, { key: 'Enter' });
        expect(onchange).toHaveBeenCalledExactlyOnceWith('b');
    });

    it('displays the enabled match when an earlier disabled row shares its value', async () => {
        render(SelectControl, {
            label: 'Target',
            value: '2',
            options: [
                { key: 'disabled', value: '2', label: 'Disabled target', disabled: true },
                { key: 'enabled', value: '2', label: 'Enabled target' },
            ],
            onchange: vi.fn(),
        });
        const trigger = screen.getByRole('combobox');
        expect(trigger.textContent).toContain('Enabled target');
        await fireEvent.click(trigger);
        expect(screen.getByRole('option', { name: 'Enabled target' }).getAttribute('aria-selected')).toBe('true');
        expect(screen.getByRole('option', { name: 'Disabled target' }).getAttribute('aria-selected')).toBe('false');
        expect(trigger.getAttribute('aria-activedescendant')).toBe(
            screen.getByRole('option', { name: 'Enabled target' }).id,
        );
    });

    it('keeps its popup inside the owning modal and consumes Escape before modal dismissal', async () => {
        const dialog = document.createElement('div');
        dialog.setAttribute('role', 'dialog');
        dialog.setAttribute('aria-modal', 'true');
        document.body.append(dialog);
        const onescape = vi.fn();
        const action = modal(dialog, { onescape });
        const view = render(SelectControl, {
            target: dialog,
            props: { label: 'Device', value: 'a', options, onchange: vi.fn() },
        });
        try {
            await fireEvent.click(screen.getByRole('combobox'));
            expect(dialog.contains(screen.getByRole('listbox'))).toBe(true);
            await fireEvent.keyDown(screen.getByRole('combobox'), { key: 'Escape' });
            expect(onescape).not.toHaveBeenCalled();
            await fireEvent.keyDown(screen.getByRole('combobox'), { key: 'Escape' });
            expect(onescape).toHaveBeenCalledOnce();
        } finally {
            view.unmount();
            action.destroy();
            dialog.remove();
        }
    });
});
