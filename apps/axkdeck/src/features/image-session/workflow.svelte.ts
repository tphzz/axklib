import { DocumentSelection } from './documentSelection';
import { loadIntegrityIssues } from './integrity';
import { collectVolumes, findPartition, findSourceItem, noImageSource } from './sourceTree';
import { imageLocation } from '../../lib/httpImageSources';
import { serverDirectoryLocation, locationKey } from '../../lib/storageLocations';
import type { DirectoryLocation, DirectoryRef, FileLocation, FileRef, ImageLocation } from '../../lib/storageLocations';
import type {
    CompanionSelection,
    FloppySetSummary,
    ImageTransport,
    ImageValidationIssue,
    JobState,
    OpenedImage,
} from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import type { ObjectSelectionMode } from '../../lib/objectSelection';
import { emptyVolumeSelection, updateVolumeSelection, type VolumeSelectionState } from '../../lib/volumeSelection';
import { AxklibApiError } from '../../lib/httpErrors';
import { userFacingMessage } from '../../lib/userFacingMessage';
import { ImageOpenDiagnostics } from './diagnostics';
import type { SessionCollaborators } from './collaborators';
import type { PickerController } from '../dialogs/picker';
import type { ExportCompanionRetry } from '../export/workflow.svelte';
import { ImageSessionController, type ImageSessionPreparation } from './actions';
import { allocationBlockerKey } from './validationStatus';
import { applyOpenedImage } from './openedImageMetadata';

export type CompanionRetry =
    { kind: 'audition'; objectId: string } | { kind: 'sample-bank'; bankId: string } | ExportCompanionRetry;

type CompanionSourceKind = 'file' | 'directory';

const imageSessionKeepAliveIntervalMs = 5 * 60 * 1000;
const imageOpenProgressDelayMs = 750;

export class ImageSessionWorkflow {
    confirmEditorLeave: () => Promise<boolean> = async () => true;
    canSwitchDocument: () => boolean = () => true;
    sourceItems = $state<DiskTreeItem[]>([]);
    volumeSelection = $state<VolumeSelectionState>(emptyVolumeSelection());
    selectedSource = $state<DiskTreeItem>({
        id: 'none',
        name: 'No image',
        kind: 'disk',
        childCount: 0,
    });
    location = $state<ImageLocation | null>(null);
    sessionId = $state<number | null>(null);
    documents = $state<OpenedImage['documents']>([]);
    documentId = $state<string | null>(null);
    sources = $state<OpenedImage['sources']>([]);
    activeSourceMemberId = $state<string | null>(null);
    discovery = $state<OpenedImage['discovery']>({ selectionRequired: false, candidates: [] });
    switchingDocument = $state(false);
    private documentSelection = new DocumentSelection();
    companionSources = $state<ImageLocation[]>([]);
    floppySet = $state<FloppySetSummary | null>(null);
    opening = $state(false);
    openProgressVisible = $state(false);
    openProgressLabel = $state('Opening image source');
    openProgressCompleted = $state(0);
    openProgressTotal = $state<number | undefined>(undefined);
    openProgressCancellable = $state(false);
    openProgressCancelling = $state(false);
    status = $state('Ready');
    hardDiskDirectory = $state<DirectoryLocation | null>(null);
    companionRequest = $state<{
        requestId: number;
        sources: ImageLocation[];
        retry: CompanionRetry | null;
        sourceKind: CompanionSourceKind;
        setLabel: string;
        nextRequiredIndex: number | null;
        requirements: OpenedImage['documents'][number]['companions']['requirements'];
        canSearchNearby: boolean;
        busy: boolean;
        error: string;
    } | null>(null);
    objectDeletionAvailable = $state(false);
    waveDataCleanupAvailable = $state(false);
    programGenerationAvailable = $state(false);
    programAssignmentCleanupAvailable = $state(false);
    packageImportAvailable = $state(false);
    packageExportAvailable = $state(false);
    volumePackageExportAvailable = $state(false);
    volumeFloppyExportAvailable = $state(false);
    audioExportAvailable = $state(false);
    sequenceExportAvailable = $state(false);
    mediaConversionAvailable = $state(false);
    extentLayoutRepairAvailable = $state(false);
    allocationInspectionAvailable = $state(false);
    imageFormat = $state<string | null>(null);
    revision = $state(0);
    integrityDialogOpen = $state(false);
    integrityIssues = $state<ImageValidationIssue[]>([]);
    integrityLoading = $state(false);
    integrityError = $state('');

