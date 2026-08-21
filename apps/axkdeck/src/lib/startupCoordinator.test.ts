import { describe, expect, it, vi } from 'vitest';

import { prepareServerConnection, prepareStartup } from './startupCoordinator';

describe('startup coordinator', () => {
    it('starts the connection before paint and defers the application module until after paint', async () => {
        let finishPaint!: () => void;
        const paint = new Promise<void>((resolve) => (finishPaint = resolve));
        const connection = Promise.resolve('connected');
        const loadModule = vi.fn().mockResolvedValue('module');

        const prepared = prepareStartup(connection, () => paint, loadModule);
        expect(loadModule).not.toHaveBeenCalled();

        finishPaint();
        await Promise.resolve();
        expect(loadModule).toHaveBeenCalledOnce();
        await expect(prepared).resolves.toEqual({ connection: 'connected', module: 'module' });
    });

    it('restarts a failed local server before retrying its connection', async () => {
        const calls: string[] = [];
        const useLocalServer = vi.fn(async () => {
            calls.push('restart');
        });
        const loadConnection = vi.fn(async () => {
            calls.push('connect');
            return 'connected';
        });

        await expect(prepareServerConnection(true, useLocalServer, loadConnection)).resolves.toBe('connected');
        expect(calls).toEqual(['restart', 'connect']);
    });

    it('does not alter local server configuration during normal startup', async () => {
        const useLocalServer = vi.fn();
        const loadConnection = vi.fn().mockResolvedValue('connected');

        await expect(prepareServerConnection(false, useLocalServer, loadConnection)).resolves.toBe('connected');
        expect(useLocalServer).not.toHaveBeenCalled();
    });
});
