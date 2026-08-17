import type { AuditionWorkflow } from '../audition/workflow.svelte';
import type { CatalogWorkflow } from '../catalog/workflow.svelte';
import type { JobController } from '../jobs/actions';
import { compareNamedItems } from '../../lib/naturalSort';
import { isEffectiveProgramAssignment } from '../../lib/relationshipResolution';
import type { ImageTransport } from '../../lib/transport';
import type {
    SampleBankAssignmentBlocker,
    SampleBankAssignmentOption,
    SampleStructureItem,
    WorkspaceView,
} from '../../lib/types';
import { userFacingMessage } from '../../lib/userFacingMessage';

interface SampleBankAssignmentDependencies {
    transport: ImageTransport;
    jobs: JobController;
    catalog: CatalogWorkflow;
    audition: AuditionWorkflow;
    available: () => boolean;
    sessionId: () => number | null;
    setWorkspaceView: (view: WorkspaceView) => void;
    clearSelection: () => void;
    refreshSession: (preferred?: { partitionIndex: number; volumeName?: string }) => Promise<void>;
    setStatus: (status: string) => void;
    reportTiming: (operation: string, started: number, itemCount: number) => void;
}

export interface SampleBankAssignmentRequest {
    samples: SampleStructureItem[];
    options: SampleBankAssignmentOption[];
    blockers: SampleBankAssignmentBlocker[];
    partitionIndex: number;
    volumeName: string;
    busy: boolean;
    error: string;
}

export class SampleBankAssignmentWorkflow {
    request = $state<SampleBankAssignmentRequest | null>(null);

    constructor(private readonly dependencies: SampleBankAssignmentDependencies) {}

    open(samples: SampleStructureItem[]): void {
        if (!this.dependencies.available() || this.dependencies.sessionId() === null || samples.length === 0) return;
        const first = samples[0]!.object;
        const valid =
            samples.length <= 127 &&
            samples.every(
                (sample) =>
                    sample.objectType === 'SBNK' &&
                    sample.object.partitionIndex === first.partitionIndex &&
                    sample.object.volumeName === first.volumeName,
            );
        if (!valid) return;
        const banks = this.dependencies.catalog.sampleBanks
            .filter(
                (bank) =>
                    bank.object.partitionIndex === first.partitionIndex && bank.object.volumeName === first.volumeName,
            )
            .toSorted(compareNamedItems);
        if (banks.length === 0) return;
        const selectedIds = new Set(samples.map((sample) => sample.objectId));
        const options = banks.map((bank): SampleBankAssignmentOption => {
            const memberIds = new Set(
                this.dependencies.catalog.membersForBank(bank.objectId).map((member) => member.objectId),
            );
            const selectedMemberCount = samples.filter((sample) => memberIds.has(sample.objectId)).length;
            const movedSampleCount = samples.length - selectedMemberCount;
            const reassignedSampleCount = samples.filter(
                (sample) => !memberIds.has(sample.objectId) && (sample.sampleBankObjectIds?.length ?? 0) > 0,
            ).length;
            return {
                objectId: bank.objectId,
                name: bank.name,
                memberCount: memberIds.size,
                selectedMemberCount,
                movedSampleCount,
                reassignedSampleCount,
                finalMemberCount: memberIds.size + movedSampleCount,
            };
        });
        this.request = {
            samples: [...samples],
            options,
            blockers: this.directProgramBlockers(samples, selectedIds),
            partitionIndex: first.partitionIndex,
            volumeName: first.volumeName,
            busy: false,
            error: '',
        };
    }

    cancel(): void {
        if (!this.request?.busy) this.request = null;
    }

    reset(): void {
        this.request = null;
    }

    async submit(bankObjectId: string): Promise<void> {
        const request = this.request;
        const sessionId = this.dependencies.sessionId();
        const target = request?.options.find((option) => option.objectId === bankObjectId);
        if (
            !request ||
            request.busy ||
            sessionId === null ||
            request.blockers.length > 0 ||
            !target ||
            target.movedSampleCount === 0 ||
            target.finalMemberCount > 127
        )
            return;
        const preferred = { partitionIndex: request.partitionIndex, volumeName: request.volumeName };
        const started = performance.now();
        request.busy = true;
        request.error = '';
        this.dependencies.setStatus(`Assigning Samples to ${target.name}`);
        try {
            await this.dependencies.audition.invalidateSession(sessionId);
            const completed = await this.dependencies.jobs.run(
                () =>
                    this.dependencies.transport.startSampleBankAssignment(sessionId, {
                        ...preferred,
                        sampleBankName: target.name,
                        sampleNames: request.samples.map((sample) => sample.name),
                    }),
                (update) => {
                    if (update.progress?.label) this.dependencies.setStatus(update.progress.label);
                },
            );
            if (completed.status !== 'completed') {
                throw new Error(completed.error ?? 'Sample Bank assignment did not complete');
            }
            await this.dependencies.refreshSession(preferred);
            this.selectTarget(target, request);
            this.dependencies.clearSelection();
            this.request = null;
            this.dependencies.setStatus(`Assigned Samples to ${target.name}`);
            this.dependencies.reportTiming('assign-sample-bank', started, request.samples.length);
        } catch (error) {
            const message = userFacingMessage(error);
            if (this.dependencies.sessionId() !== null)
                await this.dependencies.refreshSession(preferred).catch(() => undefined);
            if (this.request === request) {
                request.busy = false;
                request.error = message;
            }
            this.dependencies.setStatus(message);
        }
    }

    private directProgramBlockers(
        samples: SampleStructureItem[],
        selectedIds: ReadonlySet<string>,
    ): SampleBankAssignmentBlocker[] {
        return this.dependencies.catalog.relationships
            .filter(
                (relationship) =>
                    isEffectiveProgramAssignment(relationship) &&
                    relationship.relationshipType === 'PROG_ASSIGNMENT_TO_SBNK' &&
                    Boolean(relationship.targetObjectId && selectedIds.has(relationship.targetObjectId)),
            )
            .map((relationship): SampleBankAssignmentBlocker => {
                const sample = samples.find((item) => item.objectId === relationship.targetObjectId)!;
                const program = this.dependencies.catalog.programs.find(
                    (item) => item.objectId === relationship.sourceObjectId,
                );
                return {
                    sampleName: sample.name,
                    programName: program ? `${program.slot}: ${program.name}` : '(unresolved)',
                };
            })
            .filter(
                (blocker, index, all) =>
                    all.findIndex(
                        (candidate) =>
                            candidate.sampleName === blocker.sampleName &&
                            candidate.programName === blocker.programName,
                    ) === index,
            );
    }

    private selectTarget(target: SampleBankAssignmentOption, request: SampleBankAssignmentRequest): void {
        this.dependencies.setWorkspaceView('sample-banks');
        const refreshedTarget =
            this.dependencies.catalog.sampleBanks.find((bank) => bank.objectId === target.objectId) ??
            this.dependencies.catalog.sampleBanks.find(
                (bank) =>
                    bank.object.partitionIndex === request.partitionIndex &&
                    bank.object.volumeName === request.volumeName &&
                    bank.name === target.name,
            );
        if (!refreshedTarget) return;
        this.dependencies.catalog.selectedBankId = refreshedTarget.objectId;
        this.dependencies.catalog.inspectorObjectId = refreshedTarget.objectId;
        this.dependencies.catalog.editorObjectIds['sample-banks'] = refreshedTarget.objectId;
    }
}
