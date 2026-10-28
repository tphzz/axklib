import { beforeEach, describe, expect, it, vi } from 'vitest';
import { HttpDeviceFormatting } from './httpDeviceFormatting';
import { DeviceWriteTracking } from './deviceWriteTracking';
import { HttpJobController } from './httpJobController';
import type { AxklibHttpApiClient } from './httpApiClient';
import { AxklibApiError } from './httpErrors';
import { deviceFormatInspection } from '../test/deviceFormatFixture';

beforeEach(() => localStorage.clear());
function fixture() {
    const deviceWrites = new DeviceWriteTracking(localStorage);
    const inspection = deviceFormatInspection();
    const invoke = vi.fn(async (operation: string) => ({
        jobId: 'server-job',
        operationId: operation,
        state: 'QUEUED',
        latestSequence: 1,
    }));
    const client = { deviceWrites, invoke } as unknown as AxklibHttpApiClient;
    const jobs = new HttpJobController(client);
    const service = new HttpDeviceFormatting(client, jobs);
    const input = {
        sessionId: inspection.sessionId,
        planToken: 'plan',
        confirmationText: inspection.confirmationText,
        dataLossAcknowledged: true as const,
        exclusiveUseAcknowledged: true as const,
    };
    return { service, client, invoke, deviceWrites, input, inspection, jobs };
}
describe('physical initialization submission', () => {
    it('persists target ownership before POST without requiring an imageId', async () => {
        const f = fixture();
        f.invoke.mockImplementation(async (operation) => {
            expect(new DeviceWriteTracking(localStorage).pending()[0]?.operationId).toBe('known-operation');
            return { jobId: 'server-job', operationId: operation, state: 'QUEUED', latestSequence: 1 };
        });
        const job = await f.service.start(f.input, 'known-operation', 'Selected carrier / SCSI 0');
        expect(job.serverJobId).toBe('server-job');
        expect(f.invoke).toHaveBeenCalledExactlyOnceWith('devices.sfs.format', f.input, {
            idempotencyKey: 'known-operation',
        });
        expect(f.deviceWrites.pending()[0]?.jobId).toBe('server-job');
    });
    it('never sends the request when durable target tracking is unavailable', async () => {
        const f = fixture();
        Object.assign(f.client, { deviceWrites: new DeviceWriteTracking() });
        await expect(f.service.start(f.input, 'known-operation', 'Carrier')).rejects.toThrow('unavailable');
        expect(f.invoke).not.toHaveBeenCalled();
    });
    it('retains a lost acknowledgement but clears a definite server refusal', async () => {
        const f = fixture();
        f.invoke.mockRejectedValueOnce(new Error('Disconnected'));
        await expect(f.service.start(f.input, 'known-operation', 'Carrier')).rejects.toThrow('Disconnected');
        expect(f.deviceWrites.pending()[0]?.operationId).toBe('known-operation');
        f.deviceWrites.reconcile('known-operation');
        f.invoke.mockRejectedValueOnce(new AxklibApiError('device_busy', 'Mounted', 422));
        await expect(f.service.start(f.input, 'new-operation', 'Carrier')).rejects.toThrow('Mounted');
        expect(f.deviceWrites.pending()).toHaveLength(0);
    });
    it('does not create write tracking for read-only inspection jobs', async () => {
        const f = fixture();
        f.invoke.mockResolvedValueOnce({
            jobId: 'inspection',
            operationId: 'devices.inspect',
            state: 'COMPLETED',
            latestSequence: 2,
            result: f.inspection,
        } as never);
        expect(await f.service.inspect('format-session', 'disk')).toEqual(f.inspection);
        expect(f.deviceWrites.pending()).toHaveLength(0);
    });
    it('returns the acknowledged completed job before querying its durable status', async () => {
        const f = fixture();
        f.invoke.mockResolvedValueOnce({
            jobId: 'server-job',
            operationId: 'devices.sfs.format',
            state: 'COMPLETED',
            latestSequence: 2,
        } as never);
        expect((await f.service.start(f.input, 'known-operation', 'Carrier')).status).toBe('completed');
        expect(f.invoke).toHaveBeenCalledOnce();
        expect(f.deviceWrites.pending()[0]?.jobId).toBe('server-job');
    });
    it('retains terminal failed jobs until durable status confirms restoration or no write', async () => {
        const f = fixture();
        const job = await f.service.start(f.input, 'known-operation', 'Carrier');
        vi.spyOn(f.jobs, 'wait').mockImplementationOnce(async () => {
            f.deviceWrites.terminal('server-job');
            return { ...job, status: 'failed' };
        });
        expect((await f.service.wait(job.jobId, () => {})).status).toBe('failed');
        expect(f.deviceWrites.pending()).toHaveLength(1);
        f.invoke.mockResolvedValueOnce({
            operationId: 'known-operation',
            kind: 'FORMAT',
            state: 'RECOVERY_REQUIRED',
        } as never);
        await f.service.resolve('known-operation');
        expect(f.deviceWrites.pending()).toHaveLength(1);
        f.invoke.mockResolvedValueOnce({
            operationId: 'known-operation',
            kind: 'FORMAT',
            state: 'ROLLED_BACK',
        } as never);
        await f.service.resolve('known-operation');
        expect(f.deviceWrites.pending()).toHaveLength(0);
    });
    it('does not trust a completed job without its matching durable commit', async () => {
        const f = fixture();
        const job = await f.service.start(f.input, 'known-operation', 'Carrier');
        vi.spyOn(f.jobs, 'status').mockResolvedValue({ ...job, status: 'completed' });
        f.invoke.mockResolvedValueOnce({
            operationId: 'another-operation',
            kind: 'FORMAT',
            state: 'COMMITTED',
        } as never);
        await expect(f.service.status(job.jobId)).rejects.toThrow('identity');
        expect(f.deviceWrites.pending()).toHaveLength(1);
        f.invoke.mockResolvedValueOnce({
            operationId: 'known-operation',
            kind: 'FORMAT',
            state: 'COMMITTED',
        } as never);
        expect((await f.service.status(job.jobId)).status).toBe('completed');
        expect(f.deviceWrites.pending()).toHaveLength(0);
    });
});