    private readonly controller: ImageSessionController;
    private collaborators: SessionCollaborators | null = null;
    private nextCompanionRequestId = 1;
    private lastImageDirectory = $state<DirectoryRef | null>(null);
    private lastOpenedImageFile = $state<FileRef | null>(null);
    private lastCompanionDirectory = $state<DirectoryRef | null>(null);
    private lastAutomaticIntegrityKey = '';
    private automaticCapacityWarnings = new Set<string>();
    private nextOpenRequestId = 1;
    private openProgressTimer: ReturnType<typeof setTimeout> | null = null;
    private openProgressActive = false;
    private openStartedAt = 0;
    private readonly diagnostics = new ImageOpenDiagnostics();
    private leaseMaintenanceActive = false;
    private leaseTimer: number | null = null;
    private readonly renewVisibleLease = (): void => {
        if (typeof document === 'undefined' || document.visibilityState === 'visible') void this.maintainLease();
    };

    constructor(
        private readonly transport: ImageTransport,
        private readonly picker: PickerController,
    ) {
        this.controller = new ImageSessionController(
            transport,
            (sessionId) => this.requireCollaborators().audition.invalidateSession(sessionId),
            (snapshot) => {
                this.sessionId = snapshot.sessionId;
                this.location = snapshot.location;
                this.opening = snapshot.opening;
            },
        );
    }

    connect(collaborators: SessionCollaborators): void {
        if (this.collaborators) throw new Error('Image session workflow is already connected');
        this.collaborators = collaborators;
        this.startLeaseMaintenance();
    }

    setStatus(status: string): void {
        this.status = status;
    }

    get activeLocation(): ImageLocation | null {
        const member = this.sources.find((source) => source.id === this.activeSourceMemberId);
        if (!member) return null;
        return member.source.kind === 'DEVICE' ? this.location : imageLocation(member.source);
    }

    async showIntegrity(): Promise<void> {
        if (this.sessionId === null) return;
        this.integrityDialogOpen = true;
        await loadIntegrityIssues(this, this.transport, this.sessionId);
    }

    closeIntegrity(): void {
        this.integrityDialogOpen = false;
    }

    selectSource(item: DiskTreeItem): void {
        this.selectTreeSource(item, 'replace', collectVolumes(this.sourceItems));
    }

    selectTreeSource(item: DiskTreeItem, mode: ObjectSelectionMode, visibleVolumes: readonly DiskTreeItem[]): void {
        const update = updateVolumeSelection(this.volumeSelection, visibleVolumes, item, mode);
        this.volumeSelection = update.selection;
        this.activateSource(update.active);
    }

    selectSourceForContext(item: DiskTreeItem, visibleVolumes: readonly DiskTreeItem[]): void {
        if (item.kind === 'volume' && this.volumeSelection.items.some((candidate) => candidate.id === item.id)) {
            this.activateSource(item);
            return;
        }
        this.selectTreeSource(item, 'replace', visibleVolumes);
    }

    importDestinationSource(): DiskTreeItem {
        const selected = this.volumeSelection.items;
        if (selected.length === 1) return selected[0]!;
        if (selected.length > 1) {
            const partitionIndex = selected[0]?.partitionIndex;
            if (partitionIndex !== undefined && selected.every((item) => item.partitionIndex === partitionIndex)) {
                return (
                    findPartition(this.sourceItems, partitionIndex) ?? {
                        id: `selected-partition-${partitionIndex}`,
                        name: `Partition ${partitionIndex + 1}`,
                        kind: 'partition',
                        childCount: 0,
                        partitionIndex,
                    }
                );
            }
            return noImageSource();
        }
        return this.selectedSource.kind === 'volume' ? noImageSource() : this.selectedSource;
    }

    private activateSource(item: DiskTreeItem): void {
        const { catalog } = this.requireCollaborators();
        this.selectedSource = item;
        if (item.kind !== 'volume') {
            catalog.clear();
            return;
        }
        if (item.id !== catalog.activeVolumeId) void catalog.loadVolume(item.id, item.partitionIndex ?? null);
    }

    currentSourcePreference(): { partitionIndex: number; volumeName?: string } | undefined {
        if (this.selectedSource.partitionIndex === undefined) return undefined;
        return {
            partitionIndex: this.selectedSource.partitionIndex,
            volumeName:
                this.selectedSource.kind === 'volume' ? this.selectedSource.name : this.selectedSource.volumeName,
        };
    }

