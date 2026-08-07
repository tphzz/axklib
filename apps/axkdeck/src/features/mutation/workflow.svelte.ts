import type { CatalogWorkflow } from '../catalog/workflow.svelte';
import type { AuditionWorkflow } from '../audition/workflow.svelte';
import type { JobController } from '../jobs/actions';
import type {
    ImageTransport,
    ObjectRenameMutation,
    PartitionMutation,
    PlacementRepairInspection,
    PlacementRepairScope,
    VolumeDeletionInspection,
    VolumeMutation,
} from '../../lib/transport';
import type {
    DiskTreeItem,
    ImageTreeAction,
    ObjectRenameTarget,
    SampleStructureItem,
    WorkspaceView,
} from '../../lib/types';
import { userFacingMessage } from '../../lib/userFacingMessage';

interface MutationWorkflowDependencies {
    transport: ImageTransport;
    jobs: JobController;
    catalog: CatalogWorkflow;
    audition: AuditionWorkflow;
    sessionId: () => number | null;
    imageOpen: () => boolean;
    workspaceView: () => WorkspaceView;
    setWorkspaceView: (view: WorkspaceView) => void;
    clearSelection: () => void;
    refreshSession: (preferred?: { partitionIndex: number; volumeName?: string }) => Promise<void>;
    setStatus: (status: string) => void;
    reportTiming: (operation: string, started: number, itemCount: number) => void;
}

interface ObjectSelectionSnapshot {
    view: WorkspaceView;
    programId: string;
    sequenceId: string;
    bankId: string;
    bankMemberId: string;
    sampleId: string;
    bankWaveDataId: string;
    sampleWaveDataId: string;
    waveDataId: string;
    inspectorId: string;
    editorIds: Record<WorkspaceView, string>;
}

export class MutationWorkflow {
    private volumeActionGeneration = 0;
    private placementRepairGeneration = 0;
    volumeAvailable = $state(false);
    partitionAvailable = $state(false);
    objectRenameAvailable = $state(false);
    volumeAction = $state<{ item: DiskTreeItem; action: ImageTreeAction } | null>(null);
    volumeActionBusy = $state(false);
    volumeActionPhase = $state<'idle' | 'checking' | 'submitting'>('idle');
    volumeActionError = $state('');
    volumeDeletionInspection = $state<VolumeDeletionInspection | null>(null);
    placementRepairRequest = $state<{
        item: DiskTreeItem;
        scope: PlacementRepairScope;
        inspection: PlacementRepairInspection | null;
        busy: boolean;
        phase: 'inspecting' | 'idle' | 'repairing';
        error: string;
        message: string;
    } | null>(null);
    objectRenameRequest = $state<{
        target: ObjectRenameTarget;
        busy: boolean;
        error: string;
    } | null>(null);
    sampleBankCreationRequest = $state<{
        samples: SampleStructureItem[];
        assignedSampleCount: number;
        partitionIndex: number;
        volumeName: string;
        existingNames: string[];
        busy: boolean;
        error: string;
    } | null>(null);

    constructor(private readonly dependencies: MutationWorkflowDependencies) {}

    setCapabilities(capabilities: {
        volumeMutationsAvailable: boolean;
        partitionMutationsAvailable: boolean;
        objectRenameAvailable: boolean;
    }): void {
        this.volumeAvailable = capabilities.volumeMutationsAvailable;
        this.partitionAvailable = capabilities.partitionMutationsAvailable;
        this.objectRenameAvailable = capabilities.objectRenameAvailable;
    }

