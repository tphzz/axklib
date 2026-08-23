import { axkObjectDirectoryLocation } from '../../lib/storageLocations';
import type { DirectoryLocation, DirectoryRef, FileLocation, FileRef, ImageLocation } from '../../lib/storageLocations';
import type {
    CompanionSelection,
    FloppySetSummary,
    ImageTransport,
    ImageValidationIssue,
    OpenedImage,
} from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import type { ObjectSelectionMode } from '../../lib/objectSelection';
import { emptyVolumeSelection, updateVolumeSelection, type VolumeSelectionState } from '../../lib/volumeSelection';
import { AxklibApiError } from '../../lib/httpErrors';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { AuditionWorkflow } from '../audition/workflow.svelte';
import type { CatalogWorkflow } from '../catalog/workflow.svelte';
import type { DeletionWorkflow } from '../deletion/workflow.svelte';
import type { PickerController } from '../dialogs/picker';
import type { ExportCompanionRetry, ExportWorkflow } from '../export/workflow.svelte';
import type { MediaExportWorkflow } from '../export/mediaWorkflow.svelte';
import type { VolumePackageExportWorkflow } from '../export/volumePackageWorkflow.svelte';
import type { VolumeFloppyExportWorkflow } from '../export/volumeFloppyWorkflow.svelte';
import { ImageSessionController } from './actions';
import type { PackageImportWorkflow } from '../import/packageWorkflow.svelte';
import type { MutationWorkflow } from '../mutation/workflow.svelte';
import type { ProgramGenerationWorkflow } from '../program-generation/workflow.svelte';
import type { ExtentLayoutRepairWorkflow } from './extentLayoutRepairWorkflow.svelte';
import { validationStatus } from './validationStatus';

export type CompanionRetry =
    { kind: 'audition'; objectId: string } | { kind: 'sample-bank'; bankId: string } | ExportCompanionRetry;

type CompanionSourceKind = 'file' | 'directory';

const allocationBlockerCodes = new Set([
    'SFS_ALLOCATION_BITMAP_COPIES_DIFFER',
    'SFS_ALLOCATION_CROSS_LINK',
    'SFS_ALLOCATION_MISMATCH',
    'SFS_EXTENT_BYTE_TOTAL_MISMATCH',
]);
const imageSessionKeepAliveIntervalMs = 5 * 60 * 1000;

interface SessionCollaborators {
    catalog: CatalogWorkflow;
    audition: AuditionWorkflow;
    mutation: MutationWorkflow;
    exports: ExportWorkflow;
    volumePackages: VolumePackageExportWorkflow;
    volumeFloppies: VolumeFloppyExportWorkflow;
    mediaExports: MediaExportWorkflow;
    packageImport: PackageImportWorkflow;
    deletion: DeletionWorkflow;
    programGeneration: ProgramGenerationWorkflow;
    extentRepairs: ExtentLayoutRepairWorkflow;
    clearExportSelection: () => void;
}

export class ImageSessionWorkflow {
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
    companionSources = $state<ImageLocation[]>([]);
    floppySet = $state<FloppySetSummary | null>(null);
    opening = $state(false);
    status = $state('Ready');
    hardDiskDirectory = $state<DirectoryLocation | null>(null);
    companionRequest = $state<{
        requestId: number;
        sources: ImageLocation[];
        retry: CompanionRetry | null;
        sourceKind: CompanionSourceKind;
        setLabel: string;
        nextRequiredIndex: number | null;
        busy: boolean;
        error: string;
    } | null>(null);
    objectDeletionAvailable = $state(false);
    waveDataCleanupAvailable = $state(false);
    programGenerationAvailable = $state(false);
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