    async open(
        locations: ImageLocation[],
        preferred?: { partitionIndex: number; volumeName?: string },
        prepare?: ImageSessionPreparation,
    ): Promise<void> {
        if (!(await this.confirmEditorLeave())) return;
        const location = locations[0];
        if (!location) return;
        const requestId = this.nextOpenRequestId++;
        this.beginOpenProgress(requestId, location);
        this.status = 'Opening image';
        try {
            const opened = await this.controller.open(
                locations,
                (job) => this.updateOpenProgress(requestId, job),
                prepare,
            );
            if (!opened || requestId !== this.nextOpenRequestId - 1) return;
            this.openProgressCancellable = false;
            this.openProgressCancelling = false;
            this.openProgressLabel = 'Preparing workspace';
            this.diagnostics.report('image_open_server_completed', {
                sessionId: opened.sessionId,
                elapsedMs: this.openElapsedMs(),
            });
            this.lastOpenedImageFile = location.kind === 'server-file' ? location.reference : null;
            this.documentSelection.clear();
            await this.applyOpenedImage(opened, preferred);
            this.diagnostics.report('image_open_workspace_ready', {
                sessionId: opened.sessionId,
                elapsedMs: this.openElapsedMs(),
            });
        } catch (error) {
            if (requestId !== this.nextOpenRequestId - 1) return;
            if (error instanceof DOMException && error.name === 'AbortError') {
                this.status = 'Image opening cancelled';
                this.diagnostics.report('image_open_cancelled', {
                    elapsedMs: this.openElapsedMs(),
                });
            } else {
                this.status = userFacingMessage(error);
                this.diagnostics.report(
                    'image_open_failed',
                    {
                        elapsedMs: this.openElapsedMs(),
                        message: userFacingMessage(error),
                    },
                    'error',
                );
            }
        } finally {
            if (requestId === this.nextOpenRequestId - 1) this.finishOpenProgress();
        }
    }

    cancelOpen(): void {
        if (!this.openProgressCancellable || this.openProgressCancelling) return;
        this.openProgressCancelling = true;
        this.openProgressLabel = 'Cancelling image open';
        this.diagnostics.report('image_open_cancel_requested', { elapsedMs: this.openElapsedMs() });
        this.controller.cancelOpen();
    }

    cleanupOpen(): Promise<void> {
        return this.controller.cleanupOpen();
    }

    async reopen(preferred?: { partitionIndex: number; volumeName?: string }): Promise<void> {
        if (this.sources.length)
            await this.open(
                this.sources.map((source) => imageLocation(source.source)),
                preferred,
            );
    }

    async refresh(
        preferred?: { partitionIndex: number; volumeName?: string },
        mode: 'reload' | 'editor' = 'reload',
    ): Promise<void> {
        if (this.sessionId === null) throw new Error('Image session is no longer available');
        const sourceId = this.selectedSource.id;
        const opened = await this.controller.refresh();
        if (mode === 'editor' && (!opened || sourceId !== this.selectedSource.id))
            throw new Error('The workspace changed during refresh');
        if (opened) await this.applyOpenedImage(opened, preferred, mode === 'editor');
    }

    async maintainLease(): Promise<void> {
        if (this.leaseMaintenanceActive || this.sessionId === null || this.opening) return;
        const sessionId = this.sessionId;
        this.leaseMaintenanceActive = true;
        try {
            await this.controller.keepAlive();
        } catch (error) {
            if (
                error instanceof AxklibApiError &&
                error.code === 'image_not_found' &&
                this.sessionId === sessionId &&
                this.location?.kind !== 'device'
            ) {
                await this.reopen(this.currentSourcePreference());
            }
        } finally {
            this.leaseMaintenanceActive = false;
        }
    }

    async chooseAndOpen(): Promise<void> {
        const selected = await this.chooseImageLocation();
        if (selected) await this.open(selected);
    }

    selectDocument(documentId: string | null | undefined, sourceMemberId?: string): Promise<void> {
        return this.documentSelection.change(
            this,
            this.requireCollaborators(),
            this.transport,
            { documentId, sourceMemberId },
            (opened, preferred) => this.applyOpenedImage(opened, preferred),
        );
    }
    resolveDocuments(formats: string[]): Promise<void> {
        return this.documentSelection.change(this, this.requireCollaborators(), this.transport, { formats }, (opened) =>
            this.applyOpenedImage(opened),
        );
    }

