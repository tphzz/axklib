import { HttpImageTransport } from './httpTransport';
import type {
    ImageTransport,
    ImageSessionPackageImportPlan,
    ImageSessionAudioExportInspection,
    ImageSessionVolumePackageExportInspection,
    ImageSessionVolumeFloppyExportInspection,
    ImageSessionMediaConversionInspection,
    ImageSessionExtentLayoutRepairDestination,
    AudioImportCapabilities,
    AllocationMapReference,
    AuditionBundleDescriptor,
    ClientDownload,
    CompanionSelection,
    ContentPage,
    HardDiskCreationProfile,
    JobState,
    ObjectPage,
    ObjectDeletionInspection,
    OpenedImage,
    ImageValidationIssue,
    PackageImportPlan,
    PackageInspection,
    RetainedDownload,
    PlanSummary,
    PlacementRepairInspection,
    ProgramGenerationInspection,
    PreviewEnvelope,
    RelationshipPage,
    SystemProgramContexts,
    WaveDataOrphanInspection,
    VolumeDeletionInspection,
} from './transport';
import type { DirectoryListing, DirectoryRef, FileRef, SandboxRoot } from './storageLocations';

class UnavailableTransport implements ImageTransport {
    readonly storageMode = 'unavailable' as const;
    readonly connectionMode = 'unavailable' as const;
    readonly supportsClientUploads = false;
    private unavailable<T>(): Promise<T> {
        return Promise.reject(new Error('axklib-server is unavailable; configure or restart the server connection'));
    }

    sandboxRoots(): Promise<SandboxRoot[]> {
        return this.unavailable();
    }
    sandboxDirectory(_directory: DirectoryRef, _cursor?: string): Promise<DirectoryListing> {
        return this.unavailable();
    }
    inspectSandboxMediaSource(_directory: DirectoryRef): Promise<'AXK_OBJECT_DIRECTORY' | null> {
        return this.unavailable();
    }
    createSandboxDirectory(_parent: DirectoryRef, _name: string): Promise<void> {
        return this.unavailable();
    }
    renameSandboxEntry(_entry: FileRef, _name: string): Promise<void> {
        return this.unavailable();
    }
    deleteSandboxEntry(_entry: FileRef): Promise<void> {
        return this.unavailable();
    }
    uploadClientFile(): Promise<never> {
        return this.unavailable();
    }

    releaseClientUpload(): Promise<never> {
        return this.unavailable();
    }

    audioImportCapabilities(): Promise<AudioImportCapabilities> {
        return this.unavailable();
    }

    inspectAudio(): Promise<never> {
        return this.unavailable();
    }
    inspectMidi(): Promise<never> {
        return this.unavailable();
    }
    inspectTx16wDiskSet(): Promise<never> {
        return this.unavailable();
    }

