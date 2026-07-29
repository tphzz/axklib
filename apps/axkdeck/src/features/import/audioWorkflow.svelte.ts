import { audioExtensions, audioMediaType } from '../../lib/audioImport';
import { browserUploadSource, type ClientUploadSource } from '../../lib/clientUploadSource';
import { reportDiagnostic, reportError } from '../../lib/diagnostics';
import { listenForNativeAudioDrops, type NativeDropPosition } from '../../lib/nativeAudioDrop';
import type { DirectoryRef, FileLocation, ImageLocation } from '../../lib/storageLocations';
import type { AudioImportItem, AudioImportTarget, ImageTransport } from '../../lib/transport';
import type { DiskTreeItem, SampleStructureItem, WorkspaceView } from '../../lib/types';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { PickerController } from '../dialogs/picker';
import type { JobController } from '../jobs/actions';

export interface AudioImportRequest {
    files: (ClientUploadSource | FileLocation)[];
    target: AudioImportTarget;
}

interface AudioImportDependencies {
    transport: ImageTransport;
    jobs: JobController;
    picker: PickerController;
    isDesktop: boolean;
    sessionId: () => number | null;
    imageLocation: () => ImageLocation | null;
    mutationsAvailable: () => boolean;
    selectedSource: () => DiskTreeItem;
    setSelectedSource: (item: DiskTreeItem) => void;
    sourceItems: () => DiskTreeItem[];
    activeVolumeId: () => string;
    samples: () => SampleStructureItem[];
    loadVolume: (volumeId: string) => Promise<void>;
    refreshSession: (preferred: { partitionIndex: number; volumeName?: string }) => Promise<void>;
    invalidateSession: (sessionId: number) => Promise<void>;
    selectWorkspace: (view: WorkspaceView) => void;
    selectSample: (sample: SampleStructureItem) => void;
    setStatus: (status: string) => void;
    reportTiming: (operation: string, started: number, itemCount: number) => void;
}

export class AudioImportWorkflow {
    request = $state<AudioImportRequest | null>(null);
    dragActive = $state(false);
    dragTarget = $state<AudioImportTarget | null>(null);
    private lastDirectory = $state<DirectoryRef | null>(null);

    constructor(private readonly dependencies: AudioImportDependencies) {}

