import type { components } from './generated/axklibApiV1';
import type { AxklibHttpApiClient } from './httpApiClient';
import type { HttpJobController } from './httpJobController';
import { AxklibApiError } from './httpErrors';
import { randomIdempotencyKey } from './httpTransportWire';
import type { JobState } from './transport';

export type DeviceInspection = components['schemas']['DeviceInspection'];
export type DeviceFormatPlan = components['schemas']['DeviceSfsFormatPlan'];
export type DeviceFormatRequest = components['schemas']['DeviceSfsFormatRequest'];
export type DeviceFormatResult = components['schemas']['DeviceSfsFormatResult'];

export interface DeviceFormattingService {
    available(): Promise<boolean>;
    inspect(sessionId: string, targetId: string): Promise<DeviceInspection>;
    plan(sessionId: string, inspectionToken: string, partitionCount: number): Promise<DeviceFormatPlan>;
    start(input: DeviceFormatRequest, operationId: string, displayName: string): Promise<JobState>;
    wait(jobId: number, update: (job: JobState) => void): Promise<JobState>;
    status(jobId: number): Promise<JobState>;
    cancel(jobId: number): Promise<void>;
    resolve(operationId: string): Promise<components['schemas']['DeviceOperation']>;
}

export function definiteDeviceRefusal(error: unknown): boolean {
    return error instanceof AxklibApiError && error.status >= 400 && error.status < 500 && error.status !== 408;
}

export class HttpDeviceFormatting implements DeviceFormattingService {
    constructor(
        private readonly client: AxklibHttpApiClient,
        private readonly jobs: HttpJobController,
    ) {}
    async available(): Promise<boolean> {
        const capabilities = await this.client.serverCapabilities();
        return ['devices.inspect', 'devices.sfs.plan', 'devices.sfs.format'].every((id) =>
            capabilities.operations.some((entry) => entry.id === id && entry.implemented),
        );
    }
    inspect(sessionId: string, targetId: string): Promise<DeviceInspection> {
        return this.read('devices.inspect', { sessionId, targetId });
    }
    plan(sessionId: string, inspectionToken: string, partitionCount: number): Promise<DeviceFormatPlan> {
        return this.read('devices.sfs.plan', { sessionId, inspectionToken, partitionCount });
    }
    async start(input: DeviceFormatRequest, operationId: string, displayName: string): Promise<JobState> {
        this.client.deviceWrites.beginTarget(displayName, 'devices.sfs.format', operationId);
        let acknowledged = false;
        try {
            const job = await this.client.invoke('devices.sfs.format', input, { idempotencyKey: operationId });
            if (!this.jobs.isJob(job)) throw new Error('Format submission did not return its operation job');
            this.client.deviceWrites.acknowledge(operationId, job.jobId);
            acknowledged = true;
            return this.jobs.map(job);
        } catch (error) {
            if (!acknowledged && definiteDeviceRefusal(error)) this.client.deviceWrites.reconcile(operationId);
            throw error;
        }
    }
    async wait(jobId: number, update: (job: JobState) => void): Promise<JobState> {
        return this.confirmCompleted(await this.jobs.wait(jobId, update));
    }
    async status(jobId: number): Promise<JobState> {
        return this.confirmCompleted(await this.jobs.status(jobId));
    }
    cancel(jobId: number): Promise<void> {
        return this.jobs.cancel(jobId);
    }
    async resolve(operationId: string): Promise<components['schemas']['DeviceOperation']> {
        const result = await this.client.invoke<components['schemas']['DeviceOperation']>('devices.resolve', {
            operationId,
        });
        if (this.jobs.isJob(result)) throw new Error('Unexpected asynchronous operation status');
        if (result.operationId !== operationId || !['FORMAT', 'UNSTARTED'].includes(result.kind))
            throw new Error('Device operation identity does not match this format request');
        if (['NO_WRITE', 'COMMITTED', 'ROLLED_BACK'].includes(result.state))
            this.client.deviceWrites.reconcile(operationId);
        return result;
    }
    private async confirmCompleted(job: JobState): Promise<JobState> {
        if (job.status !== 'completed') return job;
        const retained = this.client.deviceWrites
            .pending()
            .find((record) => record.jobId === job.serverJobId && record.operation === 'devices.sfs.format');
        if (!retained) throw new Error('The format job has no retained operation identity');
        const operation = await this.resolve(retained.operationId);
        if (operation.kind !== 'FORMAT' || operation.state !== 'COMMITTED')
            throw new Error('The format job has no confirmed durable commit');
        return job;
    }
    private async read<T>(operation: string, input: Record<string, unknown>): Promise<T> {
        const submitted = await this.client.invoke(operation, input, { idempotencyKey: randomIdempotencyKey() });
        if (!this.jobs.isJob(submitted)) throw new Error(`${operation} did not return a job`);
        let job = this.jobs.map(submitted);
        if (!['completed', 'failed', 'cancelled'].includes(job.status)) job = await this.jobs.wait(job.jobId, () => {});
        if (job.status !== 'completed')
            throw new AxklibApiError(
                job.errorCode ?? 'device_inspection_failed',
                job.error ?? 'Device inspection failed',
                422,
            );
        return job.result as T;
    }
}
