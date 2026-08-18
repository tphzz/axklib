interface ImageSessionDialogState {
    companionRequest: unknown;
    hardDiskDirectory: unknown;
}

interface MutationDialogState {
    volumeAction: unknown;
    placementRepairRequest: unknown;
    objectRenameRequest: unknown;
    sampleBankCreationRequest: unknown;
    sampleBankAssignmentRequest: unknown;
}

interface DeletionDialogState {
    objectRequest: unknown;
    cleanupRequest: unknown;
}

interface MediaDropDialogState {
    notice: unknown;
    dragActive: boolean;
}

interface RequestDialogState {
    request: unknown;
}

interface ExportDialogState {
    packageRequest: unknown;
    audioRequest: unknown;
    sequenceRequest: unknown;
}

interface AppDialogState {
    pickerRequest: unknown;
    imageSession: ImageSessionDialogState;
    workspaceManagerOpen: boolean;
    connectionSettings: unknown;
    mutation: MutationDialogState;
    packageImport: RequestDialogState;
    packageBatchImport: RequestDialogState;
    exports: ExportDialogState;
    volumePackages: RequestDialogState;
    volumeFloppies: RequestDialogState;
    mediaExports: RequestDialogState;
    deletion: DeletionDialogState;
    programGeneration: RequestDialogState;
    audioImport: RequestDialogState;
    sequenceImport: RequestDialogState;
    tx16wImport: RequestDialogState;
    mediaDrop: MediaDropDialogState;
}

export function hasOpenAppDialog(state: AppDialogState): boolean {
    return [
        state.pickerRequest,
        state.imageSession.companionRequest,
        state.imageSession.hardDiskDirectory,
        state.workspaceManagerOpen,
        state.connectionSettings,
        state.mutation.volumeAction,
        state.mutation.placementRepairRequest,
        state.mutation.objectRenameRequest,
        state.mutation.sampleBankCreationRequest,
        state.mutation.sampleBankAssignmentRequest,
        state.packageImport.request,
        state.packageBatchImport.request,
        state.exports.packageRequest,
        state.volumePackages.request,
        state.volumeFloppies.request,
        state.exports.audioRequest,
        state.exports.sequenceRequest,
        state.mediaExports.request,
        state.deletion.objectRequest,
        state.deletion.cleanupRequest,
        state.programGeneration.request,
        state.audioImport.request,
        state.sequenceImport.request,
        state.tx16wImport.request,
        state.mediaDrop.notice,
        state.mediaDrop.dragActive,
    ].some(Boolean);
}