    async chooseHardDiskDirectory(): Promise<void> {
        if (this.transport.storageMode !== 'server') return;
        const selection = await this.picker.chooseLocation('directory', 'Choose image location', [], '', {
            parentDialog: 'create',
            initialDirectory: this.lastImageDirectory,
            ondirectorychange: (directory) => (this.lastImageDirectory = directory),
        });
        if (selection?.kind === 'server-directory') this.hardDiskDirectory = selection;
    }

    finishHardDiskCreation(file: FileLocation): void {
        this.hardDiskDirectory = null;
        void this.open([file]);
    }

    requestCompanionDisks(retry: CompanionRetry | null = null): void {
        if (this.sessionId === null || !this.location) return;
        this.openCompanionRequest(retry);
    }

    async addCompanionDiskSource(): Promise<void> {
        const request = this.companionRequest;
        if (!request || request.busy) return;
        const selection = await this.picker.chooseLocation(
            request.sourceKind === 'file' ? 'file' : 'directory',
            request.sourceKind === 'file' ? 'Choose companion floppy image' : 'Choose companion disk folder',
            request.sourceKind === 'file' ? ['ima', 'img'] : [],
            '',
            {
                parentDialog: 'companion-disks',
                initialDirectory:
                    this.lastCompanionDirectory ??
                    (this.activeLocation && this.activeLocation.kind !== 'device'
                        ? {
                              rootId: this.activeLocation.reference.rootId,
                              relativePath: this.activeLocation.reference.relativePath.replace(/\/?[^/]+\/?$/, ''),
                          }
                        : null),
                ondirectorychange: (directory) => (this.lastCompanionDirectory = directory),
                requireWritableDirectory: false,
            },
        );
        if (!selection || this.companionRequest?.requestId !== request.requestId) return;
        const source =
            request.sourceKind === 'file' && selection.kind === 'server-file'
                ? selection
                : request.sourceKind === 'directory' && selection.kind === 'server-directory'
                  ? serverDirectoryLocation(selection.reference, selection.displayName)
                  : null;
        if (!source || request.sources.some((candidate) => sameImageSource(candidate, source))) return;
        this.companionRequest = {
            ...request,
            sources: [...request.sources, source],
            error: '',
        };
    }

    removeCompanionDiskSource(source: ImageLocation): void {
        if (!this.companionRequest || this.companionRequest.busy) return;
        this.companionRequest = {
            ...this.companionRequest,
            sources: this.companionRequest.sources.filter((candidate) => !sameImageSource(candidate, source)),
            error: '',
        };
    }

    cancelCompanionDisks(): void {
        if (!this.companionRequest?.busy) this.companionRequest = null;
    }

    async attachCompanionDisks(selection: CompanionSelection): Promise<void> {
        const request = this.companionRequest;
        if (!request || request.busy || this.sessionId === null) return;
        const sessionId = this.sessionId;
        const preferred = this.currentSourcePreference();
        const pending = { ...request, busy: true, error: '' };
        this.companionRequest = pending;
        try {
            const collaborators = this.requireCollaborators();
            await collaborators.audition.invalidateSession(sessionId);
            const opened = await this.transport.attachCompanions(sessionId, selection);
            if (this.sessionId !== sessionId || this.companionRequest?.requestId !== request.requestId) return;
            this.companionRequest = null;
            await this.applyOpenedImage(opened, preferred);
            const requirements =
                this.documents.find((document) => document.id === this.documentId)?.companions.requirements ?? [];
            if (requirements.some((item) => item.state === 'MISSING')) this.openCompanionRequest(request.retry);
            else if (request.retry && requirements.length === 0) await this.retryCompanionAction(request.retry);
        } catch (error) {
            if (
                this.sessionId === sessionId &&
                (!this.companionRequest || this.companionRequest.requestId === request.requestId)
            ) {
                this.companionRequest = {
                    ...request,
                    busy: false,
                    error: userFacingMessage(error),
                };
            }
        }
    }

    async close(): Promise<void> {
        if (!(await this.confirmEditorLeave())) return;
        if (!this.location && this.sessionId === null) return;
        this.status = 'Closing image';
        try {
            await this.closeOpenSession();
            this.sourceItems = [];
            this.volumeSelection = emptyVolumeSelection();
            this.selectedSource = noImageSource();
            this.requireCollaborators().catalog.clear();
            this.status = 'Ready';
        } catch (error) {
            this.status = userFacingMessage(error);
            throw error;
        }
    }

