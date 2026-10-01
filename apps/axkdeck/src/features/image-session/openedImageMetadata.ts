import type { OpenedImage } from '../../lib/transport';
import type { ImageSessionWorkflow } from './workflow.svelte';

export function applyOpenedImageMetadata(workflow: ImageSessionWorkflow, opened: OpenedImage): void {
    workflow.integrityDialogOpen = false;
    workflow.integrityIssues = [];
    workflow.integrityError = '';
    workflow.integrityLoading = false;
    workflow.companionSources = opened.companionSources;
    workflow.floppySet = opened.floppySet;
    workflow.objectDeletionAvailable = opened.objectDeletionAvailable;
    workflow.waveDataCleanupAvailable = opened.waveDataCleanupAvailable;
    workflow.programGenerationAvailable = opened.programGenerationAvailable;
    workflow.programAssignmentCleanupAvailable = opened.programAssignmentCleanupAvailable;
    workflow.packageImportAvailable = opened.packageImportAvailable;
    workflow.packageExportAvailable = opened.packageExportAvailable;
    workflow.volumePackageExportAvailable = opened.volumePackageExportAvailable;
    workflow.volumeFloppyExportAvailable = opened.volumeFloppyExportAvailable;
    workflow.audioExportAvailable = opened.audioExportAvailable;
    workflow.sequenceExportAvailable = opened.sequenceExportAvailable;
    workflow.mediaConversionAvailable = opened.mediaConversionAvailable;
    workflow.extentLayoutRepairAvailable = opened.extentLayoutRepairAvailable;
    workflow.allocationInspectionAvailable = opened.allocationInspectionAvailable;
    workflow.imageFormat = opened.format ?? null;
    workflow.revision = opened.revision;
    workflow.sourceItems = opened.tree;
}
