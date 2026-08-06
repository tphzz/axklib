import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import CreateSampleBankDialog from './CreateSampleBankDialog.svelte';

describe('CreateSampleBankDialog', () => {
    it('focuses the name, rejects case-insensitive duplicates, and submits a valid name', async () => {
        const onsubmit = vi.fn();
        render(CreateSampleBankDialog, {
            props: {
                volumeName: 'Keys',
                sampleCount: 3,
                existingNames: ['Piano Bank'],
                busy: false,
                error: '',
                oncancel: vi.fn(),
                onsubmit,
            },
        });

        const name = screen.getByRole('textbox', { name: 'Sample Bank name' });
        await waitFor(() => expect(document.activeElement).toBe(name));
        await fireEvent.input(name, { target: { value: 'piano bank' } });
        expect(screen.getByText('Sample Bank already exists: piano bank')).toBeTruthy();
        expect((screen.getByRole('button', { name: 'Create Sample Bank' }) as HTMLButtonElement).disabled).toBe(true);

        await fireEvent.input(name, { target: { value: 'Layered Keys' } });
        await fireEvent.click(screen.getByRole('button', { name: 'Create Sample Bank' }));
        expect(onsubmit).toHaveBeenCalledWith('Layered Keys');
    });
});
