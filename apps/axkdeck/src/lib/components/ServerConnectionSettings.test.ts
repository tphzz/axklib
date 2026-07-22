import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import ServerConnectionSettings from './ServerConnectionSettings.svelte';

describe('ServerConnectionSettings', () => {
    it('never renders a stored bearer token and submits an explicit replacement', async () => {
        const onsave = vi.fn().mockResolvedValue(undefined);
        render(ServerConnectionSettings, {
            props: {
                settings: {
                    mode: 'remote',
                    baseUrl: 'https://sampler.example.test/api/v1',
                    tokenConfigured: true,
                    secureStorageError: null,
                },
                onsave,
                onuselocal: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        const token = screen.getByLabelText('Access token') as HTMLInputElement;
        expect(token.value).toBe('');
        expect(token.type).toBe('password');

        await fireEvent.input(token, {
            target: { value: '0123456789abcdef0123456789abcdef' },
        });
        await fireEvent.click(screen.getByRole('button', { name: 'Connect' }));

        expect(onsave).toHaveBeenCalledWith({
            baseUrl: 'https://sampler.example.test/api/v1',
            bearerToken: '0123456789abcdef0123456789abcdef',
        });
    });

    it('can return to the packaged local server without asking for a token', async () => {
        const onuselocal = vi.fn().mockResolvedValue(undefined);
        render(ServerConnectionSettings, {
            props: {
                settings: {
                    mode: 'remote',
                    baseUrl: 'https://sampler.example.test/api/v1',
                    tokenConfigured: true,
                    secureStorageError: null,
                },
                onsave: vi.fn(),
                onuselocal,
                oncancel: vi.fn(),
            },
        });

        await fireEvent.click(screen.getByRole('button', { name: 'Use local server' }));
        expect(onuselocal).toHaveBeenCalledOnce();
    });

    it('can restart a failed packaged server while local mode is selected', async () => {
        const onuselocal = vi.fn().mockResolvedValue(undefined);
        render(ServerConnectionSettings, {
            props: {
                settings: {
                    mode: 'local',
                    baseUrl: null,
                    tokenConfigured: false,
                    secureStorageError: 'axklib-server process 42 exited unexpectedly',
                },
                onsave: vi.fn(),
                onuselocal,
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByRole('alert').textContent).toContain('exited unexpectedly');
        await fireEvent.click(screen.getByRole('button', { name: 'Restart local server' }));
        expect(onuselocal).toHaveBeenCalledOnce();
    });

    it('keeps the local-server command separate from the aligned dialog actions', () => {
        render(ServerConnectionSettings, {
            props: {
                settings: {
                    mode: 'local',
                    baseUrl: null,
                    tokenConfigured: false,
                    secureStorageError: null,
                },
                onsave: vi.fn(),
                onuselocal: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        const restart = screen.getByRole('button', { name: 'Restart local server' });
        const cancel = screen.getByRole('button', { name: 'Cancel' });
        const connect = screen.getByRole('button', { name: 'Connect' });

        expect(cancel.parentElement).toBe(connect.parentElement);
        expect(restart.parentElement).not.toBe(connect.parentElement);
    });
});
