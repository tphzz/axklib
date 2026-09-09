import { describe, expect, it, vi } from 'vitest';
import { JobController } from '../../jobs/actions';
import { bindFilesystemImports } from './filesImports';
import { serverFileLocation } from '../../../lib/storageLocations';
import type { JobState } from '../../../lib/transport';
import type { FilesystemImportEntry } from '../../../lib/filesystem';

function setup() {
    let active: number | null = 3;
    const queued: JobState = { jobId: 12, kind: 'images.filesystem.import.inspect', status: 'queued' };
    const completed: JobState = { ...queued, status: 'completed', result: { entries: [] } };
    const transport = {
        startFilesystemInputInspection: vi.fn().mockResolvedValue(queued),
        startFilesystemImportInspection: vi.fn().mockResolvedValue(queued),
        jobStatus: vi.fn().mockResolvedValue(queued),
        cancelJob: vi.fn().mockResolvedValue(undefined),
        waitForJob: vi.fn().mockResolvedValue(completed),
    };
    const driver = bindFilesystemImports({ transport, jobs: new JobController(transport), sessionId: () => active }, 3);
    return {
        driver,
        transport,
        queued,
        completed,
        changeImage: () => {
            active = null;
        },
    };
}

const entries: FilesystemImportEntry[] = [
    { relativePath: ['Folder'], directory: true, sizeBytes: 0, conflict: 'SKIP' },
    { relativePath: ['Folder', 'sample'], directory: false, sizeBytes: 100, conflict: 'REPLACE' },
];
const inputs = [serverFileLocation({ rootId: 'disk', relativePath: 'sample' })];

describe('Files import binding', () => {
    it('inspects inputs and the explicit reviewed destination through observable jobs', async () => {
        const { driver, transport, queued, completed } = setup();
        const update = vi.fn();
        expect(await driver.inspectInputs(inputs, update)).toEqual(completed);
        expect(transport.startFilesystemInputInspection).toHaveBeenCalledWith(inputs);
        expect(await driver.inspectDestination(7, 'directory', entries, update)).toEqual(completed);
        expect(transport.startFilesystemImportInspection).toHaveBeenCalledWith(3, 7, 'directory', entries);
        expect(update).toHaveBeenCalledWith(queued);
        expect(transport.waitForJob).toHaveBeenCalledWith(12, update);
    });

    it('rejects both new inspections when the bound image is no longer open', async () => {
        const { driver, transport, changeImage } = setup();
        changeImage();
        await expect(driver.inspectInputs(inputs, vi.fn())).rejects.toThrow('no longer open');
        await expect(driver.inspectDestination(7, 'directory', entries, vi.fn())).rejects.toThrow('no longer open');
        expect(transport.startFilesystemInputInspection).not.toHaveBeenCalled();
        expect(transport.startFilesystemImportInspection).not.toHaveBeenCalled();
    });

    it.each(['inputs', 'destination'] as const)('discards a late %s review after navigation', async (kind) => {
        const { driver, transport, completed, changeImage } = setup();
        transport.waitForJob.mockImplementation(async () => {
            changeImage();
            return completed;
        });
        const pending =
            kind === 'inputs'
                ? driver.inspectInputs(inputs, vi.fn())
                : driver.inspectDestination(7, 'directory', entries, vi.fn());
        await expect(pending).rejects.toThrow('no longer open');
    });

    it('cancels an inspection accepted after the image closes without starting observation', async () => {
        const { driver, transport, queued, changeImage } = setup();
        transport.startFilesystemImportInspection.mockImplementation(async () => {
            changeImage();
            return queued;
        });
        await expect(driver.inspectDestination(7, 'directory', entries, vi.fn())).rejects.toThrow('no longer open');
        expect(transport.cancelJob).toHaveBeenCalledWith(12);
        expect(transport.waitForJob).not.toHaveBeenCalled();
    });

    it('continues observing and cancelling a known job after navigation without resubmitting', async () => {
        const { driver, transport, completed, changeImage } = setup();
        changeImage();
        expect(await driver.observe(12, vi.fn())).toEqual(completed);
        expect(transport.jobStatus).toHaveBeenCalledWith(12);
        await driver.cancel(12);
        expect(transport.cancelJob).toHaveBeenCalledWith(12);
        expect(transport.startFilesystemInputInspection).not.toHaveBeenCalled();
        expect(transport.startFilesystemImportInspection).not.toHaveBeenCalled();
    });

    it.each(['failed', 'cancelled'] as const)('preserves a %s result for the review lifecycle', async (status) => {
        const { driver, transport, completed } = setup();
        const terminal = { ...completed, status, error: 'Inspection stopped' };
        transport.waitForJob.mockResolvedValue(terminal);
        expect(await driver.inspectDestination(7, 'directory', entries, vi.fn())).toEqual(terminal);
    });
});
