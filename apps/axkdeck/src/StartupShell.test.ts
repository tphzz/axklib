import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import StartupShell from './StartupShell.svelte';

describe('StartupShell', () => {
    it('paints a stable starting state while retaining the startup warning', () => {
        render(StartupShell, {
            props: { onacknowledge: vi.fn(), onretry: vi.fn(), onopensettings: vi.fn() },
        });
        expect(screen.getAllByText('Starting services...')).toHaveLength(2);
        expect(screen.getByRole('dialog', { name: 'Experimental software' })).toBeTruthy();
    });

    it('offers recovery without leaving a blank window', async () => {
        const onretry = vi.fn();
        const onopensettings = vi.fn();
        render(StartupShell, {
            props: {
                status: 'unavailable',
                message: 'Connection timed out',
                warningOpen: false,
                onacknowledge: vi.fn(),
                onretry,
                onopensettings,
            },
        });
        expect(screen.getByRole('alert').textContent).toContain('Connection timed out');
        await fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Connection settings' }));
        expect(onretry).toHaveBeenCalledOnce();
        expect(onopensettings).toHaveBeenCalledOnce();
    });
});
