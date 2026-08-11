import { describe, expect, it, vi } from 'vitest';
import { createConnectionActions } from './actions';

vi.mock('../../lib/serverSettings', () => ({
    configureRemoteServer: vi.fn(async () => undefined),
    remoteServerSettings: vi.fn(async () => ({ mode: 'local' })),
    useLocalServer: vi.fn(async () => undefined),
}));

describe('connection actions', () => {
    it('reloads only after a connection change completes', async () => {
        const reload = vi.fn();
        const actions = createConnectionActions(reload);
        await actions.saveRemote({ baseUrl: 'https://example.test', bearerToken: 'secret' });
        expect(reload).toHaveBeenCalledOnce();
    });
});
