import { fireEvent, render, screen, within } from '@testing-library/svelte';
import { beforeEach, describe, expect, it, vi } from 'vitest';

const mocks = vi.hoisted(() => ({
    sandboxRoots: vi.fn(),
    sandboxDirectory: vi.fn(),
    openImage: vi.fn(),
    refreshImage: vi.fn(),
    closeImage: vi.fn(),
    contentChildren: vi.fn(),
    objectPage: vi.fn(),
    relationshipPage: vi.fn(),
    inspectObjectDeletion: vi.fn(),
    startObjectDeletion: vi.fn(),
    waitForJob: vi.fn(),
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
        refreshImage: mocks.refreshImage,
        closeImage: mocks.closeImage,
        contentChildren: mocks.contentChildren,
        objectPage: mocks.objectPage,
        relationshipPage: mocks.relationshipPage,
        inspectObjectDeletion: mocks.inspectObjectDeletion,
        startObjectDeletion: mocks.startObjectDeletion,
        waitForJob: mocks.waitForJob,
        deleteSandboxEntry: mocks.deleteSandboxEntry,
        hardDiskCreationProfiles: mocks.hardDiskCreationProfiles,
    }),
}));

vi.mock('./lib/nativeAudioDrop', () => ({
    listenForNativeAudioDrops: mocks.listenForNativeAudioDrops,
}));

import App from './App.svelte';

async function chooseNestedImage(buttonName: 'Open image' | 'Open another image' = 'Open image'): Promise<void> {
    await fireEvent.click(screen.getByRole('button', { name: buttonName }));
    const picker = await screen.findByRole('dialog', { name: 'Open image' });
    if (buttonName === 'Open image') {
        await fireEvent.click(await within(picker).findByText('Yamaha'));
        await fireEvent.click(await within(picker).findByText('images'));
    }
    await fireEvent.click(await within(picker).findByText('nested.hds'));
}