    requestVolumeAction(item: DiskTreeItem, action: ImageTreeAction): boolean {
        if (action === 'repair-placement') return this.requestPlacementRepair(item);
        const partitionAction = action === 'rename-partition';
        if (partitionAction && (!this.partitionAvailable || item.kind !== 'partition')) return false;
        if (!partitionAction && !this.volumeAvailable) return false;
        if (action === 'add-volume' && item.kind !== 'partition') return false;
        if ((action === 'rename-volume' || action === 'delete-volume') && item.kind !== 'volume') return false;
        this.volumeActionError = '';
        this.volumeDeletionInspection = null;
        const request = { item, action };
        const generation = ++this.volumeActionGeneration;
        this.volumeAction = request;
        if (action === 'delete-volume') void this.inspectVolumeDeletion(request, generation);
        return true;
    }

    requestObjectRename(target: ObjectRenameTarget): void {
        if (!this.objectRenameAvailable || this.dependencies.sessionId() === null) return;
        this.objectRenameRequest = { target, busy: false, error: '' };
    }

    requestSampleBankCreation(samples: SampleStructureItem[]): void {
        if (!this.objectRenameAvailable || this.dependencies.sessionId() === null || samples.length === 0) return;
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
        this.sampleBankCreationRequest = {
            samples: [...samples],
            assignedSampleCount: samples.filter((sample) => (sample.sampleBankObjectIds?.length ?? 0) > 0).length,
            partitionIndex: first.partitionIndex,
            volumeName: first.volumeName,
            existingNames: this.dependencies.catalog.sampleBanks
                .filter(
                    (bank) =>
                        bank.object.partitionIndex === first.partitionIndex &&
                        bank.object.volumeName === first.volumeName,
                )
                .map((bank) => bank.name),
            busy: false,
            error: '',
        };
    }

    cancelSampleBankCreation(): void {
        if (!this.sampleBankCreationRequest?.busy) this.sampleBankCreationRequest = null;
    }

    async submitSampleBankCreation(name: string): Promise<void> {
        const request = this.sampleBankCreationRequest;
        const sessionId = this.dependencies.sessionId();
        if (!request || request.busy || sessionId === null) return;
        const preferred = { partitionIndex: request.partitionIndex, volumeName: request.volumeName };
        const started = performance.now();
        request.busy = true;
        request.error = '';
        this.dependencies.setStatus(`Creating Sample Bank ${name}`);
        try {
            await this.dependencies.audition.invalidateSession(sessionId);
            const completed = await this.dependencies.jobs.run(
                () =>
                    this.dependencies.transport.startSampleBankCreation(sessionId, {
                        ...preferred,
                        sampleBankName: name,
                        sampleNames: request.samples.map((sample) => sample.name),
                    }),
                (update) => {
                    if (update.progress?.label) this.dependencies.setStatus(update.progress.label);
                },
            );
            if (completed.status !== 'completed') {
                throw new Error(completed.error ?? 'Sample Bank creation did not complete');
            }
            await this.dependencies.refreshSession(preferred);
            this.dependencies.setWorkspaceView('sample-banks');
            const inserted = this.dependencies.catalog.sampleBanks.find(
                (bank) =>
                    bank.object.partitionIndex === request.partitionIndex &&
                    bank.object.volumeName === request.volumeName &&
                    bank.name === name,
            );
            if (inserted) {
                this.dependencies.catalog.selectedBankId = inserted.objectId;
                this.dependencies.catalog.inspectorObjectId = inserted.objectId;
                this.dependencies.catalog.editorObjectIds['sample-banks'] = inserted.objectId;
            }
            this.dependencies.clearSelection();
            this.sampleBankCreationRequest = null;
            this.dependencies.setStatus(`Created Sample Bank ${name}`);
            this.dependencies.reportTiming('create-sample-bank', started, request.samples.length);
        } catch (error) {
            const message = userFacingMessage(error);
            if (this.dependencies.sessionId() !== null)
                await this.dependencies.refreshSession(preferred).catch(() => undefined);
            if (this.sampleBankCreationRequest === request) {
                request.busy = false;
                request.error = message;
            }
            this.dependencies.setStatus(message);
        }
    }

    cancelVolumeAction(): void {
        if (!this.volumeActionBusy) {
            ++this.volumeActionGeneration;
            this.volumeAction = null;
            this.volumeDeletionInspection = null;
            this.volumeActionPhase = 'idle';
        }
    }

