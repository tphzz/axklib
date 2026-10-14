import type { ImageSessionWorkflow } from '../../image-session/workflow.svelte';
import type { DirectComputerWorkflow } from '../../file-operations/directComputerWorkflow';
import type { ExportWorkflow } from '../../export/workflow.svelte';
import type { PackageExportObject, PackageExportSelection } from '../../../lib/types';
import { maximumPackageExportRoots } from '../../../lib/objectSelection';

export function exportActions(images: ImageSessionWorkflow, computer: DirectComputerWorkflow, exports: ExportWorkflow) {
    return {
        requestObjectPackageExport(items: PackageExportObject[]): void {
            if (!images.packageExportAvailable || !items.length || items.length > maximumPackageExportRoots) return;
            computer.exportPackage(exports, items);
        },
        async requestAudioExport(items: PackageExportSelection[]): Promise<void> {
            if (images.audioExportAvailable) await computer.exportAudio(exports, items);
        },
        async requestWavExport(items: PackageExportSelection[]): Promise<void> {
            if (images.audioExportAvailable) await computer.exportWav(exports, items);
        },
        requestSequenceExport(items: PackageExportObject[]): void {
            if (images.sequenceExportAvailable) computer.exportMidi(exports, items);
        },
    };
}
