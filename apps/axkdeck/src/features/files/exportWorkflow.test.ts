import { describe, expect, it, vi } from 'vitest';
import { FilesExportWorkflow } from './exportWorkflow.svelte';
import { filesystemEntry } from '../../lib/testing/filesystem';
import type { FilesystemExportActions, FilesystemExportResult } from '../../lib/filesystemExport';
import type { JobState } from '../../lib/transport';

function setup(directComputer = true) {
    const result: FilesystemExportResult = {
        rootDirectory: null,
        imageId: 'image',
        revision: 7,
        entries: [{ entryId: 'file', sourcePath: '/file', relativePath: ['file'], directory: false, sizeBytes: 4 }],
        notices: [],
        totalBytes: 4,
        destination: 'DOWNLOAD',
        output: null,
        download: {
            archiveId: 'archive',
            filename: 'Files.tar',
            sizeBytes: 2048,
            expiresInSeconds: 300,
            contentPath: '/downloads/archive/content',
        },
    };
    const publish = vi.fn().mockResolvedValue(undefined);
    const completed: JobState = { jobId: 12, kind: 'images.filesystem.export', status: 'completed', result };
    const driver = {
        directComputer,
        desktop: true,
        inspect: vi.fn().mockResolvedValue(result),
        chooseDestination: vi
            .fn()
            .mockResolvedValue({ destination: { kind: 'DOWNLOAD', directoryName: 'Files' }, publish }),
        execute: vi.fn<FilesystemExportActions['execute']>().mockResolvedValue(completed),
        observe: vi.fn().mockResolvedValue(completed),
        cancel: vi.fn().mockResolvedValue(undefined),
        release: vi.fn().mockResolvedValue(undefined),
    };
    const workflow = new FilesExportWorkflow();
    return { workflow, driver, result, completed, publish };
}

