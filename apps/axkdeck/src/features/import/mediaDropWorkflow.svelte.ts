import { audioMediaType } from '../../lib/audioImport';
import { browserUploadSource, type ClientUploadSource } from '../../lib/clientUploadSource';
import { reportDiagnostic, reportError } from '../../lib/diagnostics';
import { midiMediaType } from '../../lib/midiImport';
import { listenForNativeMediaDrops, type NativeDropPosition } from '../../lib/nativeMediaDrop';
import { packageImportUploadKind } from '../../lib/packageImportMedia';
import type { AudioImportTarget } from '../../lib/transport';
import { tx16wDiskMediaType } from '../../lib/tx16wImport';
import type { DiskTreeItem, WorkspaceView } from '../../lib/types';
import type { AudioImportWorkflow } from './audioWorkflow.svelte';
import type { PackageImportWorkflow } from './packageWorkflow.svelte';
import type { SequenceImportWorkflow } from './sequenceWorkflow.svelte';
import type { Tx16wImportWorkflow } from './tx16wWorkflow.svelte';

export type MediaDropKind = 'audio' | 'midi' | 'tx16w' | 'package' | 'mixed';
type ClassifiedMedia = MediaDropKind | 'none';
type DropTarget = AudioImportTarget | DiskTreeItem;

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
            onHover: (paths, position) => this.updateHover(paths, position),
            onDrop: (files, position, droppedPathCount) => {
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
                void this.route(files, position ? this.nativeDroppedTarget(position, kind) : null);
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
        if (kind === 'midi' && this.dependencies.workspaceView() !== 'sequences') {
            dataTransfer.dropEffect = 'none';
            this.clearHover();
            return;
        }
        this.dragKind = kind;
        const target = kind === 'mixed' ? null : this.targetForElement(event.target, kind);
        this.dragTarget = target && !('kind' in target) ? target : null;
        this.dragActive = true;
        const dropAvailable = kind === 'package' ? this.dependencies.packageImport.dropAvailable() : target !== null;
        dataTransfer.dropEffect = kind === 'mixed' || !dropAvailable ? 'none' : 'copy';
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
        const target = this.targetForElement(event.target, kind === 'none' ? this.defaultKind() : kind);
        this.clearHover();
        if (files.length > 0) void this.route(files, target);
    }

    private async route(files: ClientUploadSource[], target: DropTarget | null): Promise<void> {
        if (this.importDialogOpen() || files.length === 0) return;
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
            const volumeTarget = target && !('kind' in target) ? target : null;
            if (selectedKind === 'midi')
                await this.dependencies.sequenceImport.requestDroppedFiles(files, volumeTarget);
            else await this.dependencies.audioImport.requestDroppedFiles(files, volumeTarget);
            return;
        }
        if (selectedKind === 'package') {
            if (files.length !== 1) {
                this.dependencies.setStatus('Drop one package or A3K archive at a time');
                this.notice = {
                    title: 'Import unavailable',
                    message: 'Choose one portable package or A3K archive for each import.',
                };
                return;
            }
            if (!this.dependencies.packageImport.dropAvailable()) {
                this.dependencies.setStatus('Package import requires a writable SFS hard-disk image');
                this.notice = {
                    title: 'Package import unavailable',
                    message: 'Open a writable SFS hard-disk image, then drop the package again.',
                };
                return;
            }
            await this.dependencies.packageImport.requestDroppedFile(
                files[0],
                target && 'kind' in target ? target : null,
            );
            return;
        }
        const volumeTarget = target && !('kind' in target) ? target : null;
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
            if (this.dependencies.workspaceView() !== 'sequences') {
                this.dependencies.setStatus('Open the Sequences tab to import MIDI files');
                this.notice = {
                    title: 'MIDI import unavailable',
                    message: 'Open the Sequences tab, select a writable volume, then drop the MIDI files again.',
                };
                return;
            }
            if (!volumeTarget) {
                this.dependencies.setStatus('Select a writable volume first');
                this.notice = {
                    title: 'MIDI import unavailable',
                    message: 'Select a writable volume in Contents, then drop the MIDI files again.',
                };
                return;
            }
            await this.dependencies.sequenceImport.requestDroppedFiles(files, volumeTarget);
            return;
        }
        if (!volumeTarget) {
            this.dependencies.setStatus('Select a writable volume first');
            this.notice = {
                title: 'Audio import unavailable',
                message: 'Select a writable volume in Contents, then drop the audio files again.',
            };
            return;
        }
        await this.dependencies.audioImport.requestDroppedFiles(files, volumeTarget);
    }

    private updateHover(paths: readonly string[], position?: NativeDropPosition): void {
        if (paths.length === 0 || this.importDialogOpen()) {
            this.clearHover();
            return;
        }
        const kind = classifyDroppedNames(paths);
        if (kind === 'none' || (kind === 'midi' && this.dependencies.workspaceView() !== 'sequences')) {
            this.clearHover();
            return;
        }
        this.dragKind = kind;
        const target = kind === 'mixed' || !position ? null : this.nativeDroppedTarget(position, kind);
        this.dragTarget = target && !('kind' in target) ? target : null;
        this.dragActive = true;
    }

    private defaultKind(): 'audio' | 'midi' {
        return this.dependencies.workspaceView() === 'sequences' ? 'midi' : 'audio';
    }

    private targetForElement(target: EventTarget | null, kind: ClassifiedMedia): DropTarget | null {
        if (kind === 'mixed' || kind === 'none') return null;
        if (kind === 'package') {
            if (!this.dependencies.packageImport.dropAvailable()) return null;
            const element = target instanceof Element ? target.closest<HTMLElement>('[data-import-drop-kind]') : null;
            const partition = element?.dataset.importDropPartition;
            const itemKind = element?.dataset.importDropKind;
            const name = element?.dataset.importDropName;
            if ((itemKind === 'partition' || itemKind === 'volume') && partition !== undefined && name) {
                return {
                    id: `drop-${itemKind}-${partition}-${name}`,
                    name,
                    kind: itemKind,
                    partitionIndex: Number(partition),
                    childCount: 0,
                };
            }
            return null;
        }
        const workflow =
            kind === 'midi'
                ? this.dependencies.sequenceImport
                : kind === 'tx16w'
                  ? this.dependencies.tx16wImport
                  : this.dependencies.audioImport;
        if (!workflow.dropAvailable()) return null;
        const element = target instanceof Element ? target.closest<HTMLElement>('[data-import-drop-volume]') : null;
        const partition = element?.dataset.importDropPartition;
        if (element?.dataset.importDropVolume && partition !== undefined) {
            return { partitionIndex: Number(partition), volumeName: element.dataset.importDropVolume };
        }
        return workflow.activeTarget();
    }

    private nativeDroppedTarget(position: NativeDropPosition, kind: ClassifiedMedia): DropTarget | null {
        const scale = window.devicePixelRatio > 0 ? window.devicePixelRatio : 1;
        return this.targetForElement(document.elementFromPoint?.(position.x / scale, position.y / scale) ?? null, kind);
    }

    private importDialogOpen(): boolean {
        return (
            this.notice !== null ||
            this.dependencies.audioImport.request !== null ||
            this.dependencies.sequenceImport.request !== null ||
            this.dependencies.tx16wImport.request !== null ||
            this.dependencies.packageImport.request !== null
        );
    }

    private clearHover(): void {
        this.dragActive = false;
        this.dragKind = null;
        this.dragTarget = null;
    }
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
