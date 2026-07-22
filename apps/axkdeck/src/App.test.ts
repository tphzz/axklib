import { fireEvent, render, screen } from '@testing-library/svelte';
import { beforeEach, describe, expect, it, vi } from 'vitest';

const mocks = vi.hoisted(() => ({
    sandboxRoots: vi.fn(),
    sandboxDirectory: vi.fn(),
    openImage: vi.fn(),
    closeImage: vi.fn(),
    deleteSandboxEntry: vi.fn(),
    hardDiskCreationProfiles: vi.fn(),
    listenForNativeAudioDrops: vi.fn(),
}));

vi.mock('./lib/createTransport', () => ({
    createTransport: () => ({
        storageMode: 'server',
        supportsClientUploads: false,
        sandboxRoots: mocks.sandboxRoots,
        sandboxDirectory: mocks.sandboxDirectory,
        openImage: mocks.openImage,
        closeImage: mocks.closeImage,
        deleteSandboxEntry: mocks.deleteSandboxEntry,
        hardDiskCreationProfiles: mocks.hardDiskCreationProfiles,
    }),
}));

vi.mock('./lib/nativeAudioDrop', () => ({
    listenForNativeAudioDrops: mocks.listenForNativeAudioDrops,
}));

import App from './App.svelte';

describe('App panel layout', () => {
    beforeEach(() => {
        delete window.__AXKLIB_SERVER__;
        mocks.sandboxRoots.mockReset().mockResolvedValue([{ id: 'workspace', displayName: 'Yamaha', writable: true }]);
        mocks.sandboxDirectory.mockReset().mockImplementation(async (directory) => ({
            directory,
            entries:
                directory.relativePath === 'images'
                    ? [{ name: 'nested.hds', relativePath: 'images/nested.hds', kind: 'file', size: 2048 }]
                    : [{ name: 'images', relativePath: 'images', kind: 'directory', size: null }],
            truncated: false,
            nextCursor: null,
        }));
        mocks.hardDiskCreationProfiles.mockReset().mockResolvedValue([
            {
                profileId: 'FLOPPY_SCALE',
                sizeBytes: 1474560,
                defaultPartitionCount: 1,
                partitionOptions: [{ partitionCount: 1, partitionSizeBytes: 1454080, unusedTailBytes: 0 }],
            },
        ]);
        mocks.openImage.mockReset().mockResolvedValue({
            sessionId: 17,
            tree: [{ id: 'disk-17', name: 'nested.hds', kind: 'disk', childCount: 0 }],
            validation: {
                valid: true,
                issueCount: 0,
                errorCount: 0,
                warningCount: 0,
                objectCount: 0,
                relationshipCount: 0,
            },
            objects: [],
            objectTotalCount: 0,
            initialVolume: null,
            volumeMutationsAvailable: true,
            partitionMutationsAvailable: true,
        });
        mocks.closeImage.mockReset().mockResolvedValue(undefined);
        mocks.deleteSandboxEntry.mockReset().mockResolvedValue(undefined);
        mocks.listenForNativeAudioDrops.mockReset().mockResolvedValue(() => undefined);
    });

    it('keeps one stable toolbar across all side-panel combinations', async () => {
        const { container } = render(App);
        const shell = container.querySelector('.app-shell');
        const toolbar = screen.getByRole('toolbar', { name: 'Panel layout' });
        const library = screen.getByRole('button', { name: 'Library panel' });
        const inspector = screen.getByRole('button', { name: 'Inspector panel' });
        const editor = screen.getByRole('button', { name: 'Editor panel' });

        expect(shell?.classList.contains('sidebar-closed')).toBe(false);
        expect(shell?.classList.contains('inspector-closed')).toBe(false);

        await fireEvent.click(editor);
        expect(screen.queryByRole('region', { name: 'Object editor' })).toBeNull();
        expect(editor.getAttribute('aria-pressed')).toBe('false');
        await fireEvent.click(editor);
        expect(screen.getByRole('region', { name: 'Object editor' })).toBeTruthy();

        await fireEvent.click(library);
        expect(screen.getByRole('toolbar', { name: 'Panel layout' })).toBe(toolbar);
        expect(shell?.classList.contains('sidebar-closed')).toBe(true);
        expect(shell?.classList.contains('inspector-closed')).toBe(false);

        await fireEvent.click(inspector);
        expect(screen.getByRole('toolbar', { name: 'Panel layout' })).toBe(toolbar);
        expect(shell?.classList.contains('sidebar-closed')).toBe(true);
        expect(shell?.classList.contains('inspector-closed')).toBe(true);

        await fireEvent.click(library);
        expect(shell?.classList.contains('sidebar-closed')).toBe(false);
        expect(shell?.classList.contains('inspector-closed')).toBe(true);

        await fireEvent.click(inspector);
        expect(shell?.classList.contains('sidebar-closed')).toBe(false);
        expect(shell?.classList.contains('inspector-closed')).toBe(false);
    });

    it('uses canonical Yamaha object terminology', () => {
        render(App);

        expect(screen.getByRole('button', { name: 'Sample Banks (SBAC)' })).toBeTruthy();
        expect(screen.getByRole('button', { name: 'Samples (SBNK)' })).toBeTruthy();
        expect(screen.getByRole('button', { name: 'Wave Data (SMPL)' })).toBeTruthy();
        expect(screen.queryByText('Sample pool')).toBeNull();
    });

    it('uses contained-object lanes above the editor for SBAC and SBNK views', async () => {
        const { container } = render(App);

        await fireEvent.click(screen.getByRole('button', { name: 'Sample Banks (SBAC)' }));
        expect(screen.getByRole('region', { name: 'Sample Bank hierarchy' })).toBeTruthy();
        expect(document.querySelectorAll('.contained-lane')).toHaveLength(3);
        expect(container.querySelector('.object-editor')?.textContent).toContain('No object selected');

        await fireEvent.click(screen.getByRole('button', { name: 'Samples (SBNK)' }));
        expect(screen.getByRole('region', { name: 'Sample hierarchy' })).toBeTruthy();
        expect(document.querySelectorAll('.contained-lane')).toHaveLength(2);
        expect(container.querySelector('.object-editor')?.textContent).toContain('No object selected');
    });

    it('defaults the object browser to two-thirds of the middle workspace', () => {
        const { container } = render(App);

        expect(container.querySelector<HTMLElement>('.main-stage')?.style.getPropertyValue('--split-position')).toBe(
            '66.66666666666666%',
        );
    });

    it('does not expose the temporary save commands in the top toolbar', () => {
        render(App);

        expect(screen.queryByRole('button', { name: 'Save file' })).toBeNull();
        expect(screen.queryByRole('button', { name: 'Save directory' })).toBeNull();
        expect(screen.getByRole('button', { name: 'Manage workspaces' })).toBeTruthy();
    });

    it('uses one non-editable image chooser button instead of a selectable path input', () => {
        render(App);

        const chooser = screen.getByRole('button', { name: 'Open disk image' });
        expect(chooser.textContent).toContain('No disk image selected');
        expect(screen.queryByRole('textbox', { name: 'Disk image path' })).toBeNull();
        expect((screen.getByRole('button', { name: 'Eject disk image' }) as HTMLButtonElement).disabled).toBe(true);
    });

    it('ejects the active image and returns to the initial empty state', async () => {
        render(App);

        await fireEvent.click(screen.getByRole('button', { name: 'Open disk image' }));
        await fireEvent.click(await screen.findByText('Yamaha'));
        await fireEvent.click(await screen.findByText('images'));
        await fireEvent.click(await screen.findByText('nested.hds'));
        await vi.waitFor(() => expect(mocks.openImage).toHaveBeenCalledOnce());
        await mocks.openImage.mock.results[0].value;
        await Promise.resolve();

        const eject = screen.getByRole('button', { name: 'Eject disk image' });
        expect((eject as HTMLButtonElement).disabled).toBe(false);
        await fireEvent.click(eject);

        await vi.waitFor(() => expect(mocks.closeImage).toHaveBeenCalledWith(17));
        await vi.waitFor(() =>
            expect(screen.getByRole('button', { name: 'Open disk image' }).textContent).toContain(
                'No disk image selected',
            ),
        );
        expect((eject as HTMLButtonElement).disabled).toBe(true);
    });

    it('closes an image session that finishes opening after it was ejected', async () => {
        let finishOpening: ((value: Awaited<ReturnType<typeof mocks.openImage>>) => void) | undefined;
        mocks.openImage.mockReturnValueOnce(
            new Promise((resolve) => {
                finishOpening = resolve;
            }),
        );
        render(App);

        await fireEvent.click(screen.getByRole('button', { name: 'Open disk image' }));
        await fireEvent.click(await screen.findByText('Yamaha'));
        await fireEvent.click(await screen.findByText('images'));
        await fireEvent.click(await screen.findByText('nested.hds'));
        await vi.waitFor(() => expect(mocks.openImage).toHaveBeenCalledOnce());
        await fireEvent.click(screen.getByRole('button', { name: 'Eject disk image' }));

        finishOpening?.({
            sessionId: 23,
            tree: [{ id: 'disk-23', name: 'nested.hds', kind: 'disk', childCount: 0 }],
            validation: {
                valid: true,
                issueCount: 0,
                errorCount: 0,
                warningCount: 0,
                objectCount: 0,
                relationshipCount: 0,
            },
            objects: [],
            objectTotalCount: 0,
            initialVolume: null,
            volumeMutationsAvailable: true,
            partitionMutationsAvailable: true,
        });

        await vi.waitFor(() => expect(mocks.closeImage).toHaveBeenCalledWith(23));
        expect(screen.getByRole('button', { name: 'Open disk image' }).textContent).toContain('No disk image selected');
    });

    it('ejects an open image before permanently deleting its backing file', async () => {
        render(App);

        await fireEvent.click(screen.getByRole('button', { name: 'Open disk image' }));
        await fireEvent.click(await screen.findByText('Yamaha'));
        await fireEvent.click(await screen.findByText('images'));
        await fireEvent.click(await screen.findByText('nested.hds'));
        await fireEvent.click(screen.getByRole('button', { name: 'Open disk image' }));
        await fireEvent.click(await screen.findByRole('button', { name: 'More actions for nested.hds' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Delete' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Delete permanently' }));

        await vi.waitFor(() =>
            expect(mocks.deleteSandboxEntry).toHaveBeenCalledWith({
                rootId: 'workspace',
                relativePath: 'images/nested.hds',
            }),
        );
        expect(mocks.closeImage.mock.invocationCallOrder[0]).toBeLessThan(
            mocks.deleteSandboxEntry.mock.invocationCallOrder[0],
        );
    });

    it('suppresses context menus only in the desktop runtime', async () => {
        const runtime = window as unknown as { __TAURI_INTERNALS__?: unknown };
        runtime.__TAURI_INTERNALS__ = {};
        const desktop = render(App);
        const desktopEvent = new MouseEvent('contextmenu', { bubbles: true, cancelable: true });
        window.dispatchEvent(desktopEvent);
        expect(desktopEvent.defaultPrevented).toBe(true);
        desktop.unmount();

        delete runtime.__TAURI_INTERNALS__;
        render(App);
        const browserEvent = new MouseEvent('contextmenu', { bubbles: true, cancelable: true });
        window.dispatchEvent(browserEvent);
        expect(browserEvent.defaultPrevented).toBe(false);
    });

    it('prevents WebKit URI-list file drops from navigating away', async () => {
        render(App);

        const file = new File(['audio'], 'take.wav', { type: 'audio/wav' });
        const dataTransfer = {
            types: ['text/uri-list'],
            files: [file],
            dropEffect: 'none',
        } as unknown as DataTransfer;
        const dragOver = new Event('dragover', { bubbles: true, cancelable: true }) as DragEvent;
        Object.defineProperty(dragOver, 'dataTransfer', { value: dataTransfer });
        window.dispatchEvent(dragOver);
        expect(dragOver.defaultPrevented).toBe(true);

        const drop = new Event('drop', { bubbles: true, cancelable: true }) as DragEvent;
        Object.defineProperty(drop, 'dataTransfer', { value: dataTransfer });
        window.dispatchEvent(drop);
        expect(drop.defaultPrevented).toBe(true);
        expect(await screen.findByText('Select a writable volume first')).toBeTruthy();
    });

    it('routes native Tauri file drops through the audio import flow', async () => {
        const runtime = window as unknown as { __TAURI_INTERNALS__?: unknown };
        runtime.__TAURI_INTERNALS__ = {};
        render(App);

        await vi.waitFor(() => expect(mocks.listenForNativeAudioDrops).toHaveBeenCalledOnce());
        const callbacks = mocks.listenForNativeAudioDrops.mock.calls[0][0];
        callbacks.onDrop([new File(['audio'], 'take.wav', { type: 'audio/wav' })], { x: 20, y: 30 }, 1);

        expect(await screen.findByText('Select a writable volume first')).toBeTruthy();
        delete runtime.__TAURI_INTERNALS__;
    });

    it('consumes unsupported and empty file drops without opening the import dialog', async () => {
        render(App);

        const unsupportedTransfer = {
            types: ['text/uri-list'],
            files: [new File(['text'], 'notes.txt', { type: 'text/plain' })],
            dropEffect: 'none',
        } as unknown as DataTransfer;
        const unsupportedDrop = new Event('drop', { bubbles: true, cancelable: true }) as DragEvent;
        Object.defineProperty(unsupportedDrop, 'dataTransfer', { value: unsupportedTransfer });
        window.dispatchEvent(unsupportedDrop);
        expect(unsupportedDrop.defaultPrevented).toBe(true);
        expect(await screen.findByText('No supported audio files were dropped')).toBeTruthy();
        expect(screen.queryByRole('dialog', { name: 'Import audio' })).toBeNull();

        const emptyTransfer = {
            types: ['text/uri-list'],
            files: [],
            dropEffect: 'none',
        } as unknown as DataTransfer;
        const emptyDrop = new Event('drop', { bubbles: true, cancelable: true }) as DragEvent;
        Object.defineProperty(emptyDrop, 'dataTransfer', { value: emptyTransfer });
        window.dispatchEvent(emptyDrop);
        expect(emptyDrop.defaultPrevented).toBe(true);
        expect(screen.queryByRole('dialog', { name: 'Import audio' })).toBeNull();
    });

    it('restores the last image-picker directory after cancelling', async () => {
        render(App);

        await fireEvent.click(screen.getByRole('button', { name: 'Open disk image' }));
        await fireEvent.click(await screen.findByText('Yamaha'));
        await fireEvent.click(await screen.findByText('images'));
        expect(await screen.findByText('nested.hds')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));

        await fireEvent.click(screen.getByRole('button', { name: 'Open disk image' }));
        expect(await screen.findByText('nested.hds')).toBeTruthy();
        expect(mocks.sandboxDirectory).toHaveBeenLastCalledWith({
            rootId: 'workspace',
            relativePath: 'images',
        });
    });

    it('starts hard-disk image creation in the directory currently shown by the image browser', async () => {
        render(App);

        await fireEvent.click(screen.getByRole('button', { name: 'Open disk image' }));
        await fireEvent.click(await screen.findByText('Yamaha'));
        await fireEvent.click(await screen.findByText('images'));
        await fireEvent.click(screen.getByRole('button', { name: 'New HD image' }));

        const dialog = await screen.findByRole('dialog', { name: 'Create HD image' });
        expect(dialog.querySelector('output')?.textContent).toBe('Yamaha/images');
    });
});
