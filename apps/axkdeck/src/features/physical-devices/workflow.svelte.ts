import { getContext, setContext } from 'svelte';
import type {
    DeviceService,
    DeviceCandidate,
    DeviceSession,
    DeviceLayout,
    DeviceOperation,
} from '../../lib/httpDevices';
import type { PendingDeviceWrite } from '../../lib/deviceWriteTracking';
import type { ImageSessionWorkflow } from '../image-session/workflow.svelte';
import { userFacingMessage } from '../../lib/userFacingMessage';

const contextKey = Symbol('physical-devices');
export function provideDevices(
    service: DeviceService | undefined,
    images: ImageSessionWorkflow,
): DeviceWorkflow | undefined {
    const workflow = service ? new DeviceWorkflow(service, images) : undefined;
    setContext(contextKey, workflow);
    return workflow;
}
export function useDevices(): DeviceWorkflow | undefined {
    return getContext(contextKey);
}

export class DeviceWorkflow {
    mode = $state<'recovery' | null>(null);
    busy = $state(false);
    status = $state('');
    candidates = $state<DeviceCandidate[]>([]);
    candidateId = $state('');
    layout = $state<DeviceLayout>('RAW');
    xml = $state('');
    connection = $state<DeviceSession | null>(null);
    targetId = $state('');
    acknowledged = $state(false);
    operations = $state<DeviceOperation[]>([]);
    uncertain = $state<PendingDeviceWrite[]>([]);
    recoveryId = $state('');
    constructor(
        readonly service: DeviceService,
        readonly images: ImageSessionWorkflow,
    ) {}
    get target() {
        return this.connection?.targets.find((target) => target.id === this.targetId);
    }
    get visibleCandidates(): DeviceCandidate[] {
        return this.layout === 'RAW'
            ? this.candidates.filter((candidate) => candidate.sizeBytes <= 9_000_000_000)
            : this.candidates;
    }
    async show(): Promise<void> {
        this.mode = 'recovery';
        this.status = '';
        this.acknowledged = false;
        this.recoveryId = '';
        this.connection = null;
        this.targetId = '';
        this.candidateId = '';
        await this.refresh();
    }
    async refresh(): Promise<void> {
        await this.run('Reading device list', async () => {
            this.candidates = (await this.service.list()).devices;
            await this.checkStatus();
        });
    }
    async checkStatus(): Promise<void> {
        const result = await this.service.operations();
        this.operations = result.operations;
        this.uncertain = result.uncertain;
    }
    async connect(): Promise<void> {
        if (
            !this.visibleCandidates.some((candidate) => candidate.id === this.candidateId) ||
            this.busy ||
            this.connection
        )
            return;
        await this.run('Connecting read-only', async () => {
            this.connection = await this.service.connect(this.candidateId, this.layout, this.xml);
            this.targetId = '';
        });
    }
    async recover(): Promise<void> {
        if (!this.connection || !this.target || !this.recoveryId || !this.acknowledged || this.busy) return;
        await this.run('Recovering device; keep it connected', async () => {
            await this.service.recover(this.recoveryId, this.connection!.sessionId, this.targetId);
            await this.service.close(this.connection!.sessionId);
            this.connection = null;
            this.targetId = '';
            this.acknowledged = false;
            await this.checkStatus();
            this.status = 'Recovery completed. Select and reopen the device explicitly.';
        });
    }
    async close(): Promise<void> {
        if (this.busy) return;
        if (this.connection) {
            try {
                await this.service.close(this.connection.sessionId);
            } catch (error) {
                this.status = userFacingMessage(error);
                return;
            }
        }
        this.connection = null;
        this.mode = null;
    }
    async run(label: string, action: () => Promise<void>): Promise<void> {
        if (this.busy) return;
        this.busy = true;
        this.status = label;
        try {
            await action();
            if (this.status === label) this.status = '';
        } catch (error) {
            this.status = userFacingMessage(error);
        } finally {
            this.busy = false;
        }
    }
}
