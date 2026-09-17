import { afterEach, describe, expect, it, vi } from 'vitest';
import { fireEvent, render, waitFor } from '@testing-library/svelte';
import AxklibFilesView from './AxklibFilesView.svelte';
import { FilesController } from '../../files/controller.svelte';
import { filesystemEntry, writableFilesRoot } from '../../../lib/testing/filesystem';
import { serverFileLocation } from '../../../lib/storageLocations';
import { listenForNativeMediaDrops } from '../../../lib/nativeMediaDrop';
import type { FilesystemMutationDriver } from '../../../lib/filesystem';
import type { AxklibFilesystemImports } from './su700Actions';

const native = vi.hoisted(() => ({ handler: null as ((event: { payload: unknown }) => void) | null }));
vi.mock('@tauri-apps/api/core', () => ({ invoke: vi.fn(async () => true) }));
vi.mock('@tauri-apps/api/webview', () => ({
    getCurrentWebview: () => ({
        onDragDropEvent: async (handler: typeof native.handler) => {
            native.handler = handler;
            return () => {
                native.handler = null;
            };
        },
    }),
}));
vi.mock('@tauri-apps/plugin-fs', () => ({
    lstat: vi.fn(async () => ({ isFile: true, isDirectory: false, isSymlink: false, size: 512 })),
    open: vi.fn(),
    SeekMode: { Start: 0 },
}));
const originalElementFromPoint = Object.getOwnPropertyDescriptor(document, 'elementFromPoint');
afterEach(() => {
    vi.restoreAllMocks();
    if (originalElementFromPoint) Object.defineProperty(document, 'elementFromPoint', originalElementFromPoint);
    else Reflect.deleteProperty(document, 'elementFromPoint');
});

describe('native Files image drops through the backend wrapper', () => {
    it.each(['EX5 FAT16', 'FAT16'])(
        'keeps the File/Contents dialog open for %s instead of inspecting sampler objects',
        async (filesystemName) => {
            const root = filesystemEntry({ id: 'root', kind: 'root', parentId: null, ancestorIds: [] });
            const file = filesystemEntry({ id: 'file', kind: 'file', name: 'SONG.S1A' });
            const controller = new FilesController({
                inspect: async (query) => ({
                    revision: 1,
                    available: true,
                    filesystemName,
                    deviceView: null,
                    items: query?.parentId ? [file] : [root],
                    totalCount: 1,
                    rootCapabilities: [
                        {
                            ...writableFilesRoot,
                            namePolicy: 'FAT_8_3_UPPERCASE',
                            supportedImports: ['FAT_FLOPPY_CONTENTS'],
                        },
                    ],
                }),
            });
            await controller.initialize();
            const snapshot = { revision: 'source', sizeBytes: 512, sha256: 'a'.repeat(64) };
            const source = serverFileLocation({ rootId: 'host', relativePath: 'DISK.IMA' });
            const complete = { jobId: 1, kind: 'inspection', status: 'completed' as const };
            const images = {
                inspect: vi.fn(async () => ({
                    ...complete,
                    result: {
                        inspectionToken: 'b'.repeat(64),
                        entries: [{ entryId: 'f1', directory: false, relativePath: ['VOICE.S1A'], snapshot }],
                    },
                })),
                release: vi.fn(async () => {}),
            };
            const imports = {
                supportsClientUploads: true,
                images,
                upload: vi.fn(async () => [source]),
                release: vi.fn(async () => {}),
                inspectInputs: vi.fn(async () => ({
                    ...complete,
                    result: { inputs: [{ source: { fileRef: source.reference }, snapshot }] },
                })),
                chooseFiles: vi.fn(),
                chooseDirectory: vi.fn(),
                inspectDestination: vi.fn(),
                observe: vi.fn(),
                cancel: vi.fn(),
                // Even an available sampler importer must not intercept a FAT destination.
                floppy: { available: true, request: null, requestDroppedFiles: vi.fn(), destinations: vi.fn() },
            } as unknown as AxklibFilesystemImports;
            const driver: FilesystemMutationDriver = {
                execute: vi.fn(),
                refresh: vi.fn(),
                observe: vi.fn(),
                cancel: vi.fn(),
            };
            const view = render(AxklibFilesView, { controller, sessionId: 1, imports, driver });
            const row = view.getByRole('row');
            Object.defineProperty(document, 'elementFromPoint', { configurable: true, value: () => row });
            const deviceDrop = vi.fn();
            const stop = await listenForNativeMediaDrops({
                enabled: () => false,
                interfaceZoom: () => 1,
                onDrop: deviceDrop,
                onHover: vi.fn(),
                onError: vi.fn(),
            });
            native.handler!({ payload: { type: 'drop', paths: ['/tmp/DISK.IMA'], position: { x: 10, y: 10 } } });
            await waitFor(() => expect(images.inspect).toHaveBeenCalledOnce());
            const contents = await view.findByRole('button', { name: 'Contents' });
            expect(view.getByRole('dialog')).toBeTruthy();
            expect(await view.findByDisplayValue('VOICE.S1A')).toBeTruthy();
            await fireEvent.click(view.getByRole('button', { name: 'File' }));
            await fireEvent.click(contents);
            expect(view.getByRole('dialog')).toBeTruthy();
            expect(deviceDrop).not.toHaveBeenCalled();
            expect(imports.floppy!.requestDroppedFiles).not.toHaveBeenCalled();
            expect(driver.execute).not.toHaveBeenCalled();
            await fireEvent.click(view.getByRole('button', { name: 'Cancel' }));
            await waitFor(() => expect(view.queryByRole('dialog')).toBeNull());
            stop();
        },
    );
});
