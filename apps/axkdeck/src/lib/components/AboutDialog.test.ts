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

    it('renders the source identity as the version for a branch build and closes with Escape', async () => {
        const onclose = vi.fn();
        render(AboutDialog, {
            props: {
                state: {
                    status: 'ready',
                    buildInfo: {
                        schemaVersion: 1,
                        semanticVersion: '0.4.0-pre',
                        projectVersion: '0.4.0',
                        sourceIdentity: '0.4.0-pre-dirty-1234567',
                        releaseTag: '',
                        isRelease: false,
                        webviewEngine: 'Microsoft Edge WebView2',
                        webviewVersion: '120.0.2210.144',
                    },
                },
                onclose,
            },
        });

        const dialog = screen.getByRole('dialog', { name: 'About axkdeck' });
        expect(within(dialog).getByText('0.4.0-pre-dirty-1234567')).toBeTruthy();
        expect(within(dialog).queryByText('0.4.0-pre')).toBeNull();
        expect(within(dialog).queryByText('Build')).toBeNull();
        expect(within(dialog).getByText('Microsoft Edge WebView2 120.0.2210.144')).toBeTruthy();

        await fireEvent.keyDown(dialog, { key: 'Escape' });
        expect(onclose).toHaveBeenCalledOnce();
    });

    it('renders the canonical semantic version for a tagged release', () => {
        render(AboutDialog, {
            props: {
                state: {
                    status: 'ready',
                    buildInfo: {
                        schemaVersion: 1,
                        semanticVersion: '0.4.0',
                        projectVersion: '0.4.0',
                        sourceIdentity: 'v0.4.0-1234567',
                        releaseTag: 'v0.4.0',
                        isRelease: true,
                        webviewEngine: 'WebKitGTK',
                        webviewVersion: '2.50.4',
                    },
                },
                onclose: vi.fn(),
            },
        });

        const dialog = screen.getByRole('dialog', { name: 'About axkdeck' });
        expect(within(dialog).getByText('0.4.0')).toBeTruthy();
        expect(within(dialog).queryByText('v0.4.0-1234567')).toBeNull();
    });

    it('keeps build information available when webview introspection fails', () => {
        render(AboutDialog, {
            props: {
                state: {
                    status: 'ready',
                    buildInfo: {
                        schemaVersion: 1,
                        semanticVersion: '0.4.0',
                        projectVersion: '0.4.0',
                        sourceIdentity: 'main-dirty-1234567',
                        releaseTag: '',
                        isRelease: false,
                        webviewEngine: 'WebKitGTK',
                        webviewVersion: null,
                    },
                },
                onclose: vi.fn(),
            },
        });

        const dialog = screen.getByRole('dialog', { name: 'About axkdeck' });
        expect(within(dialog).getByText('WebKitGTK unavailable')).toBeTruthy();
        expect(within(dialog).getByText('main-dirty-1234567')).toBeTruthy();
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
