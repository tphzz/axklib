import type { AxklibHttpApiClient } from './httpApiClient';
import type { HttpJobController } from './httpJobController';
import type { CapacityAdmission, CapacityPolicy } from './importCapacity';
import { randomIdempotencyKey } from './httpTransportWire';
import type { JobState } from './transport';

export type CapacityReviewHandler = (
    check: (policy: CapacityPolicy) => Promise<CapacityAdmission>,
    context: CapacityReviewContext,
) => Promise<CapacityPolicy>;

export interface CapacityReviewContext {
    emptyVolumeCreation: boolean;
}

function record(value: unknown): Record<string, unknown> | null {
    return value !== null && typeof value === 'object' && !Array.isArray(value)
        ? (value as Record<string, unknown>)
        : null;
}

function createsOnlyEmptyVolumes(request: Record<string, unknown>, inspection: string): boolean {
    if (inspection !== 'images.alter.inspect') return false;
    const operations = record(record(request.manifest)?.inline)?.operations;
    if (!Array.isArray(operations) || operations.length === 0) return false;
    return operations.every((value) => {
        const operation = record(value);
        const volume = record(operation?.volume);
        return (
            operation?.type === 'insert_volume' &&
            volume !== null &&
            ['waveforms', 'samples', 'sample_banks', 'programs'].every((key) => {
                const objects = volume[key];
                return (
                    (objects === undefined && (key === 'sample_banks' || key === 'programs')) ||
                    (Array.isArray(objects) && objects.length === 0)
                );
            })
        );
    });
}

// Raised only before a mutation request has been sent.
export class CapacityWriteRejected extends Error {
    readonly code = 'capacity_review_refused';
}

export class HttpCapacityGate {
    constructor(
        private readonly client: AxklibHttpApiClient,
        private readonly jobs: HttpJobController,
        private readonly reviewer?: CapacityReviewHandler,
    ) {}

    async admit(
        request: Record<string, unknown>,
        inspection = 'images.alter.inspect',
    ): Promise<Record<string, unknown>> {
        const frozen: Record<string, unknown> = JSON.parse(JSON.stringify(request));
        if (!this.reviewer || frozen.capacityPolicy) return frozen;
        try {
            const policy = await this.reviewer(
                async (capacityPolicy) => {
                    const result = await this.client.invoke<{ capacity: CapacityAdmission }>(inspection, {
                        ...frozen,
                        capacityPolicy,
                    });
                    if (this.jobs.isJob(result)) throw new Error('Capacity inspection unexpectedly returned a job');
                    return result.capacity;
                },
                { emptyVolumeCreation: createsOnlyEmptyVolumes(frozen, inspection) },
            );
            return { ...frozen, capacityPolicy: policy };
        } catch (error) {
            throw new CapacityWriteRejected(error instanceof Error ? error.message : 'Capacity review failed');
        }
    }

    async start(
        request: Record<string, unknown>,
        operation = 'images.alter',
        inspection = 'images.alter.inspect',
    ): Promise<JobState> {
        const admitted = await this.admit(request, inspection);
        const job = await this.client.invoke<never>(operation, admitted, { idempotencyKey: randomIdempotencyKey() });
        if (!this.jobs.isJob(job)) throw new Error(`${operation} did not return a job`);
        return this.jobs.map(job);
    }
}
