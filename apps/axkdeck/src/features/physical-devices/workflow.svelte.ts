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
    confirmationText = $state('');
    dataLossAcknowledged = $state(false);
    constructor(
        readonly service: DeviceService,
        readonly images: ImageSessionWorkflow,
    ) {}
    get target() {
        return this.connection?.targets.find((target) => target.id === this.targetId);
    }
    get visibleCandidates(): DeviceCandidate[] {
        return this.candidates;
    }
    get formatRecovery() {
        return this.operations.find((operation) => operation.operationId === this.recoveryId)?.kind === 'FORMAT';
    }
    get canRecover() {
        return (
            !!this.connection &&
            !this.connection.invalidated &&
            this.connection.writesSupported &&
            !!this.target &&
            !this.target.readOnly &&
            !!this.recoveryId &&
            this.acknowledged &&
            !this.busy &&
            (!this.formatRecovery ||
                (!!this.target.recoveryConfirmationText &&
                    this.confirmationText === this.target.recoveryConfirmationText &&
                    this.dataLossAcknowledged))
        );
    }
    selectTarget(targetId: string) {
        if (this.busy) return;
        this.resetConsent();
        this.targetId = targetId;
    }
    selectOperation(operationId: string) {
        if (this.busy) return;
        this.resetConsent();
        this.recoveryId = operationId;
    }
    private resetConsent() {
        this.acknowledged = false;
        this.confirmationText = '';
        this.dataLossAcknowledged = false;
    }
    async show(): Promise<void> {
        this.mode = 'recovery';
        this.status = '';
        this.resetConsent();
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
            this.resetConsent();
            this.connection = await this.service.connect(this.candidateId, this.layout, this.xml);
            this.targetId = '';
        });
    }
    async recover(): Promise<void> {
        if (!this.canRecover) return;
        await this.run('Recovering device; keep it connected', async () => {
            if (this.formatRecovery)
                await this.service.recover(this.recoveryId, this.connection!.sessionId, this.targetId, {
                    confirmationText: this.confirmationText,
                    dataLossAcknowledged: true,
                });
            else await this.service.recover(this.recoveryId, this.connection!.sessionId, this.targetId);
            await this.service.close(this.connection!.sessionId);
            this.connection = null;
            this.targetId = '';
            this.resetConsent();
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