    async showIntegrity(): Promise<void> {
        if (this.sessionId === null) return;
        this.integrityDialogOpen = true;
        await this.loadIntegrityIssues(this.sessionId);
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

    async open(location: ImageLocation, preferred?: { partitionIndex: number; volumeName?: string }): Promise<void> {
        this.status = 'Opening image';
        try {
            const opened = await this.controller.open(location);
            if (opened) {
                this.lastOpenedImageFile = location.kind === 'server-file' ? location.reference : null;
                await this.applyOpenedImage(opened, preferred);
            }
        } catch (error) {
            this.status = userFacingMessage(error);
        }
    }

    async reopen(preferred?: { partitionIndex: number; volumeName?: string }): Promise<void> {
        if (this.location) await this.open(this.location, preferred);
    }

    async refresh(preferred?: { partitionIndex: number; volumeName?: string }): Promise<void> {
        if (this.sessionId === null) throw new Error('Image session is no longer available');
        const opened = await this.controller.refresh();
        if (opened) await this.applyOpenedImage(opened, preferred);
    }

    async maintainLease(): Promise<void> {
        if (this.leaseMaintenanceActive || this.sessionId === null || this.opening) return;
        const sessionId = this.sessionId;
        this.leaseMaintenanceActive = true;
        try {
            await this.controller.keepAlive();
        } catch (error) {
            if (error instanceof AxklibApiError && error.code === 'image_not_found' && this.sessionId === sessionId) {
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

    async chooseHardDiskDirectory(): Promise<void> {
        if (this.transport.storageMode !== 'server') return;
        const selection = await this.picker.chooseLocation('directory', 'Choose image location', [], '', {
            initialDirectory: this.lastImageDirectory,
            ondirectorychange: (directory) => (this.lastImageDirectory = directory),
        });
        if (selection?.kind === 'server-directory') this.hardDiskDirectory = selection;
    }

    finishHardDiskCreation(file: FileLocation): void {
        this.hardDiskDirectory = null;
        void this.open(file);
    }

    cancelHardDiskCreation(): void {
        this.hardDiskDirectory = null;
    }

    requestCompanionDisks(retry: CompanionRetry): void {
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
                initialDirectory: this.lastCompanionDirectory,
                ondirectorychange: (directory) => (this.lastCompanionDirectory = directory),
                requireWritableDirectory: false,
            },
        );
        if (!selection || this.companionRequest?.requestId !== request.requestId) return;
        const source =
            request.sourceKind === 'file' && selection.kind === 'server-file'
                ? selection
                : request.sourceKind === 'directory' && selection.kind === 'server-directory'
                  ? axkObjectDirectoryLocation(selection.reference, selection.displayName)
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
            if (request.retry) await this.retryCompanionAction(request.retry);
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
        await this.controller.dispose();
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

    private async chooseImageLocation(): Promise<ImageLocation | null> {
        if (this.transport.storageMode !== 'server') return null;
        const selection = await this.picker.chooseLocation(
            'media-source',
            'Open image',
            ['hds', 'hda', 'ima', 'img', 'iso', 'a3k', 'zip'],
            '',
            {
                initialDirectory: this.lastImageDirectory,
                initialFile: this.lastOpenedImageFile,
                ondirectorychange: (directory) => (this.lastImageDirectory = directory),
            },
        );
        return selection?.kind === 'server-file' || selection?.kind === 'axk-object-directory' ? selection : null;
    }

    private openCompanionRequest(retry: CompanionRetry | null): void {
        if (!this.location) return;
        this.companionRequest = {
            requestId: this.nextCompanionRequestId++,
            sources: [...this.companionSources],
            retry,
            sourceKind: this.location.kind === 'server-file' ? 'file' : 'directory',
            setLabel: this.floppySet?.setLabel || this.location.displayName,
            nextRequiredIndex: this.floppySet?.nextRequiredIndex ?? null,
            busy: false,
            error: '',
        };
    }

    private async applyOpenedImage(
        opened: OpenedImage,
        preferred?: { partitionIndex: number; volumeName?: string },
    ): Promise<void> {
        const { catalog, mutation, clearExportSelection } = this.requireCollaborators();
        clearExportSelection();
        this.integrityDialogOpen = false;
        this.integrityIssues = [];
        this.integrityError = '';
        this.companionSources = opened.companionSources;
        this.floppySet = opened.floppySet;
        mutation.setCapabilities(opened);
        this.objectDeletionAvailable = opened.objectDeletionAvailable;
        this.waveDataCleanupAvailable = opened.waveDataCleanupAvailable;
        this.programGenerationAvailable = opened.programGenerationAvailable;
        this.packageImportAvailable = opened.packageImportAvailable;
        this.packageExportAvailable = opened.packageExportAvailable;
        this.volumePackageExportAvailable = opened.volumePackageExportAvailable;
        this.volumeFloppyExportAvailable = opened.volumeFloppyExportAvailable;
        this.audioExportAvailable = opened.audioExportAvailable;
        this.sequenceExportAvailable = opened.sequenceExportAvailable;
        this.mediaConversionAvailable = opened.mediaConversionAvailable;
        this.extentLayoutRepairAvailable = opened.extentLayoutRepairAvailable;
        this.allocationInspectionAvailable = opened.allocationInspectionAvailable;
        this.imageFormat = opened.format ?? null;
        this.sourceItems = opened.tree;
        const preferredItem = preferred
            ? findSourceItem(opened.tree, preferred.partitionIndex, preferred.volumeName)
            : null;
        this.selectedSource = preferredItem ?? opened.initialVolume ?? opened.tree[0] ?? this.selectedSource;
        this.volumeSelection =
            this.selectedSource.kind === 'volume'
                ? { items: [this.selectedSource], anchorId: this.selectedSource.id }
                : emptyVolumeSelection();
        if (this.selectedSource.kind === 'volume')
            await catalog.loadVolume(this.selectedSource.id, this.selectedSource.partitionIndex ?? null);
        else catalog.clear();
        this.status = validationStatus(opened.validation);
        if (opened.validation.errorCount > 0) await this.showAllocationBlockers(opened);
        if (opened.floppySet?.status === 'INCOMPLETE') this.openCompanionRequest(null);
    }

    private async showAllocationBlockers(opened: OpenedImage): Promise<void> {
        await this.loadIntegrityIssues(opened.sessionId);
        if (this.sessionId !== opened.sessionId || this.integrityError) return;
        const blockerCodes = [...new Set(this.integrityIssues.map((issue) => issue.code))]
            .filter((code) => allocationBlockerCodes.has(code))
            .sort();
        if (blockerCodes.length === 0) return;
        const automaticKey = `${opened.sessionId}:${opened.revision}:${blockerCodes.join(',')}`;
        if (automaticKey === this.lastAutomaticIntegrityKey) return;
        this.lastAutomaticIntegrityKey = automaticKey;
        this.integrityDialogOpen = true;
    }

    private async loadIntegrityIssues(sessionId: number): Promise<void> {
        this.integrityLoading = true;
        this.integrityError = '';
        try {
            const issues = await this.transport.validationIssues(sessionId);
            if (this.sessionId === sessionId) this.integrityIssues = issues;
        } catch (error) {
            if (this.sessionId === sessionId) this.integrityError = userFacingMessage(error);
        } finally {
            if (this.sessionId === sessionId) this.integrityLoading = false;
        }
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
        await this.controller.close();
        collaborators.clearExportSelection();
        this.companionSources = [];
        this.floppySet = null;
        this.integrityDialogOpen = false;
        this.integrityIssues = [];
        this.integrityLoading = false;
        this.integrityError = '';
        this.lastAutomaticIntegrityKey = '';
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
        collaborators.deletion.dispose();
        collaborators.programGeneration.dispose();
        this.programGenerationAvailable = false;
    }
}

function sameImageSource(left: ImageLocation, right: ImageLocation): boolean {
    return (
        left.kind === right.kind &&
        left.reference.rootId === right.reference.rootId &&
        left.reference.relativePath === right.reference.relativePath
    );
}

function noImageSource(): DiskTreeItem {
    return { id: 'none', name: 'No image', kind: 'disk', childCount: 0 };
}

function collectVolumes(items: readonly DiskTreeItem[]): DiskTreeItem[] {
    const result: DiskTreeItem[] = [];
    for (const item of items) {
        if (item.kind === 'volume') result.push(item);
        result.push(...collectVolumes(item.children ?? []));
    }
    return result;
}

function findPartition(items: readonly DiskTreeItem[], partitionIndex: number): DiskTreeItem | null {
    for (const item of items) {
        if (item.kind === 'partition' && item.partitionIndex === partitionIndex) return item;
        const nested = findPartition(item.children ?? [], partitionIndex);
        if (nested) return nested;
    }
    return null;
}

function findSourceItem(items: DiskTreeItem[], partitionIndex: number, volumeName?: string): DiskTreeItem | null {
    for (const item of items) {
        if (
            item.partitionIndex === partitionIndex &&
            (volumeName === undefined ? item.kind === 'partition' : item.kind === 'volume' && item.name === volumeName)
        ) {
            return item;
        }
        const nested = findSourceItem(item.children ?? [], partitionIndex, volumeName);
        if (nested) return nested;
    }
    return null;
}
