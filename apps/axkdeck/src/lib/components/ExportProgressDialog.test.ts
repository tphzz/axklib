import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import ExportProgressDialog from './ExportProgressDialog.svelte';

describe('ExportProgressDialog', () => {
    it('shows export progress without destination choices', () => {
        render(ExportProgressDialog, {
            props: {
                title: 'Export package',
                progressLabel: 'Building package',
                cancellable: false,
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByRole('dialog', { name: 'Export package progress' })).toBeTruthy();
        expect(screen.getByRole('status').textContent).toContain('Building package');
        expect(screen.queryByText('This computer')).toBeNull();
        expect(screen.queryByText('Server location')).toBeNull();
        expect(screen.queryByRole('button', { name: 'Cancel export' })).toBeNull();
    });

    it('offers cancellation only for cancellable exports', async () => {
        const oncancel = vi.fn();
        render(ExportProgressDialog, {
            props: {
                title: 'Export audio',
                progressLabel: 'Writing WAV files',
                cancellable: true,
                oncancel,
            },
        });

        await fireEvent.click(screen.getByRole('button', { name: 'Cancel export' }));

        expect(oncancel).toHaveBeenCalledOnce();
    });
});
