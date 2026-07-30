import type { DirectoryLocation, DirectoryRef, FileLocation, ImageLocation } from '../../lib/storageLocations';
import type { CompanionDirectorySelection, ImageTransport, OpenedImage } from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { AuditionWorkflow } from '../audition/workflow.svelte';
import type { CatalogWorkflow } from '../catalog/workflow.svelte';
import type { DeletionWorkflow } from '../deletion/workflow.svelte';
import type { PickerController } from '../dialogs/picker';
import type { ExportCompanionRetry, ExportWorkflow } from '../export/workflow.svelte';
import { ImageSessionController } from './actions';
import type { PackageImportWorkflow } from '../import/packageWorkflow.svelte';
import type { MutationWorkflow } from '../mutation/workflow.svelte';

export type CompanionRetry =
    { kind: 'audition'; objectId: string } | { kind: 'sample-bank'; bankId: string } | ExportCompanionRetry;

interface SessionCollaborators {
    catalog: CatalogWorkflow;
    audition: AuditionWorkflow;
    mutation: MutationWorkflow;
    exports: ExportWorkflow;
    packageImport: PackageImportWorkflow;
    deletion: DeletionWorkflow;
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
    companionDirectories = $state<DirectoryRef[]>([]);
    opening = $state(false);
    status = $state('Ready');
    hardDiskDirectory = $state<DirectoryLocation | null>(null);
    companionRequest = $state<{
        directories: DirectoryRef[];
        retry: CompanionRetry;
        busy: boolean;
        error: string;
    } | null>(null);
    objectDeletionAvailable = $state(false);
    waveDataCleanupAvailable = $state(false);
    packageImportAvailable = $state(false);
    packageExportAvailable = $state(false);
    audioExportAvailable = $state(false);
    sequenceExportAvailable = $state(false);

    private readonly controller: ImageSessionController;
    private collaborators: SessionCollaborators | null = null;
    private lastImageDirectory = $state<DirectoryRef | null>(null);
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
            if (opened) await this.applyOpenedImage(opened, preferred);
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
        if (this.sessionId === null || this.location?.kind !== 'axk-object-directory') return;
        this.companionRequest = {
            directories: [...this.companionDirectories],
            retry,
            busy: false,
            error: '',
        };
    }

    async addCompanionDiskFolder(): Promise<void> {
        const request = this.companionRequest;
        if (!request || request.busy) return;
        const selection = await this.picker.chooseLocation('directory', 'Choose companion disk folder', [], '', {
            parentDialog: 'companion-disks',
            initialDirectory: this.lastCompanionDirectory,
            ondirectorychange: (directory) => (this.lastCompanionDirectory = directory),
            requireWritableDirectory: false,
        });
        if (selection?.kind !== 'server-directory' || this.companionRequest !== request) return;
        if (request.directories.some((directory) => sameDirectory(directory, selection.reference))) return;
        this.companionRequest = {
            ...request,
            directories: [...request.directories, selection.reference],
            error: '',
        };
    }

    removeCompanionDiskFolder(directory: DirectoryRef): void {
        if (!this.companionRequest || this.companionRequest.busy) return;
        this.companionRequest = {
            ...this.companionRequest,
            directories: this.companionRequest.directories.filter((candidate) => !sameDirectory(candidate, directory)),
            error: '',
        };
    }

    cancelCompanionDisks(): void {
        if (!this.companionRequest?.busy) this.companionRequest = null;
    }

    async attachCompanionDisks(selection: CompanionDirectorySelection): Promise<void> {
        const request = this.companionRequest;
        if (!request || request.busy || this.sessionId === null) return;
        const sessionId = this.sessionId;
        const preferred = this.currentSourcePreference();
        this.companionRequest = { ...request, busy: true, error: '' };
        try {
            const collaborators = this.requireCollaborators();
            await collaborators.audition.invalidateSession(sessionId);
            const opened = await this.transport.attachCompanionDirectories(sessionId, selection);
            if (this.sessionId !== sessionId || this.companionRequest?.retry !== request.retry) return;
            await this.applyOpenedImage(opened, preferred);
            this.companionRequest = null;
            await this.retryCompanionAction(request.retry);
        } catch (error) {
            if (this.companionRequest) {
                this.companionRequest = {
                    ...this.companionRequest,
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
            ['hds', 'hda', 'ima', 'img', 'iso', 'a3k'],
            '',
            {
                initialDirectory: this.lastImageDirectory,
                ondirectorychange: (directory) => (this.lastImageDirectory = directory),
            },
        );
        return selection?.kind === 'server-file' || selection?.kind === 'axk-object-directory' ? selection : null;
    }

    private async applyOpenedImage(
        opened: OpenedImage,
        preferred?: { partitionIndex: number; volumeName?: string },
    ): Promise<void> {
        const { catalog, mutation, clearExportSelection } = this.requireCollaborators();
        clearExportSelection();
        this.companionDirectories = opened.companionDirectories;
        mutation.setCapabilities(opened);
        this.objectDeletionAvailable = opened.objectDeletionAvailable;
        this.waveDataCleanupAvailable = opened.waveDataCleanupAvailable;
        this.packageImportAvailable = opened.packageImportAvailable;
        this.packageExportAvailable = opened.packageExportAvailable;
        this.audioExportAvailable = opened.audioExportAvailable;
        this.sequenceExportAvailable = opened.sequenceExportAvailable;
        this.sourceItems = opened.tree;
        const preferredItem = preferred
            ? findSourceItem(opened.tree, preferred.partitionIndex, preferred.volumeName)
            : null;
        this.selectedSource = preferredItem ?? opened.initialVolume ?? opened.tree[0] ?? this.selectedSource;
        if (this.selectedSource.kind === 'volume') await catalog.loadVolume(this.selectedSource.id);
        else catalog.clear();
        this.status = opened.validation.valid ? 'Ready' : `${opened.validation.errorCount} validation errors`;
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
        this.companionRequest = null;
        await collaborators.packageImport.dispose();
        await this.controller.close();
        collaborators.clearExportSelection();
        this.companionDirectories = [];
        collaborators.mutation.reset();
        this.objectDeletionAvailable = false;
        this.waveDataCleanupAvailable = false;
        this.packageImportAvailable = false;
        this.packageExportAvailable = false;
        this.audioExportAvailable = false;
        this.sequenceExportAvailable = false;
        collaborators.deletion.dispose();
    }
}

function sameDirectory(left: DirectoryRef, right: DirectoryRef): boolean {
    return left.rootId === right.rootId && left.relativePath === right.relativePath;
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
