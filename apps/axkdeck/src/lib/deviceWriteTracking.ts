import type { DeviceLocation } from './storageLocations';

export interface PendingDeviceWrite {
    operationId: string;
    operation: string;
    displayName: string;
    jobId?: string;
}

const storageKey = 'axkdeck.device-operations';

export class DeviceWriteTracking {
    private readonly images = new Map<string, DeviceLocation>();
    private records: PendingDeviceWrite[] = [];
    constructor(private readonly storage?: Storage) {
        const text = storage?.getItem(storageKey);
        if (text) {
            const parsed: unknown = JSON.parse(text);
            if (
                !Array.isArray(parsed) ||
                parsed.length > 4096 ||
                parsed.some(
                    (item) =>
                        !item ||
                        typeof item.operationId !== 'string' ||
                        typeof item.operation !== 'string' ||
                        typeof item.displayName !== 'string' ||
                        (item.jobId !== undefined && typeof item.jobId !== 'string'),
                )
            )
                throw new Error('Device operation tracking is invalid; retain local application data for review.');
            this.records = parsed;
        }
    }
    bind(imageId: string, location: DeviceLocation): void {
        this.images.set(imageId, location);
    }
    unbind(imageId: string): void {
        this.images.delete(imageId);
    }
    pending(): PendingDeviceWrite[] {
        return this.records.map((record) => ({ ...record }));
    }
    begin(imageId: unknown, operation: string, operationId?: string): string | undefined {
        const location = typeof imageId === 'string' ? this.images.get(imageId) : undefined;
        if (!location) return undefined;
        return this.beginTarget(location.displayName, operation, operationId);
    }
    beginTarget(displayName: string, operation: string, operationId?: string): string {
        if (!this.storage)
            throw new Error('Durable local operation tracking is unavailable. Device writing is disabled.');
        if (!operationId) throw new Error('Device mutations require a known operation identity before submission.');
        if (this.records.some((record) => record.operation === 'devices.sfs.format'))
            throw new Error('Resolve the pending device format before submitting another write.');
        if (this.records.some((record) => !record.jobId))
            throw new Error(
                'A device write has an uncertain outcome. Check device operation status before submitting another write.',
            );
        if (this.records.length >= 4096) throw new Error('Device operation tracking capacity is exhausted.');
        this.records.push({ operationId, operation, displayName });
        this.persist();
        return operationId;
    }
    acknowledge(operationId: string | undefined, jobId: string): void {
        const record = this.records.find((item) => item.operationId === operationId);
        if (record) {
            record.jobId = jobId;
            this.persist();
        }
    }
    terminal(jobId: string): void {
        if (!this.records.some((record) => record.jobId === jobId && record.operation !== 'devices.sfs.format')) return;
        this.records = this.records.filter(
            (record) => record.jobId !== jobId || record.operation === 'devices.sfs.format',
        );
        this.persist();
    }
    reconcile(operationId: string): void {
        this.records = this.records.filter((record) => record.operationId !== operationId);
        this.persist();
    }
    private persist(): void {
        this.storage?.setItem(storageKey, JSON.stringify(this.records));
    }
}
