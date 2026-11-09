import { beforeEach, describe, expect, it, vi } from 'vitest';
const mocks = vi.hoisted(() => ({ invoke: vi.fn(), lstat: vi.fn(), open: vi.fn() }));
vi.mock('@tauri-apps/api/core', () => ({ invoke: mocks.invoke }));
vi.mock('@tauri-apps/plugin-fs', () => ({ lstat: mocks.lstat, open: mocks.open, SeekMode: { Start: 0 } }));
import { nativeDiskSource, selectNativeDiskSources } from './nativeDiskSources';
const seed = { path: '/private/authorized/first.img', identity: 'native:1:2', size: 3 };
const second = { path: '/private/authorized/second.img', identity: 'native:1:3', size: 3 };
beforeEach(() => {
    vi.resetAllMocks();
    mocks.lstat.mockResolvedValue({ isFile: true, isSymlink: false, size: 3 });
});

describe('authorized native disk sources', () => {
    it('keeps paths inside capabilities, exposes canonical identities and reads no payload eagerly', async () => {
        mocks.invoke.mockResolvedValueOnce(seed).mockResolvedValueOnce([second]);
        const source = await nativeDiskSource(seed.path);
        expect(source.identity).toBe(seed.identity);
        expect(JSON.stringify(source)).not.toContain('/private/');
        const files = await source.nearbySources!(new AbortController().signal);
        expect(files.map((file) => file.name)).toEqual(['second.img']);
        expect(files[0].identity).toBe(second.identity);
        expect(mocks.invoke).toHaveBeenLastCalledWith('nearby_native_disk_sources', { path: seed.path });
        expect(mocks.open).not.toHaveBeenCalled();
    });
    it('does not invoke discovery for an aborted capability or admit late scan results', async () => {
        mocks.invoke.mockResolvedValueOnce(seed);
        const source = await nativeDiskSource(seed.path);
        const controller = new AbortController();
        controller.abort();
        await expect(source.nearbySources!(controller.signal)).rejects.toThrow();
        expect(mocks.invoke).toHaveBeenCalledOnce();
        let finish!: (files: (typeof seed)[]) => void;
        mocks.invoke.mockImplementationOnce(
            () =>
                new Promise((resolve) => {
                    finish = resolve;
                }),
        );
        const pendingController = new AbortController();
        const pending = source.nearbySources!(pendingController.signal);
        pendingController.abort();
        finish([second]);
        await expect(pending).rejects.toThrow();
        expect(mocks.lstat).toHaveBeenCalledOnce();
    });
    it('uses native selection descriptors and rejects files changed since admission', async () => {
        mocks.invoke.mockResolvedValueOnce([seed, second]);
        expect((await selectNativeDiskSources()).map((file) => file.identity)).toEqual([
            seed.identity,
            second.identity,
        ]);
        mocks.invoke.mockResolvedValueOnce({ ...seed, size: 2 });
        await expect(nativeDiskSource(seed.path)).rejects.toThrow('changed during admission');
    });
});