    cancelPlacementRepair(): void {
        if (!this.placementRepairRequest?.busy) {
            ++this.placementRepairGeneration;
            this.placementRepairRequest = null;
        }
    }

    async submitPlacementRepair(recoveryVolumeName?: string): Promise<void> {
        const request = this.placementRepairRequest;
        const sessionId = this.dependencies.sessionId();
        if (!request?.inspection?.canRepair || request.busy || sessionId === null) return;
        const generation = this.placementRepairGeneration;
        const repairCount = request.inspection.repairObjectCount;
        const partitionIndex = request.scope.partitionIndex;
        const preferredVolumeName =
            request.scope.kind === 'VOLUME' ? request.scope.volumeName : recoveryVolumeName?.trim();
        const started = performance.now();
        request.busy = true;
        request.phase = 'repairing';
        request.error = '';
        request.message = '';
        this.dependencies.setStatus('Repairing object placement');
        try {
            await this.dependencies.audition.invalidateSession(sessionId);
            const completed = await this.dependencies.jobs.run(
                () =>
                    this.dependencies.transport.startPlacementRepair(
                        sessionId,
                        request.scope,
                        request.scope.kind === 'PARTITION' ? preferredVolumeName : undefined,
                    ),
                (update) => {
                    if (update.progress?.label) this.dependencies.setStatus(update.progress.label);
                },
            );
            if (completed.status !== 'completed') {
                throw new Error(completed.error ?? 'Object placement repair did not complete');
            }
            await this.dependencies.refreshSession({ partitionIndex, volumeName: preferredVolumeName });
            if (!this.isCurrentPlacementRepair(generation)) return;
            const refreshedSessionId = this.dependencies.sessionId();
            if (refreshedSessionId === null) throw new Error('Image session is no longer available');
            request.inspection = await this.dependencies.transport.inspectPlacement(refreshedSessionId, request.scope);
            const repairedObjects = `${repairCount} ${repairCount === 1 ? 'object' : 'objects'}`;
            const blockedCount = request.inspection.blockedObjectCount;
            request.message =
                blockedCount > 0
                    ? `Repaired ${repairedObjects}. ${blockedCount} ambiguous ${blockedCount === 1 ? 'object remains' : 'objects remain'} unchanged.`
                    : `Repaired placement for ${repairedObjects}.`;
            this.dependencies.setStatus(request.message);
            this.dependencies.reportTiming('repair-object-placement', started, repairCount);
        } catch (error) {
            if (this.isCurrentPlacementRepair(generation)) {
                request.error = userFacingMessage(error);
                this.dependencies.setStatus(request.error);
            }
            if (this.dependencies.sessionId() !== null) {
                await this.dependencies
                    .refreshSession({ partitionIndex, volumeName: preferredVolumeName })
                    .catch(() => undefined);
            }
        } finally {
            if (this.isCurrentPlacementRepair(generation)) {
                request.busy = false;
                request.phase = 'idle';
            }
        }
    }

    cancelObjectRename(): void {
        if (!this.objectRenameRequest?.busy) this.objectRenameRequest = null;
    }

    reset(): void {
        ++this.volumeActionGeneration;
        this.volumeAvailable = false;
        this.partitionAvailable = false;
        this.objectRenameAvailable = false;
        this.volumeAction = null;
        this.volumeActionBusy = false;
        this.volumeActionPhase = 'idle';
        this.volumeActionError = '';
        this.volumeDeletionInspection = null;
        ++this.placementRepairGeneration;
        this.placementRepairRequest = null;
        this.objectRenameRequest = null;
    }

