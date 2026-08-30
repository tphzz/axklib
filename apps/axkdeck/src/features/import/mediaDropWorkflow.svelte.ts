import { audioMediaType } from '../../lib/audioImport';
import { browserUploadSource, type ClientUploadSource } from '../../lib/clientUploadSource';
import { reportDiagnostic, reportError } from '../../lib/diagnostics';
import { midiMediaType } from '../../lib/midiImport';
import { listenForNativeMediaDrops } from '../../lib/nativeMediaDrop';
import { packageImportUploadKind } from '../../lib/packageImportMedia';
import type { AudioImportTarget } from '../../lib/transport';
import { tx16wDiskMediaType } from '../../lib/tx16wImport';
import type { DiskTreeItem, WorkspaceView } from '../../lib/types';
import type { AudioImportWorkflow } from './audioWorkflow.svelte';
import type { PackageBatchImportWorkflow } from './packageBatchWorkflow.svelte';
import type { PackageImportWorkflow } from './packageWorkflow.svelte';
import type { SequenceImportWorkflow } from './sequenceWorkflow.svelte';
import type { Tx16wImportWorkflow } from './tx16wWorkflow.svelte';

export type MediaDropKind = 'audio' | 'midi' | 'tx16w' | 'package' | 'mixed';
type ClassifiedMedia = MediaDropKind | 'none';

export interface MediaDropNotice {
    title: string;
    message: string;
}

interface MediaDropDependencies {
    isDesktop: boolean;
    workspaceView: () => WorkspaceView;
    audioImport: AudioImportWorkflow;
    sequenceImport: SequenceImportWorkflow;
    tx16wImport: Tx16wImportWorkflow;
    packageImport: PackageImportWorkflow;
    packageBatchImport: PackageBatchImportWorkflow;
    sessionId: () => number | null;
    imageFormat: () => string | null;
    mutationsAvailable: () => boolean;
    selectedSource: () => DiskTreeItem;
    setStatus: (status: string) => void;
}

export class MediaDropWorkflow {
    dragActive = $state(false);
    dragKind = $state<MediaDropKind | null>(null);
    dragTarget = $state<AudioImportTarget | null>(null);
    notice = $state<MediaDropNotice | null>(null);

    constructor(private readonly dependencies: MediaDropDependencies) {}

    closeNotice(): void {
        this.notice = null;
    }

    mountNativeDrops(): () => void {
        if (!this.dependencies.isDesktop) return () => undefined;
        let disposed = false;
        let unlisten: (() => void) | null = null;
        void listenForNativeMediaDrops({
            onHover: (paths) => this.updateHover(paths),
            onDrop: (files, _position, droppedPathCount) => {
                const kind = classifyDroppedNames(files.map((file) => file.name));
                reportDiagnostic('native_media_drop_received', {
                    droppedPathCount,
                    admittedFileCount: files.length,
                    mediaKind: kind,
                });
                if (droppedPathCount > 0 && files.length === 0) {
                    this.dependencies.setStatus(
                        'No supported package, A3K, audio, MIDI, or TX16W disk files were dropped',
                    );
                    return;
                }
                void this.route(files);
            },
            onError: (reason) => {
                this.clearHover();
                this.dependencies.setStatus('Dropped files could not be read');
                reportError('Read dropped media files failed', reason);
            },
        })
            .then((stop) => {
                if (disposed) stop();
                else unlisten = stop;
            })
            .catch((reason) => reportError('Initialize native media drop failed', reason));
        return () => {
            disposed = true;
            unlisten?.();
        };
    }

    drag(event: DragEvent): void {
        const dataTransfer = event.dataTransfer;
        if (!dataTransfer) return;
        event.preventDefault();
        if (!dragMayContainFiles(dataTransfer) || this.importDialogOpen()) {
            dataTransfer.dropEffect = 'none';
            this.clearHover();
            return;
        }
        const names = Array.from(dataTransfer.files).map((file) => file.name);
        const classified = classifyDroppedNames(names);
        const kind = classified === 'none' ? this.defaultKind() : classified;
        this.dragKind = kind;
        this.dragTarget = kind === 'mixed' ? null : this.selectedVolumeTarget();
        this.dragActive = true;
        dataTransfer.dropEffect = kind === 'mixed' || this.admission() ? 'none' : 'copy';
    }

    leave(event: DragEvent): void {
        if (event.relatedTarget === null) this.clearHover();
    }

    drop(event: DragEvent): void {
        const dataTransfer = event.dataTransfer;
        if (!dataTransfer) return;
        event.preventDefault();
        const files = Array.from(dataTransfer.files).map(browserUploadSource);
        const kind = classifyDroppedNames(files.map((file) => file.name));
        this.clearHover();
        if (files.length > 0) void this.route(files);
    }

