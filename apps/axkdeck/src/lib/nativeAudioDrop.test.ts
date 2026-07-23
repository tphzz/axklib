import { beforeEach, describe, expect, it, vi } from 'vitest';

const mocks = vi.hoisted(() => ({
    dragHandler: null as ((event: { payload: unknown }) => void) | null,
    lstat: vi.fn(),
    open: vi.fn(),
    unlisten: vi.fn(),
}));

vi.mock('@tauri-apps/api/webview', () => ({
    getCurrentWebview: () => ({
        onDragDropEvent: vi.fn(async (handler: (event: { payload: unknown }) => void) => {
            mocks.dragHandler = handler;
            return mocks.unlisten;
        }),
    }),
}));

vi.mock('@tauri-apps/plugin-fs', () => ({
    lstat: mocks.lstat,
    open: mocks.open,
    SeekMode: { Start: 0 },
}));

import { listenForNativeAudioDrops } from './nativeAudioDrop';

describe('native audio drops', () => {
    beforeEach(() => {
        mocks.dragHandler = null;
        mocks.lstat.mockReset();
        mocks.open.mockReset();
        mocks.unlisten.mockReset();
        mocks.lstat.mockResolvedValue({ isFile: true, isSymlink: false, size: 3 });
    });

    it('stats supported native paths without reading them before handing them to the bounded uploader', async () => {
        mocks.lstat.mockImplementation(async (path: string) => ({
            isFile: true,
            isSymlink: false,
            size: path.endsWith('take.wav') ? 3 : 2,
        }));
        const onHover = vi.fn();
        const onDrop = vi.fn();
        const onError = vi.fn();

        const unlisten = await listenForNativeAudioDrops({ onHover, onDrop, onError });
        expect(mocks.dragHandler).not.toBeNull();

        mocks.dragHandler!({
            payload: {
                type: 'enter',
                paths: ['/samples/take.wav', '/samples/notes.txt', 'C:\\samples\\pad.FLAC'],
                position: { x: 200, y: 100 },
            },
        });
        expect(onHover).toHaveBeenLastCalledWith(true, { x: 200, y: 100 });

        mocks.dragHandler!({
            payload: {
                type: 'drop',
                paths: ['/samples/take.wav', '/samples/notes.txt', 'C:\\samples\\pad.FLAC'],
                position: { x: 220, y: 120 },
            },
        });

        await vi.waitFor(() => expect(onDrop).toHaveBeenCalledOnce());
        expect(mocks.lstat).toHaveBeenCalledTimes(2);
        expect(mocks.open).not.toHaveBeenCalled();
        const [files, position] = onDrop.mock.calls[0] as [
            { name: string; type: string; size: number }[],
            { x: number; y: number },
        ];
        expect(files.map((file) => ({ name: file.name, type: file.type, size: file.size }))).toEqual([
            { name: 'take.wav', type: 'audio/wav', size: 3 },
            { name: 'pad.FLAC', type: 'audio/flac', size: 2 },
        ]);
        expect(position).toEqual({ x: 220, y: 120 });
        expect(onHover).toHaveBeenLastCalledWith(false, { x: 220, y: 120 });
        expect(onError).not.toHaveBeenCalled();

        unlisten();
        expect(mocks.unlisten).toHaveBeenCalledOnce();
    });

    it('reports native read failures and clears the hover state', async () => {
        const failure = new Error('stat denied');
        mocks.lstat.mockRejectedValue(failure);
        const onHover = vi.fn();
        const onDrop = vi.fn();
        const onError = vi.fn();

        await listenForNativeAudioDrops({ onHover, onDrop, onError });
        mocks.dragHandler!({
            payload: { type: 'drop', paths: ['/samples/take.aiff'], position: { x: 5, y: 7 } },
        });

        await vi.waitFor(() => expect(onError).toHaveBeenCalledWith(failure));
        expect(onHover).toHaveBeenCalledWith(false, { x: 5, y: 7 });
        expect(onDrop).not.toHaveBeenCalled();
    });

    it('rejects oversized files before opening or reading them', async () => {
        mocks.lstat.mockResolvedValue({ isFile: true, isSymlink: false, size: 4 * 1024 * 1024 * 1024 + 1 });
        const onError = vi.fn();

        await listenForNativeAudioDrops({ onHover: vi.fn(), onDrop: vi.fn(), onError });
        mocks.dragHandler!({
            payload: { type: 'drop', paths: ['/samples/huge.wav'], position: { x: 5, y: 7 } },
        });

        await vi.waitFor(() => expect(onError).toHaveBeenCalledOnce());
        expect(mocks.open).not.toHaveBeenCalled();
    });
});