    async submitVolumeAction(name: string): Promise<void> {
        if (!this.volumeAction || !this.dependencies.imageOpen()) return;
        const requested = this.volumeAction;
        if (requested.action === 'delete-volume' && !this.volumeDeletionInspection?.canDelete) return;
        const partitionIndex = requested.item.partitionIndex;
        if (partitionIndex === undefined) return;
        const previousVolumeName = requested.item.kind === 'volume' ? requested.item.name : undefined;
        const volumeMutation: VolumeMutation | null =
            requested.action === 'add-volume'
                ? { kind: 'add', partitionIndex, volumeName: name }
                : requested.action === 'rename-volume'
                  ? {
                        kind: 'rename',
                        partitionIndex,
                        volumeName: requested.item.name,
                        newVolumeName: name,
                    }
                  : requested.action === 'delete-volume'
                    ? { kind: 'delete', partitionIndex, volumeName: requested.item.name }
                    : null;
        const partitionMutation: PartitionMutation | null =
            requested.action === 'rename-partition'
                ? {
                      kind: 'rename',
                      partitionIndex,
                      partitionName: requested.item.name,
                      newPartitionName: name,
                  }
                : null;
        const preferredVolumeName =
            requested.action === 'add-volume' || requested.action === 'rename-volume' ? name : undefined;

        this.volumeActionBusy = true;
        this.volumeActionPhase = 'submitting';
        this.volumeActionError = '';
        this.dependencies.setStatus(
            requested.action === 'add-volume'
                ? 'Adding volume'
                : requested.action === 'delete-volume'
                  ? 'Deleting volume'
                  : requested.action === 'rename-partition'
                    ? 'Renaming partition'
                    : 'Renaming volume',
        );
        const sessionId = this.dependencies.sessionId();
        if (sessionId === null) {
            this.volumeActionError = 'Image session is no longer available';
            this.volumeActionBusy = false;
            return;
        }
        const started = performance.now();
        try {
            await this.dependencies.audition.invalidateSession(sessionId);
            const completed = await this.dependencies.jobs.run(
                () =>
                    partitionMutation
                        ? this.dependencies.transport.startPartitionMutation(sessionId, partitionMutation)
                        : this.dependencies.transport.startVolumeMutation(sessionId, volumeMutation!),
                (update) => {
                    if (update.progress?.label) this.dependencies.setStatus(update.progress.label);
                },
            );
            if (completed.status !== 'completed') {
                throw new Error(completed.error ?? 'Image change did not complete');
            }
            ++this.volumeActionGeneration;
            this.volumeAction = null;
            await this.dependencies.refreshSession({ partitionIndex, volumeName: preferredVolumeName });
            this.dependencies.reportTiming(requested.action, started, 1);
        } catch (error) {
            this.volumeActionError = userFacingMessage(error);
            this.dependencies.setStatus(this.volumeActionError);
            if (this.dependencies.sessionId() !== null) {
                await this.dependencies
                    .refreshSession({ partitionIndex, volumeName: previousVolumeName })
                    .catch(() => undefined);
            }
        } finally {
            this.volumeActionBusy = false;
            this.volumeActionPhase = 'idle';
        }
    }

    private async inspectVolumeDeletion(
        requested: { item: DiskTreeItem; action: ImageTreeAction },
        generation: number,
    ): Promise<void> {
        const partitionIndex = requested.item.partitionIndex;
        const sessionId = this.dependencies.sessionId();
        if (partitionIndex === undefined || sessionId === null) {
            if (this.isCurrentVolumeAction(generation)) {
                this.volumeActionError = 'Image session is no longer available';
            }
            return;
        }
        this.volumeActionBusy = true;
        this.volumeActionPhase = 'checking';
        this.dependencies.setStatus('Checking volume relationships');
        try {
            const inspection = await this.dependencies.transport.inspectVolumeDeletion(
                sessionId,
                partitionIndex,
                requested.item.name,
            );
            if (this.isCurrentVolumeAction(generation)) this.volumeDeletionInspection = inspection;
        } catch (error) {
            if (this.isCurrentVolumeAction(generation)) {
                this.volumeActionError = userFacingMessage(error);
                this.dependencies.setStatus(this.volumeActionError);
            }
        } finally {
            if (this.isCurrentVolumeAction(generation)) {
                this.volumeActionBusy = false;
                this.volumeActionPhase = 'idle';
            }
        }
    }