    private async route(files: ClientUploadSource[]): Promise<void> {
        if (this.importDialogOpen() || files.length === 0) return;
        const admission = this.admission();
        if (admission) {
            this.dependencies.setStatus(admission.message);
            this.notice = admission;
            return;
        }
        const kind = classifyDroppedNames(files.map((file) => file.name));
        if (kind === 'mixed') {
            this.dependencies.setStatus('Drop one media type at a time');
            this.notice = {
                title: 'Import unavailable',
                message: 'Drop packages, A3K archives, audio, MIDI, and TX16W disks separately.',
            };
            return;
        }
        const selectedKind = kind === 'none' ? this.defaultKind() : kind;
        if (kind === 'none') {
            if (selectedKind === 'midi')
                await this.dependencies.sequenceImport.requestDroppedFiles(files, this.dependencies.selectedSource());
            else await this.dependencies.audioImport.requestDroppedFiles(files, this.dependencies.selectedSource());
            return;
        }
        if (selectedKind === 'package') {
            if (!this.dependencies.packageImport.dropAvailable()) {
                this.dependencies.setStatus('Package import requires a writable SFS hard-disk image');
                this.notice = {
                    title: 'Package import unavailable',
                    message: 'Open a writable SFS hard-disk image, then drop the package again.',
                };
                return;
            }
            if (files.length === 1) {
                await this.dependencies.packageImport.requestDroppedFile(files[0], this.dependencies.selectedSource());
            } else {
                await this.dependencies.packageBatchImport.requestDroppedFiles(
                    files,
                    this.dependencies.selectedSource(),
                );
            }
            return;
        }
        const volumeTarget = this.selectedVolumeTarget();
        if (selectedKind === 'tx16w') {
            if (!this.dependencies.tx16wImport.dropAvailable()) {
                this.dependencies.setStatus('TX16W import requires a writable SFS hard-disk image');
                this.notice = {
                    title: 'TX16W import unavailable',
                    message: 'Open a writable SFS hard-disk image, then drop the TX16W disk again.',
                };
                return;
            }
            await this.dependencies.tx16wImport.requestDroppedFiles(files, volumeTarget);
            return;
        }
        if (selectedKind === 'midi') {
            await this.dependencies.sequenceImport.requestDroppedFiles(files, this.dependencies.selectedSource());
            return;
        }
        await this.dependencies.audioImport.requestDroppedFiles(files, this.dependencies.selectedSource());
    }

    private updateHover(paths: readonly string[]): void {
        if (paths.length === 0 || this.importDialogOpen()) {
            this.clearHover();
            return;
        }
        const kind = classifyDroppedNames(paths);
        if (kind === 'none') {
            this.clearHover();
            return;
        }
        this.dragKind = kind;
        this.dragTarget = kind === 'mixed' ? null : this.selectedVolumeTarget();
        this.dragActive = true;
    }

    private defaultKind(): 'audio' | 'midi' {
        return this.dependencies.workspaceView() === 'sequences' ? 'midi' : 'audio';
    }

    private selectedVolumeTarget(): AudioImportTarget | null {
        const selected = this.dependencies.selectedSource();
        return selected.kind === 'volume' && selected.partitionIndex !== undefined
            ? { partitionIndex: selected.partitionIndex, volumeName: selected.name }
            : null;
    }

    private admission(): MediaDropNotice | null {
        return mediaDropAdmission(
            this.dependencies.sessionId(),
            this.dependencies.imageFormat(),
            this.dependencies.mutationsAvailable(),
        );
    }

    private importDialogOpen(): boolean {
        return (
            this.notice !== null ||
            this.dependencies.audioImport.request !== null ||
            this.dependencies.sequenceImport.request !== null ||
            this.dependencies.tx16wImport.request !== null ||
            this.dependencies.packageImport.request !== null ||
            this.dependencies.packageBatchImport.request !== null
        );
    }

    private clearHover(): void {
        this.dragActive = false;
        this.dragKind = null;
        this.dragTarget = null;
    }
}

export function mediaDropAdmission(
    sessionId: number | null,
    imageFormat: string | null,
    mutationsAvailable: boolean,
): MediaDropNotice | null {
    if (sessionId === null) {
        return {
            title: 'Open a hard-disk image',
            message: 'Open a writable SFS hard-disk image before importing dropped files.',
        };
    }
    if (imageFormat !== 'sfs') {
        return {
            title: 'Drag and drop unavailable',
            message: 'Drag and drop import can only be performed on SFS hard-disk images.',
        };
    }
    if (!mutationsAvailable) {
        return {
            title: 'Image is read-only',
            message: 'Drag and drop import requires a writable SFS hard-disk image.',
        };
    }
    return null;
}

export function classifyDroppedNames(names: readonly string[]): ClassifiedMedia {
    let audio = false;
    let midi = false;
    let tx16w = false;
    let packageMedia = false;
    for (const name of names) {
        audio ||= audioMediaType(name) !== null;
        midi ||= midiMediaType(name) !== null;
        tx16w ||= tx16wDiskMediaType(name) !== null;
        packageMedia ||= packageImportUploadKind(name) !== null;
    }
    if (Number(audio) + Number(midi) + Number(tx16w) + Number(packageMedia) > 1) return 'mixed';
    if (audio) return 'audio';
    if (midi) return 'midi';
    if (tx16w) return 'tx16w';
    if (packageMedia) return 'package';
    return 'none';
}

function dragMayContainFiles(dataTransfer: DataTransfer): boolean {
    if (dataTransfer.files.length > 0) return true;
    if (Array.from(dataTransfer.items ?? []).some((item) => item.kind === 'file')) return true;
    return Array.from(dataTransfer.types).some((type) =>
        ['Files', 'text/uri-list', 'application/x-moz-file'].includes(type),
    );
}
