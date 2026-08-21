import type { MediaExportWorkflow } from '../export/mediaWorkflow.svelte';
import type { VolumeFloppyExportWorkflow } from '../export/volumeFloppyWorkflow.svelte';
import type { VolumePackageExportWorkflow } from '../export/volumePackageWorkflow.svelte';
import type { ExportWorkflow } from '../export/workflow.svelte';
import type { AudioImportWorkflow } from '../import/audioWorkflow.svelte';
import type { PackageBatchImportWorkflow } from '../import/packageBatchWorkflow.svelte';
import type { PackageImportWorkflow } from '../import/packageWorkflow.svelte';
import type { SequenceImportWorkflow } from '../import/sequenceWorkflow.svelte';
import { shouldUseDirectComputerFileOperations } from '../../lib/fileOperationRouting';
import type { ConnectionMode } from '../../lib/transport';
import type { DiskTreeItem, PackageExportObject, PackageExportSelection } from '../../lib/types';
import { writable } from 'svelte/store';

export type DirectComputerOperation =
    | 'package-import'
    | 'package-batch-import'
    | 'package-export'
    | 'audio-export'
    | 'sequence-export'
    | 'volume-package-export'
    | 'volume-floppy-export'
    | 'media-export'
    | 'audio-import'
    | 'sequence-import';

export function directComputerDialogVisible(
    pendingOperation: DirectComputerOperation | null,
    operation: DirectComputerOperation,
    contentAvailable: boolean,
): boolean {
    return pendingOperation !== operation || contentAvailable;
}

export class DirectComputerWorkflow {
    readonly enabled: boolean;
    readonly pendingOperation = writable<DirectComputerOperation | null>(null);
    private operationGeneration = 0;

    constructor(isDesktop: boolean, connectionMode: ConnectionMode) {
        this.enabled = shouldUseDirectComputerFileOperations(isDesktop, connectionMode);
    }

    private begin(operation: DirectComputerOperation): number {
        const generation = ++this.operationGeneration;
        this.pendingOperation.set(operation);
        return generation;
    }

    private finish(generation?: number): void {
        if (generation !== undefined && generation !== this.operationGeneration) return;
        this.pendingOperation.set(null);
    }

    private async run(operation: DirectComputerOperation, action: () => void | Promise<void>): Promise<void> {
        const generation = this.begin(operation);
        try {
            await action();
        } finally {
            this.finish(generation);
        }
    }

    importPackage(workflow: PackageImportWorkflow, target: DiskTreeItem): void {
        if (!this.enabled) {
            workflow.open(target);
            return;
        }
        void this.run('package-import', async () => {
            workflow.open(target);
            await workflow.chooseLocal(true);
        });
    }

    importVolumePackages(workflow: PackageBatchImportWorkflow, target: DiskTreeItem): void {
        if (!this.enabled) {
            workflow.open(target);
            return;
        }
        void this.run('package-batch-import', async () => {
            workflow.open(target);
            await workflow.chooseLocal(true);
        });
    }

    exportPackage(workflow: ExportWorkflow, items: PackageExportSelection[]): void {
        if (!this.enabled) {
            workflow.requestPackage(items);
            return;
        }
        void this.run('package-export', async () => {
            workflow.requestPackage(items);
            await workflow.packageToComputer(true);
        });
    }

    async exportAudio(workflow: ExportWorkflow, items: PackageExportSelection[]): Promise<void> {
        if (!this.enabled) {
            await workflow.requestAudio(items);
            return;
        }
        await this.run('audio-export', async () => {
            await workflow.requestAudio(items);
            if (workflow.audioRequest?.inspection) await workflow.audioToComputer(true);
        });
    }

    exportMidi(workflow: ExportWorkflow, items: PackageExportObject[]): void {
        if (!this.enabled) {
            workflow.requestSequence(items);
            return;
        }
        void this.run('sequence-export', async () => {
            workflow.requestSequence(items);
            await workflow.sequenceToComputer(true);
        });
    }

    async exportVolumePackages(workflow: VolumePackageExportWorkflow, target: DiskTreeItem): Promise<void> {
        if (!this.enabled) {
            await workflow.open(target);
            return;
        }
        await this.run('volume-package-export', async () => {
            await workflow.open(target);
            if (workflow.request?.inspection) await workflow.toComputer(true);
        });
    }

    async exportVolumeFloppies(workflow: VolumeFloppyExportWorkflow, target: DiskTreeItem): Promise<void> {
        if (!this.enabled) {
            await workflow.open(target);
            return;
        }
        await this.run('volume-floppy-export', async () => {
            await workflow.open(target);
            if (workflow.request?.inspection) await workflow.toComputer(true);
        });
    }

    async exportMedia(workflow: MediaExportWorkflow, target: DiskTreeItem): Promise<void> {
        if (!this.enabled) {
            await workflow.open(target);
            return;
        }
        await this.run('media-export', async () => {
            await workflow.open(target);
            if (workflow.request?.inspection?.canExport) await workflow.toComputer(true);
        });
    }

    importAudio(workflow: AudioImportWorkflow, input?: HTMLInputElement): void {
        const generation = this.enabled ? this.begin('audio-import') : undefined;
        workflow.chooseFiles();
        if (this.enabled && input && workflow.request) {
            workflow.chooseLocal(input);
        } else if (generation !== undefined) {
            this.finish(generation);
        }
    }

    importMidi(workflow: SequenceImportWorkflow, input?: HTMLInputElement): void {
        const generation = this.enabled ? this.begin('sequence-import') : undefined;
        workflow.chooseFiles();
        if (this.enabled && input && workflow.request) {
            workflow.chooseLocal(input);
        } else if (generation !== undefined) {
            this.finish(generation);
        }
    }

    audioFilesChosen(workflow: AudioImportWorkflow, event: Event): void {
        workflow.filesChosen(event);
        this.finish();
    }

    midiFilesChosen(workflow: SequenceImportWorkflow, event: Event): void {
        workflow.filesChosen(event);
        this.finish();
    }

    cancelAudioSelection(workflow: AudioImportWorkflow): void {
        if (this.enabled && workflow.request?.files.length === 0) {
            workflow.request = null;
        }
        this.finish();
    }

    cancelMidiSelection(workflow: SequenceImportWorkflow): void {
        if (this.enabled && workflow.request?.files.length === 0) {
            workflow.request = null;
        }
        this.finish();
    }
}
