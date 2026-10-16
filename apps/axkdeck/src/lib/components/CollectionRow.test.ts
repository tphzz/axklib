import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import CollectionRowHarness from '../../test/CollectionRowHarness.svelte';

describe('compact collection row', () => {
    it('owns the identity column and adds a slot only when supplied', async () => {
        const view = render(CollectionRowHarness);
        const row = screen.getByRole('button', { name: /12string.*146 B/ });
        expect(row.children).toHaveLength(1);
        expect(row.firstElementChild?.contains(screen.getByText('12string'))).toBe(true);
        expect(row.firstElementChild?.contains(screen.getByText('146 B'))).toBe(true);
        await view.rerender({ slotLabel: '001' });
        expect(row.children).toHaveLength(2);
        expect(row.firstElementChild?.textContent).toBe('001');
        expect(row.lastElementChild?.contains(screen.getByText('12string'))).toBe(true);
        await view.rerender({ slotLabel: undefined });
        expect(row.children).toHaveLength(1);
        expect(screen.queryByText('001')).toBeNull();
    });

    it('retains whole-row events and distinct active and selected states', async () => {
        const onclick = vi.fn();
        const onkeydown = vi.fn();
        const oncontextmenu = vi.fn();
        const view = render(CollectionRowHarness, { active: true, selected: false, onclick, onkeydown, oncontextmenu });
        const row = screen.getByRole('button');
        expect(row.getAttribute('aria-pressed')).toBe('false');
        expect(row.classList.contains('active')).toBe(true);
        expect(row.classList.contains('selected')).toBe(false);
        await fireEvent.click(screen.getByText('146 B'), { shiftKey: true });
        expect(onclick.mock.calls[0]![0].shiftKey).toBe(true);
        expect(onclick.mock.instances[0]).toBe(row);
        await fireEvent.keyDown(row, { key: 'ArrowDown' });
        expect(onkeydown.mock.calls[0]![0].key).toBe('ArrowDown');
        await fireEvent.contextMenu(row);
        expect(oncontextmenu).toHaveBeenCalledOnce();
        await view.rerender({ active: false, selected: true, onclick, onkeydown, oncontextmenu });
        expect(row.getAttribute('aria-pressed')).toBe('true');
        expect(row.classList.contains('active')).toBe(false);
        expect(row.classList.contains('selected')).toBe(true);
    });
});
