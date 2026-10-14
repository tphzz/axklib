import type { OpenedImage } from '../../lib/transport';
import type { ImageSessionWorkflow } from './workflow.svelte';
import type { SessionCollaborators } from './collaborators';
import { findRefreshedVolume, findSourceItem, refreshVolumeSelection, noImageSource } from './sourceTree';
import { emptyVolumeSelection } from '../../lib/volumeSelection';
import { validationStatus } from './validationStatus';

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
    workflow.documents = opened.documents;
    workflow.documentId = opened.documentId;
    workflow.sources = opened.sources;
    workflow.activeSourceMemberId = opened.activeSourceMemberId;
    workflow.discovery = opened.discovery;
}

export async function applyOpenedImage(
    workflow: ImageSessionWorkflow,
    collaborators: SessionCollaborators,
    opened: OpenedImage,
    preferred?: { partitionIndex: number; volumeName?: string },
    preserve = false,
): Promise<void> {
    const { catalog, mutation, clearExportSelection } = collaborators;
    const previous = workflow.selectedSource;
    const preferredItem = preserve
        ? findRefreshedVolume(opened.tree, previous)
        : preferred
          ? findSourceItem(opened.tree, preferred.partitionIndex, preferred.volumeName)
          : null;
    if (preserve) {
        if (!preferredItem) throw new Error('The edited volume is no longer available');
        await catalog.refreshVolume(previous.id, preferredItem.id, preferredItem.partitionIndex ?? null);
        if (
            workflow.sessionId !== opened.sessionId ||
            workflow.documentId !== opened.documentId ||
            workflow.selectedSource.id !== previous.id
        )
            throw new Error('The workspace changed during refresh');
        workflow.volumeSelection = refreshVolumeSelection(workflow.volumeSelection, opened.tree);
    } else clearExportSelection();
    applyOpenedImageMetadata(workflow, opened);
    mutation.setCapabilities(opened);
    workflow.selectedSource = preferredItem ?? opened.initialVolume ?? opened.tree[0] ?? noImageSource();
    if (!preserve) {
        workflow.volumeSelection =
            workflow.selectedSource.kind === 'volume'
                ? { items: [workflow.selectedSource], anchorId: workflow.selectedSource.id }
                : emptyVolumeSelection();
        if (workflow.selectedSource.kind === 'volume')
            await catalog.loadVolume(workflow.selectedSource.id, workflow.selectedSource.partitionIndex ?? null);
        else catalog.clear();
    }
    workflow.status = validationStatus(opened.validation);
}