describe('Files export lifecycle', () => {
    it.each(['notice', 'cleanup'])('keeps successful output visible for a %s warning', async (kind) => {
        const { workflow, driver, result } = setup();
        if (kind === 'notice') result.notices = [{ entryId: 'meta', sourcePath: '/meta', message: 'Metadata omitted' }];
        else driver.release.mockRejectedValueOnce(new Error('Connection lost'));
        await workflow.open(7, [filesystemEntry()], driver);
        await workflow.choose('computer');
        expect(workflow.phase).toBe('completed');
        expect(workflow.review).not.toBeNull();
        expect(workflow.message).toContain(kind === 'notice' ? 'notice' : 'cleanup failed');
        expect(workflow.canChoose).toBe(false);
    });

    it('can export an empty directory and requests folder layout for both inspection and execution', async () => {
        const { workflow, driver, result } = setup();
        result.rootDirectory = { entryId: 'empty', sourcePath: '/Empty', name: 'Empty' };
        result.entries = [];
        await workflow.open(7, [filesystemEntry({ id: 'empty' })], driver);
        expect(workflow.canChoose).toBe(true);
        expect(workflow.entryCount).toBe(1);
        await workflow.choose('computer');
        expect(driver.inspect).toHaveBeenCalledWith(7, ['empty'], 'EXPORT_FOLDER');
        expect(driver.execute).toHaveBeenCalledWith(
            7,
            ['empty'],
            expect.anything(),
            expect.any(Function),
            'EXPORT_FOLDER',
        );
        expect(workflow.review).toBeNull();
    });

    it('closes automatically only after publication and cleanup have completed', async () => {
        const { driver } = setup();
        const status = vi.fn();
        const workflow = new FilesExportWorkflow(status);
        let finish!: () => void;
        driver.release.mockImplementation(
            () =>
                new Promise<void>((resolve) => {
                    finish = resolve;
                }),
        );
        await workflow.open(7, [filesystemEntry()], driver);
        const exporting = workflow.choose('computer');
        await vi.waitFor(() => expect(driver.release).toHaveBeenCalledOnce());
        expect(workflow.review).not.toBeNull();
        expect(status).not.toHaveBeenCalled();
        finish();
        await exporting;
        expect(workflow.review).toBeNull();
        expect(status).toHaveBeenCalledWith('Exported 1 entry');
    });

    it('cancels local publication and retries the retained archive without another export job', async () => {
        const { workflow, driver, publish } = setup();
        let fail!: (error: Error) => void;
        publish.mockImplementationOnce(
            () =>
                new Promise((_resolve, reject) => {
                    fail = reject;
                }),
        );
        const cancelPublication = vi.fn().mockResolvedValue(undefined);
        driver.chooseDestination.mockResolvedValue({
            destination: { kind: 'DOWNLOAD', directoryName: 'Files' },
            publish,
            cancelPublication,
        });
        await workflow.open(7, [filesystemEntry()], driver);
        const exporting = workflow.choose('computer');
        await vi.waitFor(() => expect(workflow.phase).toBe('saving'));
        expect(workflow.canCancel).toBe(true);
        await workflow.cancel();
        expect(cancelPublication).toHaveBeenCalledOnce();
        expect(driver.cancel).not.toHaveBeenCalled();
        workflow.close();
        expect(workflow.review).not.toBeNull();
        fail(new Error('Directory export cancelled'));
        await exporting;
        expect(workflow.phase).toBe('failed');
        expect(driver.release).not.toHaveBeenCalled();
        await workflow.choose('computer');
        expect(driver.execute).toHaveBeenCalledOnce();
        expect(driver.release).toHaveBeenCalledOnce();
        expect(workflow.phase).toBe('completed');
    });

    it('cancels publication on disposal and releases the archive only after the worker settles', async () => {
        const { workflow, driver, publish, result } = setup();
        let fail!: (error: Error) => void;
        publish.mockImplementationOnce(
            () =>
                new Promise((_resolve, reject) => {
                    fail = reject;
                }),
        );
        const cancelPublication = vi.fn().mockResolvedValue(undefined);
        driver.chooseDestination.mockResolvedValue({
            destination: { kind: 'DOWNLOAD', directoryName: 'Files' },
            publish,
            cancelPublication,
        });
        await workflow.open(7, [filesystemEntry()], driver);
        const exporting = workflow.choose('computer');
        await vi.waitFor(() => expect(workflow.phase).toBe('saving'));
        workflow.dispose();
        expect(cancelPublication).toHaveBeenCalledOnce();
        expect(driver.release).not.toHaveBeenCalled();
        fail(new Error('Directory export cancelled'));
        await exporting;
        expect(driver.release).toHaveBeenCalledExactlyOnceWith(result);
    });

    it('does not replace a completed save with a late cancellation error', async () => {
        const { workflow, driver, publish } = setup();
        let finish!: () => void;
        let failCancel!: (error: Error) => void;
        publish.mockImplementationOnce(
            () =>
                new Promise<void>((resolve) => {
                    finish = resolve;
                }),
        );
        const cancelPublication = vi.fn(
            () =>
                new Promise<void>((_resolve, reject) => {
                    failCancel = reject;
                }),
        );
        driver.chooseDestination.mockResolvedValue({
            destination: { kind: 'DOWNLOAD', directoryName: 'Files' },
            publish,
            cancelPublication,
        });
        await workflow.open(7, [filesystemEntry()], driver);
        const exporting = workflow.choose('computer');
        await vi.waitFor(() => expect(workflow.phase).toBe('saving'));
        const cancelling = workflow.cancel();
        finish();
        await exporting;
        failCancel(new Error('Late cancellation failure'));
        await cancelling;
        expect(workflow.phase).toBe('completed');
        expect(workflow.message).toBe('Exported 1 entry');
    });

    it('does not let late progress from a closed review overwrite a new review', async () => {
        const { workflow, driver } = setup();
        let late!: (job: JobState) => void;
        driver.execute.mockImplementationOnce(async (_revision, _ids, _destination, update) => {
            late = update;
            throw new Error('Connection lost');
        });
        await workflow.open(7, [filesystemEntry()], driver);
        await workflow.choose('computer');
        workflow.close();
        await workflow.open(7, [filesystemEntry({ id: 'other' })], driver);
        late({ jobId: 12, kind: 'images.filesystem.export', status: 'running' });
        expect(workflow.jobId).toBeNull();
        expect(workflow.message).toBe('Ready');
        expect(driver.cancel).toHaveBeenCalledWith(12);
    });
    it('uses the inspected host-safe name as the folder suggestion', async () => {
        const { workflow, driver, result } = setup();
        driver.inspect.mockResolvedValueOnce({
            ...result,
            rootDirectory: { entryId: 'root', sourcePath: '/A/B', name: 'A_B' },
        });
        await workflow.open(7, [filesystemEntry({ name: 'A/B' })], driver);
        await workflow.choose('computer');
        expect(driver.chooseDestination).toHaveBeenCalledWith('computer', 'A_B');
    });
    it('keeps the chosen computer route after a remote download save fails', async () => {
        const { workflow, driver, publish } = setup(false);
        publish.mockRejectedValueOnce(new Error('Disk full'));
        await workflow.open(7, [filesystemEntry()], driver);
        await workflow.choose('computer');
        await workflow.choose('workspace');
        expect(driver.chooseDestination).toHaveBeenCalledOnce();
        expect(workflow.phase).toBe('failed');
    });

    it('allows closing an inspection and ignores its late result', async () => {
        const { workflow, driver, result } = setup();
        let finish!: (value: typeof result) => void;
        driver.inspect.mockImplementationOnce(
            () =>
                new Promise((resolve) => {
                    finish = resolve;
                }),
        );
        const opening = workflow.open(7, [filesystemEntry()], driver);
        workflow.close();
        expect(workflow.review).toBeNull();
        finish(result);
        await opening;
        expect(workflow.review).toBeNull();
    });
    it('freezes review selection and route, uses one picker, and releases the completed download', async () => {
        const { workflow, driver, result, publish } = setup();
        const entry = filesystemEntry();
        await workflow.open(7, [entry], driver);
        entry.id = 'changed';
        driver.directComputer = false;
        expect(workflow.review?.directComputer).toBe(true);
        await workflow.choose('workspace');
        expect(driver.chooseDestination).not.toHaveBeenCalled();
        await workflow.choose('computer');
        expect(driver.chooseDestination).toHaveBeenCalledOnce();
        expect(driver.execute).toHaveBeenCalledWith(
            7,
            ['folder'],
            { kind: 'DOWNLOAD', directoryName: 'Files' },
            expect.any(Function),
            'EXPORT_FOLDER',
        );
        expect(publish).toHaveBeenCalledWith(result);
        expect(driver.release).toHaveBeenCalledWith(result);
        expect(workflow.phase).toBe('completed');
    });

    it('retains direct routing on inspection failure and cancellation of the picker', async () => {
        const { workflow, driver } = setup();
        driver.inspect.mockRejectedValueOnce(new Error('Source changed'));
        await workflow.open(7, [filesystemEntry()], driver);
        expect(workflow.phase).toBe('failed');
        expect(workflow.review?.directComputer).toBe(true);
        await workflow.choose('computer');
        expect(driver.chooseDestination).not.toHaveBeenCalled();
        workflow.close();
        await workflow.open(7, [filesystemEntry()], driver);
        driver.chooseDestination.mockResolvedValueOnce(null);
        await workflow.choose('computer');
        expect(workflow.phase).toBe('ready');
        expect(driver.execute).not.toHaveBeenCalled();
    });

    it('observes an uncertain job without resubmitting or choosing another destination', async () => {
        const { workflow, driver } = setup();
        driver.execute.mockImplementationOnce(async (_rev, _ids, _destination, update) => {
            update({ jobId: 12, kind: 'images.filesystem.export', status: 'running' });
            throw new Error('Connection lost');
        });
        await workflow.open(7, [filesystemEntry()], driver);
        await workflow.choose('computer');
        expect(workflow.phase).toBe('unconfirmed');
        await workflow.choose('computer');
        expect(driver.execute).toHaveBeenCalledOnce();
        await workflow.checkStatus();
        expect(driver.observe).toHaveBeenCalledWith(12, expect.any(Function));
        expect(driver.chooseDestination).toHaveBeenCalledOnce();
        expect(workflow.phase).toBe('completed');
    });

    it('retains a failed local save for explicit re-selection without rebuilding the export', async () => {
        const { workflow, driver, publish } = setup();
        publish.mockRejectedValueOnce(new Error('Disk full'));
        await workflow.open(7, [filesystemEntry()], driver);
        await workflow.choose('computer');
        expect(workflow.phase).toBe('failed');
        expect(driver.release).not.toHaveBeenCalled();
        expect(workflow.message).toContain('Disk full');
        await workflow.choose('computer');
        expect(driver.execute).toHaveBeenCalledOnce();
        expect(driver.chooseDestination).toHaveBeenCalledTimes(2);
        expect(publish).toHaveBeenCalledTimes(2);
        expect(driver.release).toHaveBeenCalledOnce();
    });

    it('cancels a running job, retaining the dialog until its terminal result', async () => {
        const { workflow, driver } = setup();
        let finish!: (job: JobState) => void;
        driver.execute.mockImplementationOnce(async (_rev, _ids, _destination, update) => {
            update({ jobId: 12, kind: 'images.filesystem.export', status: 'running' });
            return new Promise((resolve) => {
                finish = resolve;
            });
        });
        await workflow.open(7, [filesystemEntry()], driver);
        const exporting = workflow.choose('computer');
        await vi.waitFor(() => expect(workflow.jobId).toBe(12));
        await workflow.cancel();
        expect(driver.cancel).toHaveBeenCalledWith(12);
        workflow.close();
        expect(workflow.review).not.toBeNull();
        finish({ jobId: 12, kind: 'images.filesystem.export', status: 'cancelled' });
        await exporting;
        expect(workflow.phase).toBe('failed');
        expect(workflow.message).toBe('Cancelled');
    });

    it('releases a completed archive arriving after disposal without saving it', async () => {
        const { workflow, driver, completed, result, publish } = setup();
        driver.execute.mockImplementationOnce(async () => {
            workflow.dispose();
            return completed;
        });
        await workflow.open(7, [filesystemEntry()], driver);
        await workflow.choose('computer');
        expect(publish).not.toHaveBeenCalled();
        expect(driver.release).toHaveBeenCalledWith(result);
    });

    it('rejects protected selections, empty selections and invalid revisions', async () => {
        const { workflow, driver } = setup();
        await workflow.open(0, [filesystemEntry()], driver);
        await workflow.open(7, [], driver);
        await workflow.open(7, [filesystemEntry({ filesystemMetadata: true })], driver);
        expect(workflow.review).toBeNull();
        expect(driver.inspect).not.toHaveBeenCalled();
    });
});