    mountNativeDrops(): () => void {
        if (!this.dependencies.isDesktop) return () => undefined;
        let disposed = false;
        let unlisten: (() => void) | null = null;
        void listenForNativeAudioDrops({
            onHover: (active, position) => {
                this.dragActive = active;
                this.dragTarget = active && position ? this.nativeDroppedTarget(position) : null;
            },
            onDrop: (files, position, droppedPathCount) => {
                reportDiagnostic('native_audio_drop_received', {
                    droppedPathCount,
                    admittedFileCount: files.length,
                });
                if (droppedPathCount > 0 && files.length === 0) {
                    this.dependencies.setStatus('No supported audio files were dropped');
                    return;
                }
                void this.requestFiles(files, this.nativeDroppedTarget(position));
            },
            onError: (reason) => {
                this.dependencies.setStatus('Dropped audio files could not be read');
                reportError('Read dropped audio files failed', reason);
            },
        })
            .then((stop) => {
                if (disposed) stop();
                else unlisten = stop;
            })
            .catch((reason) => reportError('Initialize native audio drop failed', reason));
        return () => {
            disposed = true;
            unlisten?.();
        };
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
        void this.requestFiles(
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
        await this.requestFiles(selections, request.target);
    }

    chooseLocal(input: HTMLInputElement): void {
        if (!this.request || !this.dependencies.transport.supportsClientUploads) return;
        input.click();
    }

    drag(event: DragEvent): void {
        const dataTransfer = event.dataTransfer;
        if (!dataTransfer) return;
        event.preventDefault();
        if (!dragMayContainFiles(dataTransfer)) {
            dataTransfer.dropEffect = 'none';
            return;
        }
        this.dragTarget = this.droppedTarget(event);
        dataTransfer.dropEffect = this.dragTarget ? 'copy' : 'none';
        this.dragActive = true;
    }

    leave(event: DragEvent): void {
        if (event.relatedTarget !== null) return;
        this.dragActive = false;
        this.dragTarget = null;
    }

    drop(event: DragEvent): void {
        const dataTransfer = event.dataTransfer;
        if (!dataTransfer) return;
        event.preventDefault();
        this.dragActive = false;
        const target = this.droppedTarget(event);
        this.dragTarget = null;
        const files = Array.from(dataTransfer.files).map(browserUploadSource);
        if (files.length > 0) void this.requestFiles(files, target);
    }

    async commit(items: AudioImportItem[]): Promise<void> {
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
                () => this.dependencies.transport.startAudioImport(sessionId, target, items),
                (update) => {
                    if (update.progress?.label) this.dependencies.setStatus(update.progress.label);
                },
            );
            if (completed.status !== 'completed') throw new Error(completed.error ?? 'Audio import did not complete');
            this.dependencies.selectWorkspace('samples');
            await this.dependencies.refreshSession(target);
            const inserted = this.dependencies.samples().find((sample) => sample.name === firstName);
            if (inserted) this.dependencies.selectSample(inserted);
            this.dependencies.reportTiming('audio-import', started, items.length);
        } catch (error) {
            this.dependencies.setStatus(userFacingMessage(error));
            throw error;
        }
    }

    private async requestFiles(
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
        if (active?.partitionIndex !== target.partitionIndex || active.volumeName !== target.volumeName) {
            const item = findSourceItem(this.dependencies.sourceItems(), target.partitionIndex, target.volumeName);
            if (!item || item.kind !== 'volume') {
                this.dependencies.setStatus('Audio import target is no longer available');
                return;
            }
            this.dependencies.setSelectedSource(item);
            await this.dependencies.loadVolume(item.id);
            if (this.dependencies.activeVolumeId() !== item.id) return;
        }
        this.request = { files: admitted, target };
    }

    private targetForElement(target: EventTarget | null): AudioImportTarget | null {
        const element = target instanceof Element ? target.closest<HTMLElement>('[data-audio-drop-volume]') : null;
        const partition = element?.dataset.audioDropPartition;
        if (element?.dataset.audioDropVolume && partition !== undefined) {
            return { partitionIndex: Number(partition), volumeName: element.dataset.audioDropVolume };
        }
        return this.activeTarget();
    }

    private droppedTarget(event: DragEvent): AudioImportTarget | null {
        return this.targetForElement(event.target);
    }

    private nativeDroppedTarget(position: NativeDropPosition): AudioImportTarget | null {
        const scale = window.devicePixelRatio > 0 ? window.devicePixelRatio : 1;
        return this.targetForElement(document.elementFromPoint?.(position.x / scale, position.y / scale) ?? null);
    }
}

function audioSourceName(source: ClientUploadSource | FileLocation): string {
    return 'kind' in source ? (source.reference.relativePath.split('/').at(-1) ?? source.displayName) : source.name;
}

function dragMayContainFiles(dataTransfer: DataTransfer): boolean {
    if (dataTransfer.files.length > 0) return true;
    if (Array.from(dataTransfer.items ?? []).some((item) => item.kind === 'file')) return true;
    return Array.from(dataTransfer.types).some((type) =>
        ['Files', 'text/uri-list', 'application/x-moz-file'].includes(type),
    );
}

function findSourceItem(items: DiskTreeItem[], partitionIndex: number, volumeName?: string): DiskTreeItem | null {
    for (const item of items) {
        if (item.partitionIndex === partitionIndex && (!volumeName || item.name === volumeName)) return item;
        const nested = findSourceItem(item.children ?? [], partitionIndex, volumeName);
        if (nested) return nested;
    }
    return null;
}

export { audioExtensions };