describe('App panel layout', () => {
    beforeEach(() => {
        delete window.__AXKLIB_SERVER__;
        mocks.sandboxRoots.mockReset().mockResolvedValue([{ id: 'workspace', displayName: 'Yamaha', writable: true }]);
        mocks.sandboxDirectory.mockReset().mockImplementation(async (directory) => ({
            directory,
            entries:
                directory.relativePath === 'images'
                    ? [{ name: 'nested.hds', relativePath: 'images/nested.hds', kind: 'FILE', size: 2048 }]
                    : [{ name: 'images', relativePath: 'images', kind: 'DIRECTORY', size: null }],
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
            objectDeletionAvailable: true,
        });
        mocks.refreshImage.mockReset();
        mocks.closeImage.mockReset().mockResolvedValue(undefined);
        mocks.contentChildren.mockReset().mockResolvedValue({ items: [], totalCount: 0 });
        mocks.objectPage.mockReset().mockResolvedValue({ objects: [], totalCount: 0 });
        mocks.relationshipPage.mockReset().mockResolvedValue({ relationships: [], totalCount: 0 });
        mocks.inspectObjectDeletion.mockReset();
        mocks.startObjectDeletion.mockReset();
        mocks.waitForJob.mockReset();
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

        expect(screen.getByRole('button', { name: 'Sample Banks' })).toBeTruthy();
        expect(screen.getByRole('button', { name: 'Samples' })).toBeTruthy();
        expect(screen.getByRole('button', { name: 'Wave Data' })).toBeTruthy();
        expect(screen.queryByText('Sample pool')).toBeNull();
    });

    it('keeps autoplay session-local and disabled until the user enables it', async () => {
        render(App);

        expect(screen.queryByRole('checkbox', { name: 'Autoplay' })).toBeNull();
        await fireEvent.click(screen.getByRole('button', { name: 'Samples' }));
        const autoplay = screen.getByRole('checkbox', { name: 'Autoplay' }) as HTMLInputElement;
        expect(autoplay.checked).toBe(false);
        await fireEvent.click(autoplay);
        expect(autoplay.checked).toBe(true);
    });

    it('uses contained-object lanes above the editor for SBAC and SBNK views', async () => {
        const { container } = render(App);

        await fireEvent.click(screen.getByRole('button', { name: 'Sample Banks' }));
        expect(screen.getByRole('region', { name: 'Sample Bank hierarchy' })).toBeTruthy();
        expect(document.querySelectorAll('.contained-lane')).toHaveLength(3);
        expect(container.querySelector('.object-editor')?.textContent).toContain('No object selected');

        await fireEvent.click(screen.getByRole('button', { name: 'Samples' }));
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

    it('keeps image management commands out of the top toolbar', async () => {
        render(App);

        expect(screen.queryByRole('button', { name: 'Save file' })).toBeNull();
        expect(screen.queryByRole('button', { name: 'Save directory' })).toBeNull();
        expect(screen.queryByRole('button', { name: 'Open disk image' })).toBeNull();
        expect(screen.queryByRole('button', { name: 'Storage locations' })).toBeNull();

        await fireEvent.click(screen.getByRole('button', { name: 'Image options' }));
        expect(screen.getByRole('menuitem', { name: 'Storage locations' })).toBeTruthy();
    });

    it('keeps image lifecycle commands in the image navigator', () => {
        render(App);

        const navigator = screen.getByRole('complementary', { name: 'Image navigator' });
        expect(screen.getByRole('button', { name: 'Open image' }).closest('aside')).toBe(navigator);
        expect(screen.getByRole('button', { name: 'Create image' }).closest('aside')).toBe(navigator);
        expect(screen.queryByRole('textbox', { name: 'Disk image path' })).toBeNull();
        expect(screen.queryByRole('button', { name: 'Eject image' })).toBeNull();
        expect(screen.getByText('Partitions, volumes and objects')).toBeTruthy();
    });

    it('closes the active image and returns to the initial empty state', async () => {
        render(App);

        await chooseNestedImage();
        await vi.waitFor(() => expect(mocks.openImage).toHaveBeenCalledOnce());
        await mocks.openImage.mock.results[0].value;
        await Promise.resolve();

        expect(screen.getAllByText('nested.hds')).not.toHaveLength(0);
        await fireEvent.click(screen.getByRole('button', { name: 'Eject image' }));

        await vi.waitFor(() => expect(mocks.closeImage).toHaveBeenCalledWith(17));
        await vi.waitFor(() => expect(screen.getByRole('button', { name: 'Open image' })).toBeTruthy());
        expect(screen.queryByRole('button', { name: 'Eject image' })).toBeNull();
    });

    it('preserves the active image when opening a replacement fails', async () => {
        render(App);

        await chooseNestedImage();
        await vi.waitFor(() => expect(mocks.openImage).toHaveBeenCalledOnce());
        await mocks.openImage.mock.results[0].value;
        mocks.openImage.mockRejectedValueOnce(new Error('Replacement is invalid'));

        await chooseNestedImage('Open another image');

        await vi.waitFor(() => expect(screen.getByText('Replacement is invalid')).toBeTruthy());
        expect(screen.getAllByText('nested.hds')).not.toHaveLength(0);
        expect(screen.getByRole('button', { name: 'Eject image' })).toBeTruthy();
        expect(mocks.closeImage).not.toHaveBeenCalledWith(17);
    });

    it('closes the active image session when the application is unmounted', async () => {
        const desktop = render(App);

        await chooseNestedImage();
        await vi.waitFor(() => expect(mocks.openImage).toHaveBeenCalledOnce());
        await mocks.openImage.mock.results[0].value;

        desktop.unmount();

        await vi.waitFor(() => expect(mocks.closeImage).toHaveBeenCalledWith(17));
    });

    it('reinspects, completes, and refreshes one object deletion before closing the dialog', async () => {
        const sample = {
            key: 'sample-1',
            objectType: 'SBNK',
            name: 'Piano C3',
            partitionIndex: 0,
            partitionName: 'Partition 0',
            volumeName: 'Piano',
            categoryName: 'SBNK',
            sfsId: 9,
            storedSizeBytes: 512,
            sampleRate: 0,
            rootKey: 60,
            frameCount: 0,
            sampleWidthBytes: 0,
        };
        const volume = {
            id: 'volume-1',
            name: 'Piano',
            kind: 'volume' as const,
            childCount: 0,
            partitionIndex: 0,
        };
        const opened = {
            sessionId: 17,
            tree: [{ id: 'disk-17', name: 'nested.hds', kind: 'disk' as const, childCount: 1, children: [volume] }],
            validation: {
                valid: true,
                issueCount: 0,
                errorCount: 0,
                warningCount: 0,
                objectCount: 1,
                relationshipCount: 0,
            },
            objects: [],
            objectTotalCount: 0,
            initialVolume: volume,
            volumeMutationsAvailable: true,
            partitionMutationsAvailable: true,
            objectDeletionAvailable: true,
        };
        const inspection = {
            valid: true,
            imageId: 'image-1',
            revision: 1,
            targetObjectId: sample.key,
            selectedObjectIds: [sample.key],
            impacts: [
                {
                    objectId: sample.key,
                    objectType: 'SBNK',
                    objectName: sample.name,
                    partitionIndex: 0,
                    partitionName: 'Partition 0',
                    volumeName: 'Piano',
                    role: 'TARGET',
                    status: 'REQUIRED',
                    selected: true,
                    storedSizeBytes: 512,
                    freedClusters: 1,
                    prerequisiteObjectIds: [],
                    reason: 'Selected object',
                },
                {
                    objectId: 'wave-1',
                    objectType: 'SMPL',
                    objectName: 'Piano C3',
                    partitionIndex: 0,
                    partitionName: 'Partition 0',
                    volumeName: 'Piano',
                    role: 'DEPENDENCY',
                    status: 'OPTIONAL',
                    selected: false,
                    storedSizeBytes: 4096,
                    freedClusters: 4,
                    prerequisiteObjectIds: [sample.key],
                    reason: 'Wave Data may be removed after its Sample is deleted',
                },
            ],
            references: [],
            blockers: [],
            warnings: [],
            estimatedFreedBytes: 1024,
            estimatedFreedClusters: 1,
        };
        const selectedInspection = {
            ...inspection,
            selectedObjectIds: [sample.key, 'wave-1'],
            impacts: inspection.impacts.map((impact) =>
                impact.objectId === 'wave-1' ? { ...impact, selected: true } : impact,
            ),
            estimatedFreedBytes: 5120,
            estimatedFreedClusters: 5,
        };
        mocks.openImage.mockResolvedValueOnce(opened);
        mocks.objectPage
            .mockResolvedValueOnce({ objects: [sample], totalCount: 1 })
            .mockResolvedValue({ objects: [], totalCount: 0 });
        mocks.inspectObjectDeletion.mockResolvedValueOnce(inspection).mockResolvedValue(selectedInspection);
        mocks.startObjectDeletion.mockResolvedValue({ jobId: 55, kind: 'images.delete', status: 'queued' });
        mocks.waitForJob.mockResolvedValue({
            jobId: 55,
            kind: 'images.delete',
            status: 'completed',
            result: { deletedObjectIds: [sample.key, 'wave-1'] },
        });
        let finishRefresh: ((value: Awaited<ReturnType<typeof mocks.refreshImage>>) => void) | undefined;
        mocks.refreshImage.mockReturnValue(
            new Promise((resolve) => {
                finishRefresh = resolve;
            }),
        );
        render(App);

        await chooseNestedImage();
        await fireEvent.click(screen.getByRole('button', { name: 'Samples' }));
        await vi.waitFor(() => expect(screen.getByText('Piano C3')).toBeTruthy());
        const row = screen.getByRole('button', { name: 'Inspect Piano C3' });
        await fireEvent.contextMenu(row);
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Delete' }));
        await vi.waitFor(() => expect(screen.getByRole('dialog', { name: 'Delete Sample' })).toBeTruthy());
        await fireEvent.click(screen.getByRole('checkbox', { name: 'Also delete all (1)' }));
        await vi.waitFor(() => expect(mocks.inspectObjectDeletion).toHaveBeenCalledWith(17, sample.key, ['wave-1']));
        await fireEvent.click(screen.getByRole('button', { name: 'Delete 2 objects' }));

        await vi.waitFor(() => expect(mocks.startObjectDeletion).toHaveBeenCalledOnce());
        expect(mocks.inspectObjectDeletion).toHaveBeenCalledTimes(3);
        expect(mocks.startObjectDeletion).toHaveBeenCalledWith(17, sample.key, ['wave-1']);
        await vi.waitFor(() => expect(mocks.waitForJob).toHaveBeenCalledWith(55, expect.any(Function)));
        await vi.waitFor(() => expect(mocks.refreshImage).toHaveBeenCalledWith(17));
        expect(screen.getByRole('dialog', { name: 'Delete Sample' })).toBeTruthy();
        expect((screen.getByRole('button', { name: 'Deleting…' }) as HTMLButtonElement).disabled).toBe(true);
        finishRefresh?.({ ...opened, validation: { ...opened.validation, objectCount: 0 } });
        await vi.waitFor(() => expect(screen.queryByRole('dialog', { name: 'Delete Sample' })).toBeNull());
        expect(screen.queryByText('Piano C3')).toBeNull();
    });

    it('closes an image session that finishes opening after the application is unmounted', async () => {
        let finishOpening: ((value: Awaited<ReturnType<typeof mocks.openImage>>) => void) | undefined;
        mocks.openImage.mockReturnValueOnce(
            new Promise((resolve) => {
                finishOpening = resolve;
            }),
        );
        const desktop = render(App);

        await chooseNestedImage();
        await vi.waitFor(() => expect(mocks.openImage).toHaveBeenCalledOnce());
        desktop.unmount();

        finishOpening?.({
            sessionId: 29,
            tree: [{ id: 'disk-29', name: 'nested.hds', kind: 'disk', childCount: 0 }],
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
            objectDeletionAvailable: true,
        });

        await vi.waitFor(() => expect(mocks.closeImage).toHaveBeenCalledWith(29));
    });

    it('keeps open-image selection free of destructive file-management commands', async () => {
        render(App);

        await fireEvent.click(screen.getByRole('button', { name: 'Open image' }));
        await fireEvent.click(await screen.findByText('Yamaha'));
        await fireEvent.click(await screen.findByText('images'));
        expect(screen.queryByRole('button', { name: /More actions/ })).toBeNull();
        expect(screen.queryByRole('menuitem', { name: 'Delete' })).toBeNull();
        expect(screen.queryByRole('button', { name: 'New folder' })).toBeNull();
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

        await fireEvent.click(screen.getByRole('button', { name: 'Open image' }));
        await fireEvent.click(await screen.findByText('Yamaha'));
        await fireEvent.click(await screen.findByText('images'));
        expect(await screen.findByText('nested.hds')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));

        await fireEvent.click(screen.getByRole('button', { name: 'Open image' }));
        expect(await screen.findByText('nested.hds')).toBeTruthy();
        expect(mocks.sandboxDirectory).toHaveBeenLastCalledWith({
            rootId: 'workspace',
            relativePath: 'images',
        });
    });

    it('starts hard-disk image creation through a dedicated destination picker', async () => {
        render(App);

        await fireEvent.click(screen.getByRole('button', { name: 'Create image' }));
        await fireEvent.click(await screen.findByText('Yamaha'));
        await fireEvent.click(await screen.findByText('images'));
        await fireEvent.click(screen.getByRole('button', { name: 'Select directory' }));

        const dialog = await screen.findByRole('dialog', { name: 'Create HD image' });
        expect(dialog.querySelector('output')?.textContent).toBe('Yamaha/images');
    });
});
