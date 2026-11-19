import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it } from 'vitest';
import Harness from '../../test/WaveDiskHarness.svelte';

describe('Wave disk dialog', () => {
    it('keeps an invalid download name editable, associated with its full message, and recovers without a new selection', async () => {
        render(Harness);
        const submit = await screen.findByRole<HTMLButtonElement>('button', { name: 'Create disk image' });
        await waitFor(() => expect(submit.disabled).toBe(false));
        await fireEvent.click(screen.getByRole('combobox', { name: 'Destination' }));
        await fireEvent.click(await screen.findByRole('option', { name: 'Download' }));
        const filename = screen.getByRole<HTMLInputElement>('textbox', { name: 'File' });
        await fireEvent.input(filename, { target: { value: 'folder/name.img' } });
        expect(submit.disabled).toBe(true);
        expect(filename.getAttribute('aria-invalid')).toBe('true');
        expect(document.getElementById(filename.getAttribute('aria-describedby')!)?.textContent).toContain(
            'without a directory',
        );
        await fireEvent.input(filename, { target: { value: 'Studio.img' } });
        await waitFor(() => expect(submit.disabled).toBe(false));
        await fireEvent.click(submit);
        expect(await screen.findByText('Studio.img')).toBeTruthy();
        await waitFor(() => expect(screen.queryByRole('dialog', { name: 'Convert' })).toBeNull());
    });
    it('reviews checked Waves as ordered .W slots and blocks Strict losses without an acceptance gate', async () => {
        render(Harness);
        const submit = await screen.findByRole<HTMLButtonElement>('button', { name: 'Create disk image' });
        await waitFor(() => expect(submit.disabled).toBe(false));
        await fireEvent.click(screen.getByRole('checkbox', { name: /Piano High/ }));
        await fireEvent.click(screen.getByRole('tab', { name: 'Planned output' }));
        expect(await screen.findByText('PIANO_HI.W02')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: 'Strict' }));
        await waitFor(() => expect(submit.disabled).toBe(true));
        expect(await screen.findByText(/Strict fidelity cannot preserve/)).toBeTruthy();
        expect(screen.queryByRole('checkbox', { name: /accept.*loss/i })).toBeNull();
    });
});
