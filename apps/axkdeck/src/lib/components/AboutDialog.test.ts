import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import AboutDialog from './AboutDialog.svelte';

describe('AboutDialog', () => {
    it('appears immediately while authoritative build information is loading', async () => {
        render(AboutDialog, {
            props: {
                state: { status: 'loading' },
                onclose: vi.fn(),
            },
        });

        const dialog = screen.getByRole('dialog', { name: 'About axkdeck' });
        expect(within(dialog).getByText('Loading version information…')).toBeTruthy();
        await waitFor(() => expect(document.activeElement).toBe(within(dialog).getByRole('button', { name: 'Close' })));
    });

    it('renders version and source identity and closes with Escape', async () => {
        const onclose = vi.fn();
        render(AboutDialog, {
            props: {
                state: {
                    status: 'ready',
                    buildInfo: {
                        schemaVersion: 1,
                        semanticVersion: '0.4.0',
                        projectVersion: '0.4.0',
                        sourceIdentity: 'v0.4.0-1234567-mod',
                        releaseTag: '',
                        isRelease: false,
                    },
                },
                onclose,
            },
        });

        const dialog = screen.getByRole('dialog', { name: 'About axkdeck' });
        expect(within(dialog).getByText('0.4.0')).toBeTruthy();
        expect(within(dialog).getByText('v0.4.0-1234567-mod')).toBeTruthy();

        await fireEvent.keyDown(dialog, { key: 'Escape' });
        expect(onclose).toHaveBeenCalledOnce();
    });

    it('shows a stable fallback when build information cannot be loaded', () => {
        render(AboutDialog, {
            props: {
                state: { status: 'error' },
                onclose: vi.fn(),
            },
        });

        expect(screen.getByText('Version information is unavailable.')).toBeTruthy();
    });
});
