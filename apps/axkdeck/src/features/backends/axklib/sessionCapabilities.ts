import type { ImageSessionWorkflow } from '../../image-session/workflow.svelte';

export function sessionCapabilities(workflow: ImageSessionWorkflow) {
    return {
        objectDeletionAvailable: workflow.objectDeletionAvailable,
        waveDataCleanupAvailable: workflow.waveDataCleanupAvailable,
        programGenerationAvailable: workflow.programGenerationAvailable,
        programAssignmentCleanupAvailable: workflow.programAssignmentCleanupAvailable,
        packageImportAvailable: workflow.packageImportAvailable,
        packageExportAvailable: workflow.packageExportAvailable,
        volumePackageExportAvailable: workflow.volumePackageExportAvailable,
        volumeFloppyExportAvailable: workflow.volumeFloppyExportAvailable,
        audioExportAvailable: workflow.audioExportAvailable,
        sequenceExportAvailable: workflow.sequenceExportAvailable,
        mediaConversionAvailable: workflow.mediaConversionAvailable,
        allocationInspectionAvailable: workflow.allocationInspectionAvailable,
        samplerOrderingEnabled: workflow.imageFormat === 'sfs',
    };
}
