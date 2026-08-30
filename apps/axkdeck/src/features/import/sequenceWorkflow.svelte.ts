import { browserUploadSource, type ClientUploadSource } from '../../lib/clientUploadSource';
import { midiExtensions, midiMediaType } from '../../lib/midiImport';
import type { DirectoryRef, FileLocation, ImageLocation } from '../../lib/storageLocations';
import type {
    ImageTransport,
    SequenceImportItem,
    SequenceSystemExclusivePolicy,
    VolumeImportDestination,
} from '../../lib/transport';
import type { DiskTreeItem, SequenceItem, WorkspaceView } from '../../lib/types';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { PickerController } from '../dialogs/picker';
import type { JobController } from '../jobs/actions';
import {
    collectImportDestinations,
    importDestination,
    initialImportDestination,
    type ImportDestinationMode,
} from './packageDestinations';
import { findVolumeSourceItem, sameVolumeTarget } from './volumeTarget';

export interface SequenceImportRequest {
    files: (ClientUploadSource | FileLocation)[];
    destinationMode: ImportDestinationMode;
    destinationPartitionIndex: number | null;
    destinationVolumeName: string;
}

interface SequenceImportDependencies {
    transport: ImageTransport;
    jobs: JobController;
    picker: PickerController;
    sessionId: () => number | null;
    imageLocation: () => ImageLocation | null;
    imageFormat: () => string | null;
    mutationsAvailable: () => boolean;
    selectedSource: () => DiskTreeItem;
    setSelectedSource: (item: DiskTreeItem) => void;
    sourceItems: () => DiskTreeItem[];
    activeVolumeId: () => string;
    sequences: () => SequenceItem[];
    loadVolume: (volumeId: string) => Promise<void>;
    refreshSession: (preferred: { partitionIndex: number; volumeName?: string }) => Promise<void>;
    invalidateSession: (sessionId: number) => Promise<void>;
    selectWorkspace: (view: WorkspaceView) => void;
    selectSequence: (sequence: SequenceItem) => void;
    setStatus: (status: string) => void;
    reportTiming: (operation: string, started: number, itemCount: number) => void;
}

export class SequenceImportWorkflow {
    request = $state<SequenceImportRequest | null>(null);
    destinationBusy = $state(false);
    private lastDirectory = $state<DirectoryRef | null>(null);
    private destinationRevision = 0;

    constructor(private readonly dependencies: SequenceImportDependencies) {}

    dropAvailable(): boolean {
        return (
            this.dependencies.mutationsAvailable() &&
            this.dependencies.imageLocation() !== null &&
            this.dependencies.imageFormat() === 'sfs'
        );
    }

    activeTarget(): VolumeImportDestination | null {
        const selected = this.dependencies.selectedSource();
        return this.dependencies.mutationsAvailable() &&
            selected.kind === 'volume' &&
            selected.partitionIndex !== undefined
            ? { kind: 'EXISTING_VOLUME', partitionIndex: selected.partitionIndex, volumeName: selected.name }
            : null;
    }

    chooseFiles(): void {
        if (!this.dropAvailable()) {
            this.dependencies.setStatus('Open a writable SFS hard-disk image first');
            return;
        }
        this.request = this.newRequest([], this.dependencies.selectedSource());
    }

    filesChosen(event: Event): void {
        const input = event.currentTarget as HTMLInputElement;
        const request = this.request;
        void this.requestDroppedFiles(
            Array.from(input.files ?? []).map(browserUploadSource),
            null,
            request ?? undefined,
        );
        input.value = '';
    }

    async chooseWorkspace(): Promise<void> {
        const request = this.request;
        if (!request) return;
        const selections = await this.dependencies.picker.chooseFiles('Choose MIDI files', [...midiExtensions], {
            parentDialog: 'sequence-import',
            initialDirectory: this.lastDirectory,
            ondirectorychange: (directory) => (this.lastDirectory = directory),
        });
        if (!selections || !this.request) return;
        await this.requestDroppedFiles(selections, null, request);
    }

    chooseLocal(input: HTMLInputElement): void {
        if (!this.request || !this.dependencies.transport.supportsClientUploads) return;
        input.click();
    }

    async commit(items: SequenceImportItem[], systemExclusivePolicy: SequenceSystemExclusivePolicy): Promise<void> {
        const request = this.request;
        const sessionId = this.dependencies.sessionId();
        if (!request || sessionId === null || this.destinationBusy) {
            throw new Error('MIDI import target is no longer available');
        }
        const target = this.destination();
        if (!target) throw new Error('Choose a valid MIDI import destination');
        const firstName = items[0]?.sequenceName;
        const started = performance.now();
        this.dependencies.setStatus('Importing MIDI');
        try {
            await this.dependencies.invalidateSession(sessionId);
            const completed = await this.dependencies.jobs.run(
                () => this.dependencies.transport.startSequenceImport(sessionId, target, items, systemExclusivePolicy),
                (update) => {
                    if (update.progress?.label) this.dependencies.setStatus(update.progress.label);
                },
            );
            if (completed.status !== 'completed') throw new Error(completed.error ?? 'MIDI import did not complete');
            this.dependencies.selectWorkspace('sequences');
            await this.dependencies.refreshSession({
                partitionIndex: target.partitionIndex,
                volumeName: target.volumeName,
            });
            const inserted = this.dependencies.sequences().find((sequence) => sequence.name === firstName);
            if (inserted) this.dependencies.selectSequence(inserted);
            this.dependencies.reportTiming('midi-import', started, items.length);
        } catch (error) {
            this.dependencies.setStatus(userFacingMessage(error));
            throw error;
        }
    }

