import { afterEach, describe, expect, it } from 'vitest';

import { createTransport } from './createTransport';

const tauriWindow = window as Window & { __TAURI_INTERNALS__?: unknown };

afterEach(() => {
    delete window.__AXKLIB_SERVER__;
    delete tauriWindow.__TAURI_INTERNALS__;
});

describe('createTransport', () => {
    it('does not fall back to an in-process domain transport in a Tauri window', async () => {
        tauriWindow.__TAURI_INTERNALS__ = {};

        const transport = createTransport();

        expect(transport.storageMode).toBe('unavailable');
        expect(transport.connectionMode).toBe('unavailable');
        await expect(transport.sandboxRoots()).rejects.toThrow('axklib-server');
    });

    it.each(['local', 'remote'] as const)('preserves the explicit %s connection mode', (mode) => {
        window.__AXKLIB_SERVER__ = {
            baseUrl: 'http://127.0.0.1:42101',
            bearerToken: 'token',
            mode,
        };

        const transport = createTransport();

        expect(transport.connectionMode).toBe(mode);
    });
});