    async dispose(): Promise<void> {
        this.stopLeaseMaintenance();
        this.clearOpenProgressTimer();
        this.controller.cancelOpen();
        await this.controller.dispose();
    }

    private beginOpenProgress(requestId: number, location: ImageLocation): void {
        this.clearOpenProgressTimer();
        this.openStartedAt = performance.now();
        this.diagnostics.begin(requestId);
        this.openProgressVisible = false;
        this.openProgressActive = true;
        this.openProgressLabel = 'Opening image source';
        this.openProgressCompleted = 0;
        this.openProgressTotal = undefined;
        this.openProgressCancellable = true;
        this.openProgressCancelling = false;
        this.openProgressTimer = setTimeout(() => {
            if (requestId === this.nextOpenRequestId - 1 && this.openProgressActive) this.openProgressVisible = true;
        }, imageOpenProgressDelayMs);
        this.diagnostics.report('image_open_requested', {
            sourceKind: location.kind,
            displayName: location.displayName,
        });
    }

    private updateOpenProgress(requestId: number, job: JobState): void {
        if (requestId !== this.nextOpenRequestId - 1) return;
        if (job.status === 'completed') {
            this.openProgressLabel = 'Preparing workspace';
            this.openProgressCancellable = false;
            this.openProgressCancelling = false;
        } else if (job.progress) {
            this.openProgressLabel = job.progress.label;
            this.openProgressCompleted = job.progress.completed;
            this.openProgressTotal = job.progress.total;
        }
        this.openProgressCancelling = job.status === 'cancelling';
        this.diagnostics.progress(job, this.openElapsedMs());
    }

    private finishOpenProgress(): void {
        this.clearOpenProgressTimer();
        this.openProgressActive = false;
        this.openProgressVisible = false;
        this.openProgressCancellable = false;
        this.openProgressCancelling = false;
    }

    private clearOpenProgressTimer(): void {
        if (this.openProgressTimer === null) return;
        clearTimeout(this.openProgressTimer);
        this.openProgressTimer = null;
    }

    private openElapsedMs(): number {
        return Math.round(performance.now() - this.openStartedAt);
    }

    private startLeaseMaintenance(): void {
        if (typeof window === 'undefined' || this.leaseTimer !== null) return;
        this.leaseTimer = window.setInterval(() => void this.maintainLease(), imageSessionKeepAliveIntervalMs);
        window.addEventListener('focus', this.renewVisibleLease);
        document.addEventListener('visibilitychange', this.renewVisibleLease);
    }

    private stopLeaseMaintenance(): void {
        if (typeof window === 'undefined' || this.leaseTimer === null) return;
        window.clearInterval(this.leaseTimer);
        this.leaseTimer = null;
        window.removeEventListener('focus', this.renewVisibleLease);
        document.removeEventListener('visibilitychange', this.renewVisibleLease);
    }

    private requireCollaborators(): SessionCollaborators {
        if (!this.collaborators) throw new Error('Image session workflow is not connected');
        return this.collaborators;
    }

    private async chooseImageLocation(): Promise<ImageLocation[] | null> {
        if (this.transport.storageMode !== 'server') return null;
        const selection = await this.picker.chooseSources({
            purpose: 'open-source',
            initialDirectory: this.lastImageDirectory,
            initialFile: this.lastOpenedImageFile,
            ondirectorychange: (directory) => (this.lastImageDirectory = directory),
        });
        return selection;
    }

    private openCompanionRequest(retry: CompanionRetry | null): void {
        const document = this.documents.find((document) => document.id === this.documentId);
        if (!document?.companions.canAttach) return;
        const memberId = document.sourceMemberIds[0];
        const source = this.sources.find((member) => member.id === memberId)?.source;
        if (!source || source.kind === 'DEVICE') return;
        this.companionRequest = {
            requestId: this.nextCompanionRequestId++,
            sources: [...this.companionSources],
            retry,
            sourceKind: source.kind === 'FILE' ? 'file' : 'directory',
            setLabel: this.floppySet?.setLabel.trim() || imageLocation(source).displayName.split(/[\\/]/).at(-1) || '',
            nextRequiredIndex: this.floppySet?.nextRequiredIndex ?? null,
            requirements: document.companions.requirements,
            canSearchNearby: document.companions.canSearchNearby,
            busy: false,
            error: '',
        };
    }

