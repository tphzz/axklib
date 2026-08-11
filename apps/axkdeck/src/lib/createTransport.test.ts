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
        await expect(transport.sandboxRoots()).rejects.toThrow('axklib-server');
    });
});
