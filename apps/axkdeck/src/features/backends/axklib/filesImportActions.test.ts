import { describe, expect, it, vi } from 'vitest';
import { bindFilesystemImportActions } from './filesImportActions';
import { JobController } from '../../jobs/actions';
import { PickerController } from '../../dialogs/picker';
import { clientUploadLocation, serverFileLocation, serverDirectoryLocation } from '../../../lib/storageLocations';
import { browserUploadSource } from '../../../lib/clientUploadSource';

function setup() {
    let active: number | null = 3;
    const upload = clientUploadLocation({ uploadId: 'upload' }, 'FILE', 'empty');
    const transport = {
        supportsClientUploads: true,
        startFilesystemInputInspection: vi.fn(),
        startFilesystemImageInspection: vi.fn(),
        releaseFilesystemImageInspection: vi.fn().mockResolvedValue(undefined),
        startFilesystemImportInspection: vi.fn(),
        waitForJob: vi.fn(),
        jobStatus: vi.fn(),
        cancelJob: vi.fn(),
        uploadClientFile: vi.fn().mockResolvedValue(upload),
        releaseClientUpload: vi.fn().mockResolvedValue(undefined),
        sandboxDirectory: vi
            .fn()
            .mockImplementation(async (directory) => ({ directory, entries: [], truncated: false, nextCursor: null })),
    };
    const picker = new PickerController(() => undefined);
    const chooseDirectory = vi
        .spyOn(picker, 'chooseLocation')
        .mockResolvedValue(serverDirectoryLocation({ rootId: 'host', relativePath: 'source' }));
    const choose = vi
        .spyOn(picker, 'chooseFiles')
        .mockResolvedValue([serverFileLocation({ rootId: 'host', relativePath: 'empty' })]);
    const actions = bindFilesystemImportActions(
        { transport, picker, jobs: new JobController(transport), sessionId: () => active },
        3,
    );
    return {
        actions,
        transport,
        choose,
        chooseDirectory,
        upload,
        close: () => {
            active = null;
        },
    };
}

describe('Files import source acquisition', () => {
    it('chooses readable directory contents through the shared picker and preserves empty directories', async () => {
        const { actions, transport, chooseDirectory } = setup();
        transport.sandboxDirectory.mockImplementation(async (directory) => ({
            directory,
            entries:
                directory.relativePath === 'source'
                    ? [{ name: 'empty', relativePath: 'source/empty', kind: 'DIRECTORY', size: null }]
                    : [],
            truncated: false,
            nextCursor: null,
        }));
        const result = await actions.chooseDirectory(new AbortController().signal, vi.fn());
        expect(result).toEqual([{ relativePath: ['empty'], directory: true }]);
        expect(chooseDirectory).toHaveBeenCalledWith(
            'directory',
            'Import from disk',
            [],
            '',
            expect.objectContaining({ parentDialog: 'filesystem-import' }),
        );
        expect(transport.uploadClientFile).not.toHaveBeenCalled();
    });
    it('does not enumerate after a cancelled, aborted or stale directory picker', async () => {
        const test = setup();
        test.chooseDirectory.mockResolvedValueOnce(null);
        expect(await test.actions.chooseDirectory(new AbortController().signal, vi.fn())).toBeNull();
        expect(test.transport.sandboxDirectory).not.toHaveBeenCalled();
        const abort = new AbortController();
        test.chooseDirectory.mockImplementationOnce(async () => {
            abort.abort();
            return serverDirectoryLocation({ rootId: 'host', relativePath: 'source' });
        });
        await expect(test.actions.chooseDirectory(abort.signal, vi.fn())).rejects.toThrow();
        expect(test.transport.sandboxDirectory).not.toHaveBeenCalled();
        test.chooseDirectory.mockImplementationOnce(async () => {
            test.close();
            return serverDirectoryLocation({ rootId: 'host', relativePath: 'source' });
        });
        await expect(test.actions.chooseDirectory(new AbortController().signal, vi.fn())).rejects.toThrow(
            'no longer open',
        );
    });
    it('uses the shared batch picker without extension restrictions', async () => {
        const { actions, choose } = setup();
        await actions.chooseFiles();
        expect(choose).toHaveBeenCalledWith(
            'Add files',
            [],
            expect.objectContaining({ parentDialog: 'filesystem-import' }),
        );
    });
    it('stages empty and extensionless files as FILE and releases only uploads', async () => {
        const { actions, transport, upload } = setup();
        const file = browserUploadSource(new File([], 'empty'));
        const abort = new AbortController();
        expect(await actions.upload([file], abort.signal, vi.fn())).toEqual([upload]);
        expect(transport.uploadClientFile).toHaveBeenCalledWith(file, 'FILE', expect.any(Function), abort.signal);
        await actions.release([upload, serverFileLocation({ rootId: 'host', relativePath: 'kept' })]);
        expect(transport.releaseClientUpload).toHaveBeenCalledExactlyOnceWith(upload);
    });
    it('cleans the completed uploads when a later upload fails', async () => {
        const { actions, transport, upload } = setup();
        transport.uploadClientFile.mockResolvedValueOnce(upload).mockRejectedValueOnce(new Error('Quota exceeded'));
        const files = [browserUploadSource(new File([], 'one')), browserUploadSource(new File([], 'two'))];
        await expect(actions.upload(files, new AbortController().signal, vi.fn())).rejects.toThrow('Quota exceeded');
        expect(transport.releaseClientUpload).toHaveBeenCalledWith(upload);
    });
    it('rejects a stale picker result and cleans an upload accepted after navigation', async () => {
        const { actions, transport, choose, upload, close } = setup();
        choose.mockImplementationOnce(async () => {
            close();
            return [];
        });
        await expect(actions.chooseFiles()).rejects.toThrow('no longer open');
        const next = setup();
        next.transport.uploadClientFile.mockImplementationOnce(async () => {
            next.close();
            return next.upload;
        });
        await expect(
            next.actions.upload([browserUploadSource(new File([], 'one'))], new AbortController().signal, vi.fn()),
        ).rejects.toThrow('no longer open');
        expect(next.transport.releaseClientUpload).toHaveBeenCalledWith(next.upload);
        expect(transport.releaseClientUpload).not.toHaveBeenCalledWith(upload);
    });
});
