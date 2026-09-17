import { describe, it, expect, vi } from 'vitest';
import { FilesDragWorkflow } from './dragWorkflow';
import type { FilesystemExportActions, FilesystemExportResult } from '../../lib/filesystemExport';
import { filesystemEntry } from '../../lib/testing/filesystem';

function setup() {
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
            contentPath: '/api/v1/download-archives/archive/content',
        },
    };
    const driver = {
        inspect: vi.fn().mockResolvedValue(result),
        execute: vi.fn().mockResolvedValue({ jobId: 12, status: 'completed', result }),
        observe: vi.fn(),
        cancel: vi.fn().mockResolvedValue(undefined),
        release: vi.fn().mockResolvedValue(undefined),
        chooseDestination: vi.fn(),
        directComputer: true,
        desktop: true,
        drag: {
            reserve: vi.fn().mockResolvedValue('ticket'),
            prepare: vi.fn().mockResolvedValue(undefined),
            start: vi.fn().mockResolvedValue(undefined),
            cancel: vi.fn().mockResolvedValue(undefined),
        },
    } satisfies FilesystemExportActions;
    const status = vi.fn();
    const errors = vi.fn();
    const flow = new FilesDragWorkflow(status, errors);
    return { flow, driver, status, errors, result };
}

describe('Files native drag lifecycle', () => {
    it('freezes selection, uses one download job without a picker and releases both artifacts', async () => {
        const { flow, driver, result } = setup();
        const row = filesystemEntry({ id: 'file' });
        const running = flow.start(7, [row], driver, () => true);
        row.id = 'other';
        await running;
        expect(driver.execute).toHaveBeenCalledWith(
            7,
            ['file'],
            { kind: 'DOWNLOAD', directoryName: 'Files' },
            expect.any(Function),
        );
        expect(driver.chooseDestination).not.toHaveBeenCalled();
        expect(driver.drag.prepare).toHaveBeenCalledWith('ticket', result.download!.contentPath);
        expect(driver.drag.start).toHaveBeenCalledOnce();
        expect(driver.release).toHaveBeenCalledWith(result);
        expect(driver.drag.cancel).toHaveBeenCalledWith('ticket');
    });
    it('cleans up a ticket that arrives after pointer release', async () => {
        const { flow, driver } = setup();
        driver.drag.reserve.mockImplementationOnce(async () => {
            flow.cancel();
            return 'late';
        });
        await flow.start(7, [filesystemEntry()], driver, () => true);
        expect(driver.drag.prepare).not.toHaveBeenCalled();
        expect(driver.drag.start).not.toHaveBeenCalled();
        expect(driver.drag.cancel).toHaveBeenCalledWith('late');
        expect(driver.release).toHaveBeenCalledOnce();
    });
    it('cancels a running job and never starts a drag from its late completion', async () => {
        const { flow, driver, result } = setup();
        driver.execute.mockImplementationOnce(async (_revision, _ids, _destination, update) => {
            update({ jobId: 12, kind: 'images.filesystem.export', status: 'running' });
            flow.cancel();
            return { jobId: 12, kind: 'images.filesystem.export', status: 'completed', result };
        });
        await flow.start(7, [filesystemEntry()], driver, () => true);
        expect(driver.cancel).toHaveBeenCalledWith(12);
        expect(driver.drag.reserve).not.toHaveBeenCalled();
        expect(driver.release).toHaveBeenCalledOnce();
    });
    it('rejects protected selections, stale context and exports needing review', async () => {
        const { flow, driver, result, errors } = setup();
        await flow.start(7, [filesystemEntry({ filesystemMetadata: true })], driver, () => true);
        expect(driver.inspect).not.toHaveBeenCalled();
        await flow.start(7, [filesystemEntry()], driver, () => false);
        expect(driver.execute).not.toHaveBeenCalled();
        driver.inspect.mockResolvedValueOnce({
            ...result,
            notices: [{ entryId: 'file', sourcePath: '/file', message: 'Name changed' }],
        });
        await flow.start(7, [filesystemEntry()], driver, () => true);
        expect(driver.execute).not.toHaveBeenCalled();
        expect(errors).toHaveBeenCalledWith(expect.stringContaining('review'));
    });
});
