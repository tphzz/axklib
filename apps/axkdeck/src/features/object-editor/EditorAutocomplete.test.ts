import { fireEvent, render } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import EditorAutocomplete from './EditorAutocomplete.svelte';

const options = [
    { value: 1, label: 'CC 001' },
    { value: 74, label: 'CC 074' },
    { value: 121, label: 'Aftertouch' },
];
describe('editor autocomplete', () => {
    it('clears only the query, filters numeric CCs, selects explicitly, and restores unfinished searches', async () => {
        const onchange = vi.fn();
        const view = render(EditorAutocomplete, { label: 'Controller', value: 1, options, onchange });
        const input = view.getByRole('combobox') as HTMLInputElement;
        await fireEvent.focus(input);
        expect(view.queryByRole('listbox')).toBeNull();
        await fireEvent.click(view.getByRole('button', { name: 'Clear Controller' }));
        expect(input.value).toBe('');
        expect(view.getAllByRole('option')).toHaveLength(3);
        expect(onchange).not.toHaveBeenCalled();
        await fireEvent.input(input, { target: { value: '74' } });
        expect(view.getAllByRole('option')).toHaveLength(1);
        await fireEvent.keyDown(input, { key: 'Enter' });
        expect(onchange).toHaveBeenCalledWith(74);
        await fireEvent.input(input, { target: { value: 'unknown' } });
        expect(view.queryAllByRole('option')).toHaveLength(0);
        await fireEvent.keyDown(input, { key: 'Escape' });
        expect(input.value).toBe('CC 001');
        expect(view.queryByRole('listbox')).toBeNull();
    });
    it('keeps internal pointers open, dismisses outside, and respects disabled state', async () => {
        const onchange = vi.fn();
        const view = render(EditorAutocomplete, { label: 'Controller', value: 1, options, onchange });
        const input = view.getByRole('combobox') as HTMLInputElement;
        await fireEvent.keyDown(input, { key: 'End' });
        await fireEvent.pointerDown(view.getByRole('listbox'));
        expect(view.queryByRole('listbox')).not.toBeNull();
        await fireEvent.keyDown(input, { key: 'Enter' });
        expect(onchange).toHaveBeenCalledWith(121);
        await fireEvent.click(input);
        await fireEvent.pointerDown(document.body);
        expect(view.queryByRole('listbox')).toBeNull();
        await view.rerender({ disabled: true });
        expect(input.disabled).toBe(true);
        expect((view.getByRole('button', { name: 'Clear Controller' }) as HTMLButtonElement).disabled).toBe(true);
    });
});