    private requestPlacementRepair(item: DiskTreeItem): boolean {
        if (item.partitionIndex === undefined || this.dependencies.sessionId() === null) return false;
        if (item.kind === 'partition' && !this.partitionAvailable) return false;
        if (item.kind === 'volume' && !this.volumeAvailable) return false;
        if (item.kind !== 'partition' && item.kind !== 'volume') return false;
        const scope: PlacementRepairScope =
            item.kind === 'volume'
                ? { kind: 'VOLUME', partitionIndex: item.partitionIndex, volumeName: item.name }
                : { kind: 'PARTITION', partitionIndex: item.partitionIndex };
        const request = {
            item,
            scope,
            inspection: null,
            busy: true,
            phase: 'inspecting' as const,
            error: '',
            message: '',
        };
        const generation = ++this.placementRepairGeneration;
        this.placementRepairRequest = request;
        void this.inspectPlacementRepair(scope, generation);
        return true;
    }

    private async inspectPlacementRepair(scope: PlacementRepairScope, generation: number): Promise<void> {
        const sessionId = this.dependencies.sessionId();
        if (sessionId === null) return;
        this.dependencies.setStatus('Inspecting object placement');
        try {
            const inspection = await this.dependencies.transport.inspectPlacement(sessionId, scope);
            if (this.isCurrentPlacementRepair(generation)) this.placementRepairRequest!.inspection = inspection;
        } catch (error) {
            if (this.isCurrentPlacementRepair(generation)) {
                this.placementRepairRequest!.error = userFacingMessage(error);
                this.dependencies.setStatus(this.placementRepairRequest!.error);
            }
        } finally {
            if (this.isCurrentPlacementRepair(generation)) {
                this.placementRepairRequest!.busy = false;
                this.placementRepairRequest!.phase = 'idle';
            }
        }
    }

    private isCurrentPlacementRepair(generation: number): boolean {
        return this.placementRepairRequest !== null && this.placementRepairGeneration === generation;
    }

    private isCurrentVolumeAction(generation: number): boolean {
        return this.volumeAction !== null && this.volumeActionGeneration === generation;
    }

    async submitObjectRename(name: string): Promise<void> {
        const request = this.objectRenameRequest;
        const sessionId = this.dependencies.sessionId();
        if (!request || request.busy || sessionId === null) return;
        const target = request.target;
        const selection = this.captureObjectSelection();
        const preferred = {
            partitionIndex: target.object.partitionIndex,
            volumeName: target.object.volumeName,
        };
        const started = performance.now();
        this.objectRenameRequest = { ...request, busy: true, error: '' };
        this.dependencies.setStatus(`Renaming ${target.name}`);
        try {
            await this.dependencies.audition.invalidateSession(sessionId);
            const completed = await this.dependencies.jobs.run(
                () => this.dependencies.transport.startObjectRename(sessionId, this.objectRenameMutation(target, name)),
                (update) => {
                    if (update.progress?.label) this.dependencies.setStatus(update.progress.label);
                },
            );
            if (completed.status !== 'completed') {
                throw new Error(completed.error ?? 'Object rename did not complete');
            }
            await this.dependencies.refreshSession(preferred);
            this.restoreObjectSelection(selection, target.object.key);
            this.objectRenameRequest = null;
            this.dependencies.setStatus(`Renamed ${target.name} to ${name}`);
            this.dependencies.reportTiming('rename-object', started, 1);
        } catch (error) {
            const message = userFacingMessage(error);
            if (this.dependencies.sessionId() === sessionId) {
                await this.dependencies.refreshSession(preferred).catch(() => undefined);
                this.restoreObjectSelection(selection, target.object.key);
            }
            if (this.objectRenameRequest?.target.object.key === target.object.key) {
                this.objectRenameRequest = { ...this.objectRenameRequest, busy: false, error: message };
            }
            this.dependencies.setStatus(message);
        }
    }

