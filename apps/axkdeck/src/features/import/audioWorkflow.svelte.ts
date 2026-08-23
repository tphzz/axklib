import { audioExtensions, audioMediaType } from '../../lib/audioImport';
import { browserUploadSource, type ClientUploadSource } from '../../lib/clientUploadSource';
import type { DirectoryRef, FileLocation, ImageLocation } from '../../lib/storageLocations';
import type {
    AudioImportDestination,
    AudioImportGrouping,
    AudioImportItem,
    AudioImportTarget,
    ImageTransport,
} from '../../lib/transport';
import type { DiskTreeItem, SampleStructureItem, WorkspaceView } from '../../lib/types';
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

export interface AudioImportRequest {
    files: (ClientUploadSource | FileLocation)[];
    destinationMode: ImportDestinationMode;
    destinationPartitionIndex: number | null;
    destinationVolumeName: string;
}

interface AudioImportDependencies {
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
    sampleBanks: () => SampleStructureItem[];
    samples: () => SampleStructureItem[];
    loadVolume: (volumeId: string) => Promise<void>;
    refreshSession: (preferred: { partitionIndex: number; volumeName?: string }) => Promise<void>;
    invalidateSession: (sessionId: number) => Promise<void>;
    selectWorkspace: (view: WorkspaceView) => void;
    selectSampleBank: (sampleBank: SampleStructureItem) => void;
    selectSample: (sample: SampleStructureItem) => void;
    setStatus: (status: string) => void;
    reportTiming: (operation: string, started: number, itemCount: number) => void;
}

export class AudioImportWorkflow {
    request = $state<AudioImportRequest | null>(null);
    private lastDirectory = $state<DirectoryRef | null>(null);

    constructor(private readonly dependencies: AudioImportDependencies) {}

    dropAvailable(): boolean {
        return (
            this.dependencies.mutationsAvailable() &&
            this.dependencies.imageLocation() !== null &&
            this.dependencies.imageFormat() === 'sfs'
        );
    }

    activeTarget(): AudioImportTarget | null {
        const selected = this.dependencies.selectedSource();
        return this.dependencies.mutationsAvailable() &&
            selected.kind === 'volume' &&
            selected.partitionIndex !== undefined
            ? { partitionIndex: selected.partitionIndex, volumeName: selected.name }
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
        const selections = await this.dependencies.picker.chooseFiles('Choose audio files', [...audioExtensions], {
            parentDialog: 'audio-import',
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

    async commit(items: AudioImportItem[], grouping: AudioImportGrouping): Promise<void> {
        const request = this.request;
        const sessionId = this.dependencies.sessionId();
        if (!request || sessionId === null) throw new Error('Audio import target is no longer available');
        const target = this.destination();
        if (!target) throw new Error('Choose a valid audio import destination');
        const firstName = items[0]?.sampleName;
        const started = performance.now();
        this.dependencies.setStatus('Importing audio');
        try {
            await this.dependencies.invalidateSession(sessionId);
            const completed = await this.dependencies.jobs.run(
                () => this.dependencies.transport.startAudioImport(sessionId, target, items, grouping),
                (update) => {
                    if (update.progress?.label) this.dependencies.setStatus(update.progress.label);
                },
            );
            if (completed.status !== 'completed') throw new Error(completed.error ?? 'Audio import did not complete');
            this.dependencies.selectWorkspace(grouping.kind === 'SAMPLE_BANK' ? 'sample-banks' : 'samples');
            await this.dependencies.refreshSession({
                partitionIndex: target.partitionIndex,
                volumeName: target.volumeName,
            });
            if (grouping.kind === 'SAMPLE_BANK') {
                const inserted = this.dependencies
                    .sampleBanks()
                    .find((sampleBank) => sampleBank.name === grouping.sampleBankName);
                if (inserted) this.dependencies.selectSampleBank(inserted);
            } else {
                const inserted = this.dependencies.samples().find((sample) => sample.name === firstName);
                if (inserted) this.dependencies.selectSample(inserted);
            }
            this.dependencies.reportTiming('audio-import', started, items.length);
        } catch (error) {
            this.dependencies.setStatus(userFacingMessage(error));
            throw error;
        }
    }

    async requestDroppedFiles(
        files: (ClientUploadSource | FileLocation)[],
        selected: DiskTreeItem | null = this.dependencies.selectedSource(),
        destinationRequest?: AudioImportRequest,
    ): Promise<void> {
        const admitted: (ClientUploadSource | FileLocation)[] = [];
        for (const source of files) {
            const mediaType = audioMediaType(audioSourceName(source));
            if (mediaType) admitted.push('kind' in source ? source : { ...source, type: mediaType });
        }
        if (files.length > 0 && admitted.length === 0) {
            this.dependencies.setStatus('No supported WAV, FLAC, or AIFF files were selected');
            return;
        }
        if (admitted.length === 0 || !this.dropAvailable()) {
            this.dependencies.setStatus('Drop WAV, FLAC, or AIFF audio files onto a writable SFS hard-disk image');
            return;
        }
        this.request = destinationRequest
            ? { ...destinationRequest, files: admitted }
            : this.newRequest(admitted, selected);
        const target = this.destination();
        if (target?.kind === 'EXISTING_VOLUME') await this.loadExistingVolume(target);
    }

    destination(): AudioImportDestination | null {
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

    setDestinationMode(mode: ImportDestinationMode): void {
        const request = this.request;
        if (!request) return;
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
        if (partitionIndex !== null && volumeName) {
            try {
                await this.loadExistingVolume({ partitionIndex, volumeName });
            } catch (error) {
                request.destinationPartitionIndex = null;
                request.destinationVolumeName = '';
                this.dependencies.setStatus(userFacingMessage(error));
            }
        }
    }

    setDestinationPartition(partitionIndex: number): void {
        if (!this.request) return;
        this.request.destinationPartitionIndex = partitionIndex;
        if (this.request.destinationMode === 'existing') this.request.destinationVolumeName = '';
    }

    setDestinationVolumeName(volumeName: string): void {
        if (this.request) this.request.destinationVolumeName = volumeName.slice(0, 16);
    }

    private newRequest(
        files: (ClientUploadSource | FileLocation)[],
        selected: DiskTreeItem | null,
    ): AudioImportRequest {
        const initial = selected ? initialImportDestination(selected) : null;
        const firstPartition = collectImportDestinations(this.dependencies.sourceItems()).partitions[0];
        return {
            files,
            destinationMode: initial?.mode ?? 'existing',
            destinationPartitionIndex: initial?.partitionIndex ?? firstPartition?.partitionIndex ?? null,
            destinationVolumeName: initial?.volumeName ?? '',
        };
    }

    private async loadExistingVolume(target: AudioImportTarget): Promise<void> {
        const active = this.activeTarget();
        if (sameVolumeTarget(active, target)) return;
        const item = findVolumeSourceItem(this.dependencies.sourceItems(), target);
        if (!item) throw new Error('Audio import target is no longer available');
        this.dependencies.setSelectedSource(item);
        await this.dependencies.loadVolume(item.id);
        if (this.dependencies.activeVolumeId() !== item.id) {
            throw new Error('Audio import target is no longer available');
        }
    }
}

function audioSourceName(source: ClientUploadSource | FileLocation): string {
    return 'kind' in source ? (source.reference.relativePath.split('/').at(-1) ?? source.displayName) : source.name;
}

export { audioExtensions };
