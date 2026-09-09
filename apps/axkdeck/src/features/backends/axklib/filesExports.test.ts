import { describe, expect, it, vi } from 'vitest';
import { JobController } from '../../jobs/actions';
import { bindFilesystemExports } from './filesExports';
import type { JobState } from '../../../lib/transport';

function setup() {
    let active: number | null = 3;
    const queued: JobState = { jobId: 12, kind: 'images.filesystem.export', status: 'queued' };
    const completed: JobState = { ...queued, status: 'completed' };
    const inspection = { imageId: 'image', revision: 7, rootDirectory: null, entries: [], notices: [], totalBytes: 0 };
    const transport = {
        inspectFilesystemExport: vi.fn().mockResolvedValue(inspection),
        startFilesystemExport: vi.fn().mockResolvedValue(queued),
        jobStatus: vi.fn().mockResolvedValue(queued),
        cancelJob: vi.fn().mockResolvedValue(undefined),
        waitForJob: vi.fn().mockResolvedValue(completed),
    };
    const driver = bindFilesystemExports({ transport, jobs: new JobController(transport), sessionId: () => active }, 3);
    return {
        driver,
        transport,
        inspection,
        queued,
        completed,
        changeImage: () => {
            active = null;
        },
    };
}

describe('Files export binding', () => {
    it('uses the reviewed revision, session and complete selection for inspection and execution', async () => {
        const { driver, transport, inspection, queued, completed } = setup();
        const entries = ['directory', 'empty'];
        expect(await driver.inspect(7, entries)).toEqual(inspection);
        expect(transport.inspectFilesystemExport).toHaveBeenCalledWith(3, 7, entries, 'SELECTED_ENTRIES');
        const destination = { kind: 'WORKSPACE', output: { rootId: 'host', relativePath: 'Export' } } as const;
        const update = vi.fn();
        expect(await driver.execute(7, entries, destination, update)).toEqual(completed);
        expect(transport.startFilesystemExport).toHaveBeenCalledWith(3, 7, entries, destination, 'SELECTED_ENTRIES');
        expect(update).toHaveBeenCalledWith(queued);
        expect(transport.waitForJob).toHaveBeenCalledWith(12, update);
    });

    it('rejects inspection and new submission after the bound image closes', async () => {
        const { driver, transport, changeImage } = setup();
        changeImage();
        await expect(driver.inspect(7, ['file'])).rejects.toThrow('no longer open');
        await expect(
            driver.execute(7, ['file'], { kind: 'DOWNLOAD', directoryName: 'Files' }, vi.fn()),
        ).rejects.toThrow('no longer open');
        expect(transport.inspectFilesystemExport).not.toHaveBeenCalled();
        expect(transport.startFilesystemExport).not.toHaveBeenCalled();
    });

    it('discards an inspection that returns after the bound image closes', async () => {
        const { driver, transport, inspection, changeImage } = setup();
        transport.inspectFilesystemExport.mockImplementation(async () => {
            changeImage();
            return inspection;
        });
        await expect(driver.inspect(7, ['file'])).rejects.toThrow('no longer open');
    });

    it('observes and cancels the existing job independently of the current image', async () => {
        const { driver, transport, completed, changeImage } = setup();
        changeImage();
        const update = vi.fn();
        expect(await driver.observe(12, update)).toEqual(completed);
        expect(transport.jobStatus).toHaveBeenCalledWith(12);
        expect(transport.startFilesystemExport).not.toHaveBeenCalled();
        await driver.cancel(12);
        expect(transport.cancelJob).toHaveBeenCalledWith(12);
    });
});