    private async applyOpenedImage(
        opened: OpenedImage,
        preferred?: { partitionIndex: number; volumeName?: string },
        preserve = false,
    ): Promise<void> {
        await applyOpenedImage(this, this.requireCollaborators(), opened, preferred, preserve);
        if (
            opened.validation.errorCount > 0 ||
            opened.validation.warningCount > 0 ||
            opened.sources.some((source) => source.diagnostics.length)
        )
            await this.showAutomaticIntegrityIssues(opened);
        else this.automaticCapacityWarnings.clear();
        if (
            this.documents
                .find((document) => document.id === this.documentId)
                ?.companions.requirements.some((item) => item.state === 'MISSING')
        )
            this.openCompanionRequest(null);
    }

    private async showAutomaticIntegrityIssues(opened: OpenedImage): Promise<void> {
        await loadIntegrityIssues(this, this.transport, opened.sessionId, opened.revision);
        if (this.sessionId !== opened.sessionId || this.revision !== opened.revision || this.integrityError) return;
        const capacityWarnings = new Set<string>();
        for (const issue of this.integrityIssues) {
            if (issue.code !== 'EX5_CAPACITY_EXCEEDS_IMAGE' && issue.code !== 'EX5_FILE_DATA_UNAVAILABLE') continue;
            const key = JSON.stringify([opened.sessionId, issue.code, issue.samplerPath, issue.message]);
            if (!this.automaticCapacityWarnings.has(key)) this.integrityDialogOpen = true;
            capacityWarnings.add(key);
        }
        this.automaticCapacityWarnings = capacityWarnings;
        const automaticKey = allocationBlockerKey(this.integrityIssues, opened.sessionId, opened.revision);
        if (!automaticKey) return;
        if (automaticKey === this.lastAutomaticIntegrityKey) return;
        this.lastAutomaticIntegrityKey = automaticKey;
        this.integrityDialogOpen = true;
    }

    private async retryCompanionAction(retry: CompanionRetry): Promise<void> {
        const { audition, catalog, exports } = this.requireCollaborators();
        if (retry.kind === 'audition') {
            audition.playObject(retry.objectId);
            return;
        }
        if (retry.kind === 'sample-bank') {
            const bank = catalog.sampleBanks.find((candidate) => candidate.objectId === retry.bankId);
            if (bank) await audition.playSampleBank(bank);
            return;
        }
        if (retry.kind === 'package-export') {
            await exports.runPackage(retry.destination, retry.localDestination);
            return;
        }
        await exports.runAudio(retry.destination, retry.format, retry.localDestination);
    }

    private async closeOpenSession(): Promise<void> {
        if (this.sessionId === null) return;
        const collaborators = this.requireCollaborators();
        collaborators.exports.dispose();
        collaborators.volumePackages.dispose();
        collaborators.volumeFloppies.dispose();
        collaborators.mediaExports.dispose();
        collaborators.extentRepairs.dispose();
        this.companionRequest = null;
        await collaborators.packageImport.dispose();
        await collaborators.floppyImport?.dispose();
        await this.controller.close();
        collaborators.clearExportSelection();
        this.companionSources = [];
        this.documents = [];
        this.documentId = null;
        this.sources = [];
        this.activeSourceMemberId = null;
        this.documentSelection.clear();
        this.floppySet = null;
        this.integrityDialogOpen = false;
        this.integrityIssues = [];
        this.integrityLoading = false;
        this.integrityError = '';
        this.lastAutomaticIntegrityKey = '';
        this.automaticCapacityWarnings.clear();
        collaborators.mutation.reset();
        this.objectDeletionAvailable = false;
        this.waveDataCleanupAvailable = false;
        this.packageImportAvailable = false;
        this.packageExportAvailable = false;
        this.volumePackageExportAvailable = false;
        this.volumeFloppyExportAvailable = false;
        this.audioExportAvailable = false;
        this.sequenceExportAvailable = false;
        this.mediaConversionAvailable = false;
        this.extentLayoutRepairAvailable = false;
        this.allocationInspectionAvailable = false;
        this.imageFormat = null;
        this.revision = 0;
        collaborators.deletion.dispose();
        collaborators.programGeneration.dispose();
        this.programGenerationAvailable = false;
        this.programAssignmentCleanupAvailable = false;
    }
}

function sameImageSource(left: ImageLocation, right: ImageLocation): boolean {
    return left.kind === right.kind && locationKey(left) === locationKey(right);
}