    private captureObjectSelection(): ObjectSelectionSnapshot {
        const catalog = this.dependencies.catalog;
        return {
            view: this.dependencies.workspaceView(),
            programId: catalog.selectedProgramId,
            sequenceId: catalog.selectedSequenceId,
            bankId: catalog.selectedBankId,
            bankMemberId: catalog.selectedBankMemberId,
            sampleId: catalog.selectedSampleId,
            bankWaveDataId: catalog.selectedBankWaveDataId,
            sampleWaveDataId: catalog.selectedSampleWaveDataId,
            waveDataId: catalog.selectedWaveDataId,
            inspectorId: catalog.inspectorObjectId,
            editorIds: { ...catalog.editorObjectIds },
        };
    }

    private restoreObjectSelection(snapshot: ObjectSelectionSnapshot, renamedObjectId: string): void {
        const catalog = this.dependencies.catalog;
        const exists = (objectId: string): boolean => Boolean(objectId && catalog.objectsById.has(objectId));
        this.dependencies.setWorkspaceView(snapshot.view);
        catalog.selectedProgramId = exists(snapshot.programId) ? snapshot.programId : '';
        catalog.selectedSequenceId = exists(snapshot.sequenceId) ? snapshot.sequenceId : '';
        catalog.selectedBankId = exists(snapshot.bankId) ? snapshot.bankId : '';
        catalog.selectedBankMemberId = exists(snapshot.bankMemberId) ? snapshot.bankMemberId : '';
        catalog.selectedSampleId = exists(snapshot.sampleId) ? snapshot.sampleId : '';
        catalog.selectedBankWaveDataId = exists(snapshot.bankWaveDataId) ? snapshot.bankWaveDataId : '';
        catalog.selectedSampleWaveDataId = exists(snapshot.sampleWaveDataId) ? snapshot.sampleWaveDataId : '';
        catalog.selectedWaveDataId = exists(snapshot.waveDataId) ? snapshot.waveDataId : '';
        catalog.inspectorObjectId = exists(renamedObjectId)
            ? renamedObjectId
            : exists(snapshot.inspectorId)
              ? snapshot.inspectorId
              : '';
        catalog.editorObjectIds = {
            programs: exists(snapshot.editorIds.programs) ? snapshot.editorIds.programs : '',
            sequences: exists(snapshot.editorIds.sequences) ? snapshot.editorIds.sequences : '',
            'sample-banks': exists(snapshot.editorIds['sample-banks']) ? snapshot.editorIds['sample-banks'] : '',
            samples: exists(snapshot.editorIds.samples) ? snapshot.editorIds.samples : '',
            'wave-data': exists(snapshot.editorIds['wave-data']) ? snapshot.editorIds['wave-data'] : '',
        };
    }

    private objectRenameMutation(target: ObjectRenameTarget, name: string): ObjectRenameMutation {
        const common = {
            partitionIndex: target.object.partitionIndex,
            volumeName: target.object.volumeName,
        };
        if (target.kind === 'program') {
            return {
                ...common,
                kind: 'program',
                programNumber: target.programNumber,
                newProgramName: name,
            };
        }
        if (target.kind === 'sample-bank') {
            return {
                ...common,
                kind: 'sample-bank',
                sampleBankName: target.name,
                newSampleBankName: name,
            };
        }
        if (target.kind === 'sample') {
            return {
                ...common,
                kind: 'sample',
                sampleName: target.name,
                newSampleName: name,
            };
        }
        if (target.kind === 'sequence') {
            return {
                ...common,
                kind: 'sequence',
                sequenceName: target.name,
                newSequenceName: name,
            };
        }
        return {
            ...common,
            kind: 'wave-data',
            waveformName: target.name,
            newWaveformName: name,
        };
    }
}