    async requestDroppedFiles(
        files: (ClientUploadSource | FileLocation)[],
        selected: DiskTreeItem | null = this.dependencies.selectedSource(),
        destinationRequest?: SequenceImportRequest,
    ): Promise<void> {
        const admitted = files
            .map((source) => {
                const mediaType = midiMediaType(sourceName(source));
                if (!mediaType) return null;
                return 'kind' in source ? source : { ...source, type: mediaType };
            })
            .filter((source) => source !== null);
        if (files.length > 0 && admitted.length === 0) {
            this.dependencies.setStatus('No Standard MIDI Files were selected');
            return;
        }
        if (admitted.length === 0 || !this.dropAvailable()) {
            this.dependencies.setStatus('Drop Standard MIDI Files onto a writable SFS hard-disk image');
            return;
        }
        this.request = destinationRequest
            ? { ...destinationRequest, files: admitted }
            : this.newRequest(admitted, selected);
        const target = this.destination();
        if (target?.kind === 'EXISTING_VOLUME') await this.synchronizeExistingVolume(target);
    }

    destination(): VolumeImportDestination | null {
        const request = this.request;
        if (!request) return null;
        return importDestination(
            request.destinationMode,
            request.destinationPartitionIndex,
            request.destinationVolumeName,
        );
    }

    partitionOptions() {
        return collectImportDestinations(this.dependencies.sourceItems()).partitions;
    }

    volumeOptions() {
        return collectImportDestinations(this.dependencies.sourceItems()).volumes;
    }

    existingSequenceNames(): string[] {
        if (this.destinationBusy) return [];
        const target = this.destination();
        if (target?.kind !== 'EXISTING_VOLUME') return [];
        const item = findVolumeSourceItem(this.dependencies.sourceItems(), target);
        if (!item || item.id !== this.dependencies.activeVolumeId()) return [];
        return this.dependencies.sequences().map((sequence) => sequence.name);
    }

    setDestinationMode(mode: ImportDestinationMode): void {
        const request = this.request;
        if (!request) return;
        this.cancelDestinationSynchronization();
        const destinations = collectImportDestinations(this.dependencies.sourceItems());
        request.destinationMode = mode;
        request.destinationVolumeName = '';
        request.destinationPartitionIndex =
            request.destinationPartitionIndex ?? destinations.partitions[0]?.partitionIndex ?? null;
    }

    async setExistingVolume(partitionIndex: number | null, volumeName: string): Promise<void> {
        const request = this.request;
        if (!request) return;
        request.destinationPartitionIndex = partitionIndex;
        request.destinationVolumeName = volumeName;
        if (partitionIndex === null || !volumeName) {
            this.cancelDestinationSynchronization();
            return;
        }
        await this.synchronizeExistingVolume({
            kind: 'EXISTING_VOLUME',
            partitionIndex,
            volumeName,
        });
    }

    setDestinationPartition(partitionIndex: number): void {
        if (!this.request) return;
        this.cancelDestinationSynchronization();
        this.request.destinationPartitionIndex = partitionIndex;
        if (this.request.destinationMode === 'existing') this.request.destinationVolumeName = '';
    }

    setDestinationVolumeName(volumeName: string): void {
        if (this.request) this.request.destinationVolumeName = volumeName.slice(0, 16);
    }

    private newRequest(
        files: (ClientUploadSource | FileLocation)[],
        selected: DiskTreeItem | null,
    ): SequenceImportRequest {
        const initial = selected ? initialImportDestination(selected) : null;
        const firstPartition = collectImportDestinations(this.dependencies.sourceItems()).partitions[0];
        return {
            files,
            destinationMode: initial?.mode ?? 'existing',
            destinationPartitionIndex: initial?.partitionIndex ?? firstPartition?.partitionIndex ?? null,
            destinationVolumeName: initial?.volumeName ?? '',
        };
    }

    private async synchronizeExistingVolume(target: VolumeImportDestination): Promise<void> {
        if (target.kind !== 'EXISTING_VOLUME') return;
        const revision = ++this.destinationRevision;
        this.destinationBusy = true;
        try {
            await this.loadExistingVolume(target);
        } catch (error) {
            if (revision === this.destinationRevision && this.request) {
                this.request.destinationVolumeName = '';
                this.dependencies.setStatus(userFacingMessage(error));
            }
        } finally {
            if (revision === this.destinationRevision) this.destinationBusy = false;
        }
    }

    private cancelDestinationSynchronization(): void {
        this.destinationRevision += 1;
        this.destinationBusy = false;
    }

    private async loadExistingVolume(target: VolumeImportDestination): Promise<void> {
        if (target.kind !== 'EXISTING_VOLUME') return;
        if (sameVolumeTarget(this.activeTarget(), target)) return;
        const item = findVolumeSourceItem(this.dependencies.sourceItems(), target);
        if (!item) throw new Error('MIDI import target is no longer available');
        if (this.dependencies.activeVolumeId() === item.id) return;
        this.dependencies.setSelectedSource(item);
        await this.dependencies.loadVolume(item.id);
        if (this.dependencies.activeVolumeId() !== item.id) {
            throw new Error('MIDI import target is no longer available');
        }
    }
}

function sourceName(source: ClientUploadSource | FileLocation): string {
    return 'kind' in source ? (source.reference.relativePath.split('/').at(-1) ?? source.displayName) : source.name;
}
