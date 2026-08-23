import type { ImageTransport } from '../../lib/transport';
import type { DiskTreeItem, ImageTreeAction, PackageExportSelection } from '../../lib/types';
import { openAllocationInspector } from '../../lib/allocationInspector';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { DirectComputerWorkflow } from '../file-operations/directComputerWorkflow';
import type { ExportWorkflow } from '../export/workflow.svelte';
import type { MediaExportWorkflow } from '../export/mediaWorkflow.svelte';
import type { VolumeFloppyExportWorkflow } from '../export/volumeFloppyWorkflow.svelte';
import type { VolumePackageExportWorkflow } from '../export/volumePackageWorkflow.svelte';
import type { PackageBatchImportWorkflow } from '../import/packageBatchWorkflow.svelte';
import type { MutationWorkflow } from '../mutation/workflow.svelte';
import type { ImageSessionWorkflow } from './workflow.svelte';

interface ImageTreeActionDependencies {
    transport: ImageTransport;
    imageSession: ImageSessionWorkflow;
    mutation: MutationWorkflow;
    directComputer: DirectComputerWorkflow;
    packageBatchImport: PackageBatchImportWorkflow;
    exports: ExportWorkflow;
    volumePackages: VolumePackageExportWorkflow;
    volumeFloppies: VolumeFloppyExportWorkflow;
    mediaExports: MediaExportWorkflow;
    isDesktop: boolean;
    exportAudio: (items: PackageExportSelection[]) => Promise<void>;
}

export function createImageTreeActionHandler(dependencies: ImageTreeActionDependencies) {
    const { imageSession } = dependencies;
    return (item: DiskTreeItem, action: ImageTreeAction): void => {
        if (action === 'delete-volume' && item.kind === 'volume') {
            const selected = imageSession.volumeSelection.items;
            dependencies.mutation.requestVolumeDeletion(
                selected.some((candidate) => candidate.id === item.id) ? selected : [item],
            );
            return;
        }
        if (item.partitionIndex === undefined) return;
        if (action === 'inspect-allocation') {
            const sessionId = imageSession.sessionId;
            if (
                !dependencies.isDesktop ||
                sessionId === null ||
                !imageSession.allocationInspectionAvailable ||
                item.kind !== 'partition'
            )
                return;
            imageSession.selectSource(item);
            void dependencies.transport
                .allocationMapReference(sessionId)
                .then((reference) =>
                    openAllocationInspector({
                        ...reference,
                        partitionIndex: item.partitionIndex!,
                        partitionName: item.name,
                    }),
                )
                .catch((error) => imageSession.setStatus(userFacingMessage(error)));
            return;
        }
        if (action === 'import-packages') {
            if (!imageSession.packageImportAvailable || (item.kind !== 'partition' && item.kind !== 'volume')) return;
            imageSession.selectSource(item);
            dependencies.directComputer.importPackages(dependencies.packageBatchImport, item);
            return;
        }
        if (action === 'export-package') {
            if (!imageSession.packageExportAvailable || item.kind !== 'volume') return;
            imageSession.selectSource(item);
            dependencies.directComputer.exportPackage(dependencies.exports, [
                {
                    kind: 'VOLUME',
                    contentId: item.id,
                    partitionIndex: item.partitionIndex,
                    volumeName: item.name,
                    name: item.name,
                    typeLabel: 'Volume',
                },
            ]);
            return;
        }
        if (action === 'export-volume-packages') {
            if (!imageSession.volumePackageExportAvailable || item.kind !== 'partition') return;
            imageSession.selectSource(item);
            void dependencies.directComputer.exportVolumePackages(dependencies.volumePackages, item);
            return;
        }
        if (action === 'export-volume-floppies') {
            if (!imageSession.volumeFloppyExportAvailable || item.kind !== 'partition') return;
            imageSession.selectSource(item);
            void dependencies.directComputer.exportVolumeFloppies(dependencies.volumeFloppies, item);
            return;
        }
        if (action === 'export-sfz') {
            if (!imageSession.audioExportAvailable || item.kind !== 'volume') return;
            imageSession.selectSource(item);
            void dependencies.exportAudio([
                {
                    kind: 'VOLUME',
                    contentId: item.id,
                    partitionIndex: item.partitionIndex,
                    volumeName: item.name,
                    name: item.name,
                    typeLabel: 'Volume',
                },
            ]);
            return;
        }
        if (action === 'export-cdrom' || action === 'export-floppy') {
            if (!imageSession.mediaConversionAvailable) return;
            imageSession.selectSource(item);
            void dependencies.directComputer.exportMedia(dependencies.mediaExports, item);
            return;
        }
        if (dependencies.mutation.requestVolumeAction(item, action)) imageSession.selectSource(item);
    };
}
