import { beforeEach, describe, expect, it, vi } from 'vitest';

const mocks = vi.hoisted(() => ({ invoke: vi.fn() }));
vi.mock('@tauri-apps/api/core', () => ({ invoke: mocks.invoke }));

import { configureRemoteServer, useLocalServer } from './serverSettings';

describe('server settings', () => {
    beforeEach(() => {
        mocks.invoke.mockReset();
        vi.unstubAllGlobals();
    });

    it('preflights the normalized endpoint before storing its credentials', async () => {
        mocks.invoke.mockImplementation((command: string) => {
            if (command === 'validate_remote_server_settings') {
                return Promise.resolve({
                    baseUrl: 'https://sampler.example.test/api/v1',
                    bearerToken: '0123456789abcdef0123456789abcdef',
                    mode: 'remote',
                });
            }
            if (command === 'configure_remote_server') {
                return Promise.resolve({
                    mode: 'remote',
                    baseUrl: 'https://sampler.example.test/api/v1',
                    tokenConfigured: true,
                    secureStorageError: null,
                });
            }
            throw new Error(`unexpected command ${command}`);
        });
        const fetch = vi.fn().mockResolvedValue(
            new Response(
                JSON.stringify({
                    data: {
                        apiVersion: 'v1',
                        operations: [],
                        limits: {},
                    },
                }),
                { status: 200, headers: { 'content-type': 'application/json' } },
            ),
        );
        vi.stubGlobal('fetch', fetch);

        await configureRemoteServer({
            baseUrl: 'https://sampler.example.test',
            bearerToken: '0123456789abcdef0123456789abcdef',
        });

        expect(fetch).toHaveBeenCalledWith(
            'https://sampler.example.test/api/v1/system/capabilities',
            expect.objectContaining({
                headers: expect.objectContaining({
                    Authorization: 'Bearer 0123456789abcdef0123456789abcdef',
                }),
            }),
        );
        expect(mocks.invoke).toHaveBeenLastCalledWith('configure_remote_server', {
            settings: {
                baseUrl: 'https://sampler.example.test',
                bearerToken: '0123456789abcdef0123456789abcdef',
            },
        });
    });

    it('does not store credentials when capability discovery fails', async () => {
        mocks.invoke.mockResolvedValue({
            baseUrl: 'https://sampler.example.test/api/v1',
            bearerToken: '0123456789abcdef0123456789abcdef',
            mode: 'remote',
        });
        vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response('', { status: 401 })));

        await expect(
            configureRemoteServer({
                baseUrl: 'https://sampler.example.test',
                bearerToken: '0123456789abcdef0123456789abcdef',
            }),
        ).rejects.toThrow();

        expect(mocks.invoke).toHaveBeenCalledTimes(1);
        expect(mocks.invoke).not.toHaveBeenCalledWith('configure_remote_server', expect.anything());
    });

    it('clears protected remote settings through the backend', async () => {
        mocks.invoke.mockResolvedValue({
            mode: 'local',
            baseUrl: null,
            tokenConfigured: false,
            secureStorageError: null,
        });
        await useLocalServer();
        expect(mocks.invoke).toHaveBeenCalledWith('use_local_server');
    });
});
