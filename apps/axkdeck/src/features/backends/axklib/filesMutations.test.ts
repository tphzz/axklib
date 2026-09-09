import { describe, expect, it, vi } from 'vitest';
import { JobController } from '../../jobs/actions';
import { bindFilesystemMutations } from './filesMutations';
import type { JobState } from '../../../lib/transport';

const job: JobState = { jobId: 1, kind: 'images.filesystem.edit', status: 'completed' };

describe('Files mutation binding', () => {
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
