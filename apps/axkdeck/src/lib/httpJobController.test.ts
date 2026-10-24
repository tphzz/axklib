import { describe, expect, it, vi } from 'vitest';

import { AxklibApiError, type ApiJobEvent, type ApiJobSnapshot, type AxklibHttpApiClient } from './httpApiClient';
import { HttpJobController } from './httpJobController';

function snapshot(state: ApiJobSnapshot['state'], latestSequence: number): ApiJobSnapshot {
    return {
        jobId: 'job-import',
        operationId: 'images.alter',
        state,
        latestSequence,
        progress:
            state === 'COMPLETED' ? { phase: 'commit', completed: 100, total: 100, message: 'Imported audio' } : null,
        result: state === 'COMPLETED' ? { revision: 2 } : null,
        error: null,
    };
}

describe('HttpJobController', () => {
    it('sends cancellation while event connection setup is unresolved and reconciles its terminal result', async () => {
        let cancelled = false;
        const request = vi.fn(async (method: string) => {
            if (method === 'DELETE') cancelled = true;
            return snapshot(cancelled ? 'CANCELLED' : 'RUNNING', cancelled ? 3 : 2);
        });
        const close = vi.fn();
        const connectEvents = vi.fn().mockResolvedValue({ opened: new Promise<void>(() => {}), close });
        const controller = new HttpJobController({
            request,
            connectEvents,
            replayJobEvents: async () => ({ events: [] }),
            deviceWrites: { terminal: vi.fn() },
        } as unknown as AxklibHttpApiClient);
        const submitted = controller.map(snapshot('QUEUED', 1));
        const abort = new AbortController();
        const pending = controller.wait(submitted.jobId, () => {}, abort.signal);
        await vi.waitFor(() => expect(connectEvents).toHaveBeenCalledOnce());
        abort.abort();
        await expect(pending).resolves.toMatchObject({ status: 'cancelled' });
        expect(request).toHaveBeenCalledWith('DELETE', '/jobs/job-import');
        expect(close).toHaveBeenCalledOnce();
    });
    it('recovers an expired replay cursor from the current snapshot and keeps monitoring', async () => {
        const completedEvent: ApiJobEvent = {
            schemaVersion: '1',
            eventId: 'event-71',
            sequence: 71,
            jobId: 'job-import',
            operationId: 'images.alter',
            type: 'state',
            timestampUnixMs: 1,
            state: 'COMPLETED',
            progress: { phase: 'commit', completed: 100, total: 100, message: 'Imported audio' },
            jobUrl: '/api/v1/jobs/job-import',
        };
        const replayJobEvents = vi
            .fn()
            .mockRejectedValueOnce(
                new AxklibApiError('job_event_replay_expired', 'requested job events are no longer retained', 409),
            )
            .mockResolvedValueOnce({ events: [completedEvent] });
        const request = vi
            .fn()
            .mockResolvedValueOnce(snapshot('RUNNING', 70))
            .mockResolvedValueOnce(snapshot('COMPLETED', 71));
        const close = vi.fn();
        const connectEvents = vi.fn().mockResolvedValue({ opened: Promise.resolve(), close });
        const terminal = vi.fn();
        const client = {
            replayJobEvents,
            request,
            connectEvents,
            deviceWrites: { terminal },
        } as unknown as AxklibHttpApiClient;
        const controller = new HttpJobController(client);
        const submitted = controller.map(snapshot('QUEUED', 1));
        const updates: string[] = [];

        const completed = await controller.wait(submitted.jobId, (update) => updates.push(update.status));

        expect(completed).toMatchObject({ status: 'completed', result: { revision: 2 } });
        expect(replayJobEvents).toHaveBeenNthCalledWith(1, 'job-import', 0);
        expect(replayJobEvents).toHaveBeenNthCalledWith(2, 'job-import', 70);
        expect(updates).toContain('running');
        expect(updates.at(-1)).toBe('completed');
        expect(close).toHaveBeenCalledOnce();
        expect(terminal).toHaveBeenCalledWith('job-import');
    });
});
