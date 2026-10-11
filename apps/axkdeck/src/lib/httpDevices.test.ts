import { beforeEach, describe, expect, it, vi } from 'vitest';
import { HttpDevices } from './httpDevices';
import { DeviceWriteTracking } from './deviceWriteTracking';
import { AxklibApiError, type AxklibHttpApiClient } from './httpApiClient';
import { HttpJobController } from './httpJobController';

beforeEach(() => localStorage.clear());
function setup(state = 'NO_WRITE') {
    const deviceWrites = new DeviceWriteTracking(localStorage);
    deviceWrites.bind('image', {
        kind: 'device',
        reference: { sessionId: 'owned', targetId: 'disk' },
        displayName: 'Test',
    });
    deviceWrites.begin('image', 'images.alter', 'known-request');
    const operation = {
        operationId: 'known-request',
        state,
        model: 'Test',
        serial: '1',
        targetLabel: 'Disk',
        targetSize: 1024,
    };
    const invoke = vi.fn(async (name: string) =>
        name === 'devices.resolve' ? operation : { operations: [operation] },
    );
    const request = vi.fn();
    const client = { invoke, request, deviceWrites } as unknown as AxklibHttpApiClient;
    return { service: new HttpDevices(client, new HttpJobController(client)), deviceWrites, invoke, request };
}
describe('device status reconciliation', () => {
    it('fences an unacknowledged request before accepting a no-write outcome', async () => {
        const { service, invoke, deviceWrites } = setup();
        await service.operations();
        expect(invoke).toHaveBeenCalledWith('devices.resolve', { operationId: 'known-request' });
        expect(deviceWrites.pending()).toHaveLength(0);
    });
    it('keeps a prepared operation unresolved and never resubmits it', async () => {
        const { service, invoke, deviceWrites } = setup('PREPARED');
        const result = await service.operations();
        expect(result.uncertain).toHaveLength(1);
        expect(deviceWrites.pending()).toHaveLength(1);
        expect(invoke.mock.calls.map(([operation]) => operation)).toEqual(['devices.operations', 'devices.resolve']);
    });
    it('uses durable status when an acknowledged job is no longer retained', async () => {
        const { service, request, deviceWrites } = setup('COMMITTED');
        deviceWrites.acknowledge('known-request', 'old-job');
        request.mockRejectedValue(new AxklibApiError('job_not_found', 'expired', 404));
        await service.operations();
        expect(deviceWrites.pending()).toHaveLength(0);
    });
    it('does not infer a terminal outcome from a failed status request', async () => {
        const { service, request, deviceWrites } = setup('COMMITTED');
        deviceWrites.acknowledge('known-request', 'job');
        request.mockRejectedValue(new Error('Offline'));
        await expect(service.operations()).rejects.toThrow('Offline');
        expect(deviceWrites.pending()).toHaveLength(1);
    });
});

describe('device XML inspection', () => {
    it('requires all local device operations without listing or connecting devices', async () => {
        const operations = ['devices.list', 'devices.connect', 'devices.xml.inspect'].map((id) => ({
            id,
            implemented: true,
        }));
        const serverCapabilities = vi.fn(async () => ({ operations }));
        const invoke = vi.fn();
        const client = { serverCapabilities, invoke } as unknown as AxklibHttpApiClient;
        const service = new HttpDevices(client, new HttpJobController(client));
        expect(await service.available()).toBe(true);
        operations[2]!.implemented = false;
        expect(await service.available()).toBe(false);
        operations.pop();
        expect(await service.available()).toBe(false);
        expect(invoke).not.toHaveBeenCalled();
    });
    it('routes validation through the native read operation, not a connection job', async () => {
        const result = { targets: [] };
        const invoke = vi.fn(async () => result);
        const client = { invoke } as unknown as AxklibHttpApiClient;
        const service = new HttpDevices(client, new HttpJobController(client));
        expect(await service.inspectXml('<SCSI2SD/>')).toBe(result);
        expect(invoke).toHaveBeenCalledExactlyOnceWith('devices.xml.inspect', { xml: '<SCSI2SD/>' });
    });
});
