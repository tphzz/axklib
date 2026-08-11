import { axkObjectDirectoryLocation } from '../../lib/storageLocations';
import type { DirectoryLocation, DirectoryRef, FileLocation, FileRef, ImageLocation } from '../../lib/storageLocations';
import type { CompanionSelection, FloppySetSummary, ImageTransport, OpenedImage } from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
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
import { validationStatus } from './validationStatus';

export type CompanionRetry =
    { kind: 'audition'; objectId: string } | { kind: 'sample-bank'; bankId: string } | ExportCompanionRetry;

type CompanionSourceKind = 'file' | 'directory';

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
    clearExportSelection: () => void;
}

export class ImageSessionWorkflow {
    sourceItems = $state<DiskTreeItem[]>([]);
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

    private readonly controller: ImageSessionController;
    private collaborators: SessionCollaborators | null = null;
    private nextCompanionRequestId = 1;
    private lastImageDirectory = $state<DirectoryRef | null>(null);
    private lastOpenedImageFile = $state<FileRef | null>(null);
    private lastCompanionDirectory = $state<DirectoryRef | null>(null);

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
    }

    setStatus(status: string): void {
        this.status = status;
    }

    selectSource(item: DiskTreeItem): void {
        const { catalog } = this.requireCollaborators();
        this.selectedSource = item;
        if (item.kind !== 'volume') {
            catalog.clear();
            return;
        }
        if (item.id !== catalog.activeVolumeId) void catalog.loadVolume(item.id);
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
            this.selectedSource = { id: 'none', name: 'No image', kind: 'disk', childCount: 0 };
            this.requireCollaborators().catalog.clear();
            this.status = 'Ready';
        } catch (error) {
            this.status = userFacingMessage(error);
            throw error;
        }
    }

    async dispose(): Promise<void> {
        await this.controller.dispose();
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
        this.sourceItems = opened.tree;
        const preferredItem = preferred
            ? findSourceItem(opened.tree, preferred.partitionIndex, preferred.volumeName)
            : null;
        this.selectedSource = preferredItem ?? opened.initialVolume ?? opened.tree[0] ?? this.selectedSource;
        if (this.selectedSource.kind === 'volume') await catalog.loadVolume(this.selectedSource.id);
        else catalog.clear();
        this.status = validationStatus(opened.validation);
        if (opened.floppySet?.status === 'INCOMPLETE') this.openCompanionRequest(null);
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
        this.companionRequest = null;
        await collaborators.packageImport.dispose();
        await this.controller.close();
        collaborators.clearExportSelection();
        this.companionSources = [];
        this.floppySet = null;
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
