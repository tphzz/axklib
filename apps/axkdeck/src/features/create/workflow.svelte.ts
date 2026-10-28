import type { DeviceService } from '../../lib/httpDevices';
import type { DeviceInspection, DeviceFormatPlan, DeviceFormatResult } from '../../lib/httpDeviceFormatting';
import { definiteDeviceRefusal } from '../../lib/httpDeviceFormatting';
import type { ImageSessionWorkflow } from '../image-session/workflow.svelte';
import type { JobState } from '../../lib/transport';
import { randomIdempotencyKey } from '../../lib/httpTransportWire';
import { userFacingMessage } from '../../lib/userFacingMessage';
import { CreateDeviceSelection } from './deviceSelection.svelte';
import type { SourceTab } from '../dialogs/sourceTabs';

type Phase =
    | 'editing'
    | 'reviewing'
    | 'confirming'
    | 'submitting'
    | 'running'
    | 'checking'
    | 'unconfirmed'
    | 'recovery'
    | 'complete'
    | 'opening';

export class CreateWorkflow {
    visible = $state(false);
    phase = $state<Phase>('editing');
    status = $state('');
    inspection = $state<DeviceInspection | null>(null);
    plan = $state<DeviceFormatPlan | null>(null);
    partitionCount = $state(1);
    confirmationText = $state('');
    dataLossAcknowledged = $state(false);
    exclusiveUseAcknowledged = $state(false);
    operationId = $state('');
    job = $state<JobState | null>(null);
    result = $state<DeviceFormatResult | null>(null);
    private reopenRequired = false;
    imageBusy = $state(false);
    imageCancellable = $state(true);
    cancelImage: (() => Promise<void>) | null = null;
    readonly selection: CreateDeviceSelection;
    constructor(
        service: DeviceService | undefined,
        private readonly images: ImageSessionWorkflow,
    ) {
        this.selection = new CreateDeviceSelection(service, () => this.invalidate());
    }
    get locked() {
        return ['submitting', 'running', 'checking', 'unconfirmed', 'recovery'].includes(this.phase);
    }
    get busy() {
        return (
            this.selection.busy ||
            this.imageBusy ||
            ['reviewing', 'submitting', 'running', 'checking', 'opening'].includes(this.phase)
        );
    }
    get canReview() {
        return (
            this.phase === 'editing' &&
            !this.busy &&
            !!this.inspection &&
            !this.inspection.blockedReasons.length &&
            this.inspection.partitionOptions.some((option) => option.partitionCount === this.partitionCount)
        );
    }
    get canFormat() {
        return (
            this.phase === 'confirming' &&
            !!this.plan &&
            this.confirmationText === this.plan.inspection.confirmationText &&
            this.dataLossAcknowledged &&
            this.exclusiveUseAcknowledged
        );
    }
    async show() {
        if (this.visible || this.locked) return;
        this.visible = true;
        this.phase = 'editing';
        this.status = '';
        this.operationId = '';
        this.job = null;
        this.result = null;
        this.reopenRequired = false;
        this.selection.tab = 'Image';
        this.invalidate();
        await this.selection.initialize();
    }
    invalidate() {
        this.inspection = null;
        this.plan = null;
        this.confirmationText = '';
        this.dataLossAcknowledged = false;
        this.exclusiveUseAcknowledged = false;
        if (!this.locked) this.phase = 'editing';
    }
    async activate(tab: SourceTab) {
        if (this.busy || this.locked || this.result || this.phase === 'complete') return;
        await this.selection.activate(tab);
    }
    async select(candidateId: string) {
        if (this.busy || this.locked) return;
        await this.selection.select(candidateId);
        if (this.selection.target) await this.inspect();
    }
    async selectTarget(targetId: string) {
        if (this.busy || this.locked) return;
        this.invalidate();
        this.selection.targetId = targetId;
        if (this.selection.target) await this.inspect();
    }
    async inspect() {
        const selection = this.selection;
        if (!selection.connection || !selection.target || this.locked || this.busy) return;
        this.invalidate();
        this.phase = 'reviewing';
        this.status = 'Inspecting device contents read-only...';
        try {
            this.inspection = await selection.service!.formatting!.inspect(
                selection.connection.sessionId,
                selection.targetId,
            );
            this.partitionCount = this.inspection.partitionOptions[0]?.partitionCount ?? 1;
            this.status = '';
        } catch (error) {
            this.status = userFacingMessage(error);
        } finally {
            this.phase = 'editing';
        }
    }
    setPartitions(value: number) {
        if (this.busy || this.locked) return;
        this.partitionCount = value;
        this.plan = null;
        this.confirmationText = '';
        this.dataLossAcknowledged = false;
        this.exclusiveUseAcknowledged = false;
        this.phase = 'editing';
    }
    async review() {
        if (!this.canReview) return;
        this.phase = 'reviewing';
        this.status = 'Reviewing current device bytes...';
        try {
            if (!(await this.images.confirmEditorLeave())) {
                this.phase = 'editing';
                this.status = '';
                return;
            }
            this.plan = await this.selection.service!.formatting!.plan(
                this.selection.connection!.sessionId,
                this.inspection!.inspectionToken,
                this.partitionCount,
            );
            this.inspection = this.plan.inspection;
            this.confirmationText = '';
            this.dataLossAcknowledged = false;
            this.exclusiveUseAcknowledged = false;
            this.phase = 'confirming';
            this.status = '';
        } catch (error) {
            this.plan = null;
            this.phase = 'editing';
            this.status = userFacingMessage(error);
        }
    }
    cancelConfirmation() {
        if (this.phase !== 'confirming') return;
        this.plan = null;
        this.confirmationText = '';
        this.dataLossAcknowledged = false;
        this.exclusiveUseAcknowledged = false;
        this.phase = 'editing';
    }
    async format() {
        if (!this.canFormat) return;
        const plan = this.plan!;
        this.operationId = randomIdempotencyKey();
        this.phase = 'submitting';
        this.status = 'Submitting format request...';
        try {
            this.job = await this.selection.service!.formatting!.start(
                {
                    sessionId: this.selection.connection!.sessionId,
                    planToken: plan.planToken,
                    confirmationText: this.confirmationText,
                    dataLossAcknowledged: true,
                    exclusiveUseAcknowledged: true,
                },
                this.operationId,
                `${plan.inspection.candidate.model} / ${plan.inspection.target.label}`,
            );
            this.phase = 'running';
            await this.observe();
        } catch (error) {
            if (!this.job && definiteDeviceRefusal(error)) {
                this.phase = 'editing';
                this.plan = null;
                this.status = userFacingMessage(error);
            } else this.unconfirmed(error);
        }
    }
    async checkStatus() {
        if (!this.locked || this.busy) return;
        this.phase = 'checking';
        this.status = 'Checking retained operation status...';
        try {
            if (this.job) {
                this.job = await this.selection.service!.formatting!.status(this.job.jobId);
                this.phase = 'running';
                await this.observe();
                return;
            }
            await this.resolveOperation();
        } catch (error) {
            this.unconfirmed(error);
        }
    }
    private async resolveOperation() {
        this.phase = 'checking';
        const operation = await this.selection.service!.formatting!.resolve(this.operationId);
        if (operation.kind !== 'FORMAT' && operation.kind !== 'UNSTARTED')
            throw new Error('Operation kind does not match this format request.');
        if (operation.state === 'COMMITTED') {
            if (this.plan)
                this.result = {
                    operationId: this.operationId,
                    sessionId: this.selection.connection?.sessionId ?? this.plan.inspection.sessionId,
                    targetId: this.plan.inspection.target.id,
                    sizeBytes: this.plan.inspection.formattedSizeBytes,
                    partitionCount: this.plan.partitionCount,
                };
            this.phase = 'complete';
            await this.openFormatted();
        } else if (['NO_WRITE', 'ROLLED_BACK'].includes(operation.state)) {
            this.phase = 'editing';
            this.invalidate();
            await this.selection.release();
            this.status =
                operation.state === 'NO_WRITE'
                    ? 'No device write was admitted. Select and inspect the device again.'
                    : 'Original device bytes were restored. Select and inspect the device again.';
        } else {
            this.phase = 'recovery';
            await this.selection.release();
            this.status = 'Recovery is required. Retain the journal and open Device operation status.';
        }
    }
    async cancel() {
        if (this.imageBusy) {
            await this.cancelImage?.();
            return;
        }
        if (this.locked) {
            if (this.phase === 'running' && this.job) {
                try {
                    await this.selection.service!.formatting!.cancel(this.job.jobId);
                    this.status = 'Cancellation requested; keep the device connected until status is confirmed.';
                } catch (error) {
                    this.unconfirmed(error);
                }
            }
            return;
        }
        if (this.busy) return;
        if (this.phase === 'confirming') {
            this.cancelConfirmation();
            return;
        }
        if (!(await this.selection.release())) return;
        this.visible = false;
        this.invalidate();
    }
    async openFormatted() {
        if (this.phase !== 'complete' || !this.inspection) return;
        this.phase = 'opening';
        try {
            if (this.reopenRequired || !this.selection.connection) await this.selection.reconnect(this.inspection);
            const connection = this.selection.connection!;
            const target = this.selection.target!;
            await this.images.open([
                {
                    kind: 'device',
                    reference: { sessionId: connection.sessionId, targetId: target.id },
                    displayName: `${connection.candidate.model} / ${target.label}`,
                },
            ]);
            if (
                this.images.location?.kind !== 'device' ||
                this.images.location.reference.sessionId !== connection.sessionId ||
                this.images.location.reference.targetId !== target.id
            )
                throw new Error('Formatting completed, but reopening failed. Retry Open; do not format again.');
            this.selection.connection = null;
            this.visible = false;
            this.images.setStatus('Device formatted as empty Yamaha SFS.');
        } catch (error) {
            this.reopenRequired = true;
            this.status = userFacingMessage(error);
        } finally {
            this.phase = 'complete';
        }
    }
    private async observe() {
        const formatting = this.selection.service!.formatting!;
        let job = this.job!;
        if (job.status === 'completed') job = await formatting.status(job.jobId);
        if (!['completed', 'failed', 'cancelled'].includes(job.status))
            job = await formatting.wait(job.jobId, (value) => {
                this.job = value;
                this.status = value.progress?.label ?? 'Formatting device; keep it connected...';
            });
        this.job = job;
        if (job.status === 'completed') {
            this.result = job.result as DeviceFormatResult;
            this.phase = 'complete';
            await this.openFormatted();
        } else {
            this.job = null;
            await this.resolveOperation();
        }
    }
    private unconfirmed(error: unknown) {
        this.phase = 'unconfirmed';
        this.status = `${userFacingMessage(error)} Check status before continuing; do not submit another format.`;
    }
}
