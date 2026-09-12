import { describe, expect, it, vi } from 'vitest';
import { JobController } from '../../jobs/actions';
import { bindFilesystemMutations } from './filesMutations';
import type { JobState } from '../../../lib/transport';
import { AxklibApiError } from '../../../lib/httpErrors';
import { FilesystemWriteRejected } from '../../../lib/filesystem';

const job: JobState = { jobId: 1, kind: 'images.filesystem.edit', status: 'completed' };

describe('Files mutation binding', () => {
    it.each([400, 409, 408, 500])('classifies only definite pre-write rejection for HTTP %s', async (status) => {
        const error = new AxklibApiError('failure', 'Failure', status);
        const transport = {
            startFilesystemEdits: vi.fn().mockRejectedValue(error),
            jobStatus: vi.fn(),
            cancelJob: vi.fn(),
            waitForJob: vi.fn(),
        };
        const driver = bindFilesystemMutations(
            {
                transport,
                jobs: new JobController(transport),
                sessionId: () => 3,
                invalidateSession: async () => {},
                refreshSession: async () => {},
            },
            3,
        );
        const result = driver.execute(1, [{ kind: 'RENAME', entryId: 'file', newName: 'New' }], vi.fn());
        if (status === 400 || status === 409) await expect(result).rejects.toBeInstanceOf(FilesystemWriteRejected);
        else await expect(result).rejects.toBe(error);
    });
    it('invalidates audition, uses the captured session and revision, and refreshes shared image state', async () => {
        let active = 3;
        const transport = {
            startFilesystemEdits: vi.fn().mockResolvedValue(job),
            jobStatus: vi.fn().mockResolvedValue(job),
            cancelJob: vi.fn().mockResolvedValue(undefined),
            waitForJob: vi.fn().mockResolvedValue(job),
        };
        const invalidateSession = vi.fn(async () => expect(transport.startFilesystemEdits).not.toHaveBeenCalled());
        const refreshSession = vi.fn().mockResolvedValue(undefined);
        const driver = bindFilesystemMutations(
            {
                transport,
                jobs: new JobController(transport),
                sessionId: () => active,
                invalidateSession,
                refreshSession,
            },
            active,
        );
        const edits = [{ kind: 'DELETE' as const, entryId: 'file', recursive: false }];
        const update = vi.fn();
        await driver.execute(8, edits, update);
        expect(transport.startFilesystemEdits).toHaveBeenCalledWith(3, 8, edits);
        expect(invalidateSession).toHaveBeenCalledWith(3);
        expect(update).toHaveBeenCalledWith(job);
        await driver.refresh();
        expect(refreshSession).toHaveBeenCalledOnce();
        active = 4;
        await expect(driver.execute(8, edits, update)).rejects.toThrow('no longer open');
        await expect(driver.refresh()).rejects.toThrow('no longer open');
        expect(transport.startFilesystemEdits).toHaveBeenCalledOnce();
        await driver.observe(1, update);
        expect(transport.jobStatus).toHaveBeenCalledWith(1);
        await driver.cancel(1);
        expect(transport.cancelJob).toHaveBeenCalledWith(1);
    });
});
