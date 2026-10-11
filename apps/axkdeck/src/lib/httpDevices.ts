import type { components } from './generated/axklibApiV1';
import { AxklibHttpApiClient, type AxklibApiConnection } from './httpApiClient';
import { AxklibApiError } from './httpErrors';
import { HttpJobController } from './httpJobController';
import { randomIdempotencyKey } from './httpTransportWire';
import type { PendingDeviceWrite } from './deviceWriteTracking';
import type { JobState } from './transport';

export function createHttpDeviceConnection(connection: AxklibApiConnection & { mode: string }) {
    const enabled = connection.mode === 'local' && '__TAURI_INTERNALS__' in window;
    const client = new AxklibHttpApiClient(connection, enabled ? window.localStorage : undefined);
    const jobs = new HttpJobController(client);
    return { client, jobs, devices: enabled ? new HttpDevices(client, jobs) : undefined };
}

export type DeviceCandidate = components['schemas']['DeviceCandidate'];
export type DeviceSession = components['schemas']['DeviceSession'];
export type DeviceOperation = components['schemas']['DeviceOperation'];
export type DeviceLayout = components['schemas']['DeviceConnectRequest']['layout'];
export type DeviceXmlInspection = components['schemas']['DeviceXmlInspection'];
export type DeviceProgress = NonNullable<JobState['progress']>;
export interface DeviceService {
    available(): Promise<boolean>;
    inspectXml(xml: string): Promise<DeviceXmlInspection>;
    list(): Promise<components['schemas']['DeviceCandidateList']>;
    connect(candidateId: string, layout: DeviceLayout, xml?: string): Promise<DeviceSession>;
    status(sessionId: string): Promise<DeviceSession>;
    close(sessionId: string): Promise<void>;
    authorize(sessionId: string, targetId: string, onProgress?: (progress: DeviceProgress) => void): Promise<void>;
    recover(operationId: string, sessionId: string, targetId: string): Promise<void>;
    operations(): Promise<{ operations: DeviceOperation[]; uncertain: PendingDeviceWrite[] }>;
}

export class HttpDevices implements DeviceService {
    constructor(
        private readonly client: AxklibHttpApiClient,
        private readonly jobs: HttpJobController,
    ) {}
    async available(): Promise<boolean> {
        const capabilities = await this.client.serverCapabilities();
        return ['devices.list', 'devices.connect', 'devices.xml.inspect'].every((id) =>
            capabilities.operations.some((operation) => operation.id === id && operation.implemented),
        );
    }
    async inspectXml(xml: string): Promise<DeviceXmlInspection> {
        return (await this.client.invoke<DeviceXmlInspection>('devices.xml.inspect', { xml })) as DeviceXmlInspection;
    }
    async list() {
        return (await this.client.invoke<components['schemas']['DeviceCandidateList']>(
            'devices.list',
        )) as components['schemas']['DeviceCandidateList'];
    }
    async status(sessionId: string): Promise<DeviceSession> {
        return (await this.client.invoke<DeviceSession>('devices.status', { sessionId })) as DeviceSession;
    }
    async close(sessionId: string): Promise<void> {
        await this.client.invoke('devices.close', { sessionId });
    }
    connect(candidateId: string, layout: DeviceLayout, xml?: string): Promise<DeviceSession> {
        return this.run('devices.connect', { candidateId, layout, ...(xml ? { xml } : {}) });
    }
    async authorize(
        sessionId: string,
        targetId: string,
        onProgress?: (progress: DeviceProgress) => void,
    ): Promise<void> {
        await this.run('devices.authorize', { sessionId, targetId, exclusiveUseAcknowledged: true }, onProgress);
    }
    async recover(operationId: string, sessionId: string, targetId: string): Promise<void> {
        await this.run('devices.recover', { operationId, sessionId, targetId, exclusiveUseAcknowledged: true });
    }
    async operations() {
        const result = (await this.client.invoke<components['schemas']['DeviceOperationList']>(
            'devices.operations',
        )) as components['schemas']['DeviceOperationList'];
        for (const record of this.client.deviceWrites.pending()) {
            if (record.jobId) {
                const job = await this.client
                    .request<components['schemas']['Job']>('GET', `/jobs/${encodeURIComponent(record.jobId)}`)
                    .catch((error: unknown) => {
                        if (error instanceof AxklibApiError && error.code === 'job_not_found') return null;
                        throw error;
                    });
                if (job && ['COMPLETED', 'FAILED', 'CANCELLED'].includes(job.state))
                    this.client.deviceWrites.terminal(job.jobId);
                if (job) continue;
            }
            {
                const resolved = (await this.client.invoke<DeviceOperation>('devices.resolve', {
                    operationId: record.operationId,
                })) as DeviceOperation;
                if (['NO_WRITE', 'COMMITTED', 'ROLLED_BACK'].includes(resolved.state))
                    this.client.deviceWrites.reconcile(record.operationId);
            }
        }
        return { ...result, uncertain: this.client.deviceWrites.pending() };
    }
    private async run<T>(
        operation: string,
        input: Record<string, unknown>,
        onProgress?: (progress: DeviceProgress) => void,
    ): Promise<T> {
        const submitted = await this.client.invoke(operation, input, { idempotencyKey: randomIdempotencyKey() });
        if (!this.jobs.isJob(submitted)) throw new Error(`${operation} did not return a job`);
        let job = this.jobs.map(submitted);
        const update = (value: JobState) => {
            if (value.progress) onProgress?.(value.progress);
        };
        update(job);
        if (!['completed', 'failed', 'cancelled'].includes(job.status)) job = await this.jobs.wait(job.jobId, update);
        if (job.status !== 'completed')
            throw new AxklibApiError(
                job.errorCode ?? 'device_operation_failed',
                job.error ?? 'Device operation failed',
                422,
            );
        return job.result as T;
    }
}