    startAudioImport(): Promise<never> {
        return this.unavailable();
    }
    startSampleBankCreation(): Promise<never> {
        return this.unavailable();
    }
    startSampleBankAssignment(): Promise<never> {
        return this.unavailable();
    }
    startSequenceImport(): Promise<never> {
        return this.unavailable();
    }
    startTx16wDiskSetImport(): Promise<never> {
        return this.unavailable();
    }
    downloadFile(): Promise<ClientDownload> {
        return this.unavailable();
    }
    downloadDirectory(): Promise<ClientDownload> {
        return this.unavailable();
    }
    inspectPackage(): Promise<PackageInspection> {
        return this.unavailable();
    }
    planPackageImport(): Promise<PackageImportPlan> {
        return this.unavailable();
    }
    startPackageImport(): Promise<JobState> {
        return this.unavailable();
    }
    planImagePackageImport(): Promise<ImageSessionPackageImportPlan> {
        return this.unavailable();
    }
    releaseImagePackageImportPlan(): Promise<void> {
        return this.unavailable();
    }
    startImagePackageImport(): Promise<JobState> {
        return this.unavailable();
    }
    startImagePackageExport(): Promise<JobState> {
        return this.unavailable();
    }
    inspectImageVolumePackageExport(): Promise<ImageSessionVolumePackageExportInspection> {
        return this.unavailable();
    }
    startImageVolumePackageExport(): Promise<JobState> {
        return this.unavailable();
    }
    inspectImageVolumeFloppyExport(): Promise<ImageSessionVolumeFloppyExportInspection> {
        return this.unavailable();
    }
    startImageVolumeFloppyExport(): Promise<JobState> {
        return this.unavailable();
    }
    inspectImageAudioExport(): Promise<ImageSessionAudioExportInspection> {
        return this.unavailable();
    }
    startImageAudioExport(): Promise<JobState> {
        return this.unavailable();
    }
    startImageSequenceExport(): Promise<JobState> {
        return this.unavailable();
    }
    inspectImageMediaConversion(): Promise<ImageSessionMediaConversionInspection> {
        return this.unavailable();
    }
    startImageMediaConversion(): Promise<JobState> {
        return this.unavailable();
    }
    startExtentLayoutRepair(
        _sessionId: number,
        _destination: ImageSessionExtentLayoutRepairDestination,
    ): Promise<JobState> {
        return this.unavailable();
    }
    deleteRetainedPackage(_download: RetainedDownload): Promise<void> {
        return this.unavailable();
    }
    hardDiskCreationProfiles(): Promise<HardDiskCreationProfile[]> {
        return this.unavailable();
    }
    planHardDiskCreation(): Promise<PlanSummary> {
        return this.unavailable();
    }
    startHardDiskCreation(): Promise<JobState> {
        return this.unavailable();
    }
    planFloppyCreation(): Promise<PlanSummary> {
        return this.unavailable();
    }
    startFloppyCreation(): Promise<JobState> {
        return this.unavailable();
    }
    openImage(): Promise<OpenedImage> {
        return this.unavailable();
    }
    keepImageAlive(): Promise<void> {
        return this.unavailable();
    }
    refreshImage(): Promise<OpenedImage> {
        return this.unavailable();
    }
    attachCompanions(_sessionId: number, _selection: CompanionSelection): Promise<OpenedImage> {
        return this.unavailable();
    }
    contentChildren(): Promise<ContentPage> {
        return this.unavailable();
    }
    validationIssues(): Promise<ImageValidationIssue[]> {
        return this.unavailable();
    }
    objectPage(): Promise<ObjectPage> {
        return this.unavailable();
    }
    relationshipPage(): Promise<RelationshipPage> {
        return this.unavailable();
    }
    systemProgramContexts(): Promise<SystemProgramContexts> {
        return this.unavailable();
    }
    allocationMapReference(): Promise<AllocationMapReference> {
        return this.unavailable();
    }
    closeImage(): Promise<void> {
        return Promise.resolve();
    }
    startVolumeMutations(): Promise<JobState> {
        return this.unavailable();
    }
    startPartitionMutation(): Promise<JobState> {
        return this.unavailable();
    }
    startObjectRename(): Promise<JobState> {
        return this.unavailable();
    }
    inspectVolumeDeletion(): Promise<VolumeDeletionInspection> {
        return this.unavailable();
    }
    inspectPlacement(): Promise<PlacementRepairInspection> {
        return this.unavailable();
    }
    startPlacementRepair(): Promise<JobState> {
        return this.unavailable();
    }
    inspectObjectDeletion(): Promise<ObjectDeletionInspection> {
        return this.unavailable();
    }
    inspectWaveDataOrphans(): Promise<WaveDataOrphanInspection> {
        return this.unavailable();
    }
    startObjectDeletion(): Promise<JobState> {
        return this.unavailable();
    }
    inspectProgramGeneration(): Promise<ProgramGenerationInspection> {
        return this.unavailable();
    }
    startProgramGeneration(): Promise<JobState> {
        return this.unavailable();
    }
    preview(): Promise<PreviewEnvelope> {
        return this.unavailable();
    }
    prepareAuditionBundle(): Promise<AuditionBundleDescriptor> {
        return this.unavailable();
    }
    readAuditionContent(): Promise<ArrayBuffer> {
        return this.unavailable();
    }
    deleteAudition(): Promise<void> {
        return Promise.resolve();
    }
    planCreate(): Promise<PlanSummary> {
        return this.unavailable();
    }
    planAlter(): Promise<PlanSummary> {
        return this.unavailable();
    }
    startCreate(): Promise<JobState> {
        return this.unavailable();
    }
    startAlter(): Promise<JobState> {
        return this.unavailable();
    }
    startExport(): Promise<JobState> {
        return this.unavailable();
    }
    jobStatus(): Promise<JobState> {
        return this.unavailable();
    }
    waitForJob(): Promise<JobState> {
        return this.unavailable();
    }
    cancelJob(): Promise<void> {
        return Promise.resolve();
    }
}

export function createTransport(): ImageTransport {
    if (window.__AXKLIB_SERVER__) return new HttpImageTransport(window.__AXKLIB_SERVER__);
    return new UnavailableTransport();
}
