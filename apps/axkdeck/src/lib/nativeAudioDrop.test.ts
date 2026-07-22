import { beforeEach, describe, expect, it, vi } from 'vitest';

const mocks = vi.hoisted(() => ({
    dragHandler: null as ((event: { payload: unknown }) => void) | null,
    readFile: vi.fn(),
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
    readFile: mocks.readFile,
}));

import { listenForNativeAudioDrops } from './nativeAudioDrop';

describe('native audio drops', () => {
    beforeEach(() => {
        mocks.dragHandler = null;
        mocks.readFile.mockReset();
        mocks.unlisten.mockReset();
    });

    it('reads only supported native paths and returns browser File objects at the drop position', async () => {
        mocks.readFile.mockImplementation(async (path: string) =>
            path.endsWith('take.wav') ? new Uint8Array([1, 2, 3]) : new Uint8Array([4, 5]),
        );
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
        expect(mocks.readFile).toHaveBeenCalledTimes(2);
        expect(mocks.readFile).toHaveBeenNthCalledWith(1, '/samples/take.wav');
        expect(mocks.readFile).toHaveBeenNthCalledWith(2, 'C:\\samples\\pad.FLAC');
        const [files, position] = onDrop.mock.calls[0] as [File[], { x: number; y: number }];
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
        const failure = new Error('read denied');
        mocks.readFile.mockRejectedValue(failure);
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
});
