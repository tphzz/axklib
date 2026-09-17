import { describe, expect, it, vi } from 'vitest';
import type { JobState } from '../../lib/transport';
import { JobController } from '../jobs/actions';
import { ImportCompletion } from './importCompletion.svelte';
import { AxklibApiError } from '../../lib/httpErrors';

const completed = { jobId: 7, status: 'completed' } as JobState;

describe('ImportCompletion', () => {
    it('retains new completion warnings and clipping but not previously reviewed conversion notices', async () => {
        const result = {
            kind: 'ALTERATION',
            warnings: [{ message: 'Converted to PCM16' }, { message: 'New warning' }],
            operations: [{ objectName: 'Bass', audioImport: { clippedSamples: 2 } }],
        };
        const transport = { waitForJob: vi.fn().mockResolvedValue({ ...completed, result }), cancelJob: vi.fn() };
        const state = new ImportCompletion(transport, new JobController(transport));
        const finish = vi.fn(async () => {
            expect(state.warnings).toEqual([
                'New warning',
                'Bass: 2 audio sample values exceeded full scale during conversion',
            ]);
        });
        expect(await state.run(async () => completed, finish, vi.fn(), ['Converted to PCM16'])).toBe(false);
        expect(state.phase).toBe('warnings');
        expect(state.canDismiss).toBe(true);
        expect(state.locked).toBe(true);
        expect(await state.recover()).toBe(false);
        expect(finish).toHaveBeenCalledTimes(1);
    });
    it('does not treat a submission timeout as confirmation that nothing was written', async () => {
        const transport = { waitForJob: vi.fn(), cancelJob: vi.fn() };
        const state = new ImportCompletion(transport, new JobController(transport));
        expect(
            await state.run(vi.fn().mockRejectedValue(new AxklibApiError('timeout', 'Timed out', 408)), vi.fn()),
        ).toBe(false);
        expect(state.phase).toBe('unconfirmed');
        expect(state.canCheck).toBe(false);
    });
    it('retries refresh without submitting a committed import again', async () => {
        const waitForJob = vi.fn().mockResolvedValue(completed);
        const start = vi.fn().mockResolvedValue({ jobId: 7, status: 'queued' });
        const finish = vi.fn().mockRejectedValueOnce(new Error('Refresh unavailable')).mockResolvedValue(undefined);
        const transport = { waitForJob, cancelJob: vi.fn() };
        const state = new ImportCompletion(transport, new JobController(transport));
        expect(await state.run(start, finish)).toBe(false);
        expect(state.phase).toBe('refresh-failed');
        expect(state.locked).toBe(true);
        expect(await state.run(start, finish)).toBe(false);
        expect(await state.recover()).toBe(true);
        expect(start).toHaveBeenCalledTimes(1);
        expect(finish).toHaveBeenCalledTimes(2);
    });

    it('observes the acknowledged job after a connection loss instead of resubmitting', async () => {
        const waitForJob = vi.fn().mockRejectedValueOnce(new Error('Disconnected')).mockResolvedValue(completed);
        const transport = { waitForJob, cancelJob: vi.fn() };
        const state = new ImportCompletion(transport, new JobController(transport));
        const start = vi.fn().mockResolvedValue({ jobId: 7, status: 'queued' });
        const finish = vi.fn().mockResolvedValue(undefined);
        expect(await state.run(start, finish)).toBe(false);
        expect(state.phase).toBe('unconfirmed');
        expect(state.canCheck).toBe(true);
        expect(await state.recover()).toBe(true);
        expect(start).toHaveBeenCalledTimes(1);
        expect(finish).toHaveBeenCalledTimes(1);
    });

    it('does not retry an unacknowledged submission', async () => {
        const transport = { waitForJob: vi.fn(), cancelJob: vi.fn() };
        const state = new ImportCompletion(transport, new JobController(transport));
        const start = vi.fn().mockRejectedValue(new Error('Disconnected'));
        expect(await state.run(start, vi.fn())).toBe(false);
        expect(state.phase).toBe('unconfirmed');
        expect(state.canCheck).toBe(false);
        expect(await state.recover()).toBe(false);
        expect(await state.run(start, vi.fn())).toBe(false);
        expect(start).toHaveBeenCalledTimes(1);
    });

    it('unlocks a confirmed failed job without running completion', async () => {
        const transport = {
            waitForJob: vi.fn().mockResolvedValue({ jobId: 7, status: 'failed', error: 'Rejected' }),
            cancelJob: vi.fn(),
        };
        const state = new ImportCompletion(transport, new JobController(transport));
        const finish = vi.fn();
        expect(await state.run(async () => completed, finish)).toBe(false);
        expect(state.phase).toBe('idle');
        expect(state.message).toBe('Rejected');
        expect(finish).not.toHaveBeenCalled();
    });
});
