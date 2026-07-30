import type { CatalogWorkflow } from '../catalog/workflow.svelte';
import type { AuditionWorkflow } from '../audition/workflow.svelte';
import type { JobController } from '../jobs/actions';
import type { ImageTransport, ObjectRenameMutation, PartitionMutation, VolumeMutation } from '../../lib/transport';
import type { DiskTreeItem, ImageTreeAction, ObjectRenameTarget, WorkspaceView } from '../../lib/types';
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
    volumeAvailable = $state(false);
    partitionAvailable = $state(false);
    objectRenameAvailable = $state(false);
    volumeAction = $state<{ item: DiskTreeItem; action: ImageTreeAction } | null>(null);
    volumeActionBusy = $state(false);
    volumeActionError = $state('');
    objectRenameRequest = $state<{
        target: ObjectRenameTarget;
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
        const partitionAction = action === 'rename-partition';
        if (partitionAction && (!this.partitionAvailable || item.kind !== 'partition')) return false;
        if (!partitionAction && !this.volumeAvailable) return false;
        if (action === 'add-volume' && item.kind !== 'partition') return false;
        if ((action === 'rename-volume' || action === 'delete-volume') && item.kind !== 'volume') return false;
        this.volumeActionError = '';
        this.volumeAction = { item, action };
        return true;
    }

    requestObjectRename(target: ObjectRenameTarget): void {
        if (!this.objectRenameAvailable || this.dependencies.sessionId() === null) return;
        this.objectRenameRequest = { target, busy: false, error: '' };
    }

    cancelVolumeAction(): void {
        if (!this.volumeActionBusy) this.volumeAction = null;
    }

    cancelObjectRename(): void {
        if (!this.objectRenameRequest?.busy) this.objectRenameRequest = null;
    }

    reset(): void {
        this.volumeAvailable = false;
        this.partitionAvailable = false;
        this.objectRenameAvailable = false;
        this.volumeAction = null;
        this.volumeActionBusy = false;
        this.volumeActionError = '';
        this.objectRenameRequest = null;
    }

    async submitVolumeAction(name: string): Promise<void> {
        if (!this.volumeAction || !this.dependencies.imageOpen()) return;
        const requested = this.volumeAction;
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
        }
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
