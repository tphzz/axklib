import { cleanup, fireEvent, render } from '@testing-library/svelte';
import { afterEach, describe, expect, it, vi } from 'vitest';
import FilesystemNameField from './FilesystemNameField.svelte';
import { writableFilesRoot } from '../../lib/testing/filesystem';

afterEach(cleanup);
const capabilities = {
    ...writableFilesRoot,
    namePolicy: 'FAT_8_3_UPPERCASE' as const,
    maximumNameBytes: 12,
    namePattern: '^[A-Z0-9]{1,8}(\\.[A-Z0-9]{1,3})?$',
};
describe('filesystem name field', () => {
    it('keeps untouched blank fields neutral, then explains rejection accessibly', async () => {
        const view = render(FilesystemNameField, { value: '', label: 'Name', capabilities, onchange: vi.fn() });
        const input = view.getByRole('textbox');
        expect(view.queryByRole('button')).toBeNull();
        await fireEvent.blur(input);
        expect(input.getAttribute('aria-invalid')).toBe('true');
        expect(document.getElementById(input.getAttribute('aria-describedby')!)?.textContent).toBe('Enter a name.');
        await fireEvent.focus(view.getByRole('button', { name: 'Name: Rejected' }));
        expect(view.getByRole('tooltip').textContent).toContain('Enter a name');
        await fireEvent.keyDown(input, { key: 'Escape' });
        expect(view.queryByRole('tooltip')).toBeNull();
    });
    it('uppercases typing and paste without moving the caret or changing Unicode', async () => {
        const onchange = vi.fn();
        const view = render(FilesystemNameField, { value: '', label: 'Name', capabilities, onchange });
        const input = view.getByRole('textbox') as HTMLInputElement;
        input.value = 'aßb.wav';
        input.setSelectionRange(2, 2);
        await fireEvent.input(input);
        expect(onchange).toHaveBeenCalledWith('AßB.WAV');
        expect(input.value).toBe('AßB.WAV');
        expect(input.selectionStart).toBe(2);
    });
    it('shows per-row backend conflicts and clears stale help after a name change or submission', async () => {
        const props = { value: 'FILE.BIN', label: 'Filename 1', capabilities, onchange: vi.fn(), immediate: true };
        const view = render(FilesystemNameField, props);
        expect(view.getByRole('button', { name: 'Filename 1: Valid' })).toBeTruthy();
        await view.rerender({ ...props, error: 'A directory already uses this name.' });
        await fireEvent.click(view.getByRole('button', { name: 'Filename 1: Rejected' }));
        expect(view.getByRole('tooltip').textContent).toContain('A directory already');
        await view.rerender({ ...props, value: 'NEW.BIN', error: null });
        expect(view.queryByRole('tooltip')).toBeNull();
        await fireEvent.click(view.getByRole('button', { name: 'Filename 1: Valid' }));
        await fireEvent.pointerDown(document.body);
        expect(view.queryByRole('tooltip')).toBeNull();
        await view.rerender({ ...props, disabled: true });
        expect(view.queryByRole('button')).toBeNull();
        expect(view.getByRole('textbox').getAttribute('aria-invalid')).toBe('false');
    });
});
