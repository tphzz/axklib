import { audioExtensions, audioMediaType } from '../../lib/audioImport';
import { browserUploadSource, type ClientUploadSource } from '../../lib/clientUploadSource';
import type { DirectoryRef, FileLocation, ImageLocation } from '../../lib/storageLocations';
import type { AudioImportGrouping, AudioImportItem, AudioImportTarget, ImageTransport } from '../../lib/transport';
import type { DiskTreeItem, SampleStructureItem, WorkspaceView } from '../../lib/types';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { PickerController } from '../dialogs/picker';
import type { JobController } from '../jobs/actions';
import { findVolumeSourceItem, sameVolumeTarget } from './volumeTarget';

export interface AudioImportRequest {
    files: (ClientUploadSource | FileLocation)[];
    target: AudioImportTarget;
}

interface AudioImportDependencies {
    transport: ImageTransport;
    jobs: JobController;
    picker: PickerController;
    sessionId: () => number | null;
    imageLocation: () => ImageLocation | null;
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
        return this.dependencies.mutationsAvailable() && this.dependencies.imageLocation() !== null;
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
        const target = this.activeTarget();
        if (!target || !this.dependencies.imageLocation()) {
            this.dependencies.setStatus('Select a writable volume first');
            return;
        }
        this.request = { files: [], target };
    }

    filesChosen(event: Event): void {
        const input = event.currentTarget as HTMLInputElement;
        void this.requestDroppedFiles(
            Array.from(input.files ?? []).map(browserUploadSource),
            this.request?.target ?? this.activeTarget(),
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
        await this.requestDroppedFiles(selections, request.target);
    }

    chooseLocal(input: HTMLInputElement): void {
        if (!this.request || !this.dependencies.transport.supportsClientUploads) return;
        input.click();
    }

    async commit(items: AudioImportItem[], grouping: AudioImportGrouping): Promise<void> {
        const request = this.request;
        const sessionId = this.dependencies.sessionId();
        if (!request || sessionId === null) throw new Error('Audio import target is no longer available');
        const target = request.target;
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
            await this.dependencies.refreshSession(target);
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
        target = this.activeTarget(),
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
        if (!target || admitted.length === 0 || !this.dependencies.imageLocation()) {
            this.dependencies.setStatus(
                target ? 'Drop WAV, FLAC, or AIFF audio files' : 'Select a writable volume first',
            );
            return;
        }
        const active = this.activeTarget();
        if (!sameVolumeTarget(active, target)) {
            const item = findVolumeSourceItem(this.dependencies.sourceItems(), target);
            if (!item) {
                this.dependencies.setStatus('Audio import target is no longer available');
                return;
            }
            this.dependencies.setSelectedSource(item);
            await this.dependencies.loadVolume(item.id);
            if (this.dependencies.activeVolumeId() !== item.id) {
                this.dependencies.setStatus('Audio import target is no longer available');
                return;
            }
        }
        this.request = { files: admitted, target };
    }
}

function audioSourceName(source: ClientUploadSource | FileLocation): string {
    return 'kind' in source ? (source.reference.relativePath.split('/').at(-1) ?? source.displayName) : source.name;
}

export { audioExtensions };
