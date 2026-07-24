import { HttpImageTransport } from './httpTransport';
import type {
    ImageTransport,
    AudioImportCapabilities,
    AuditionDescriptor,
    ClientDownload,
    ContentPage,
    HardDiskCreationProfile,
    JobState,
    ObjectPage,
    ObjectDeletionInspection,
    OpenedImage,
    PackageImportPlan,
    PackageInspection,
    PlanSummary,
    PreviewEnvelope,
    RelationshipPage,
} from './transport';
import type { DirectoryListing, DirectoryRef, FileRef, SandboxRoot } from './storageLocations';

class UnavailableTransport implements ImageTransport {
    readonly storageMode = 'unavailable' as const;
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

    startAudioImport(): Promise<never> {
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
    hardDiskCreationProfiles(): Promise<HardDiskCreationProfile[]> {
        return this.unavailable();
    }
    planHardDiskCreation(): Promise<PlanSummary> {
        return this.unavailable();
    }
    startHardDiskCreation(): Promise<JobState> {
        return this.unavailable();
    }
    openImage(): Promise<OpenedImage> {
        return this.unavailable();
    }
    refreshImage(): Promise<OpenedImage> {
        return this.unavailable();
    }
    contentChildren(): Promise<ContentPage> {
        return this.unavailable();
    }
    objectPage(): Promise<ObjectPage> {
        return this.unavailable();
    }
    relationshipPage(): Promise<RelationshipPage> {
        return this.unavailable();
    }
    closeImage(): Promise<void> {
        return Promise.resolve();
    }
    startVolumeMutation(): Promise<JobState> {
        return this.unavailable();
    }
    startPartitionMutation(): Promise<JobState> {
        return this.unavailable();
    }
    inspectObjectDeletion(): Promise<ObjectDeletionInspection> {
        return this.unavailable();
    }
    startObjectDeletion(): Promise<JobState> {
        return this.unavailable();
    }
    preview(): Promise<PreviewEnvelope> {
        return this.unavailable();
    }
    prepareAudition(): Promise<AuditionDescriptor> {
        return this.unavailable();
    }
    readAuditionAudio(): Promise<ArrayBuffer> {
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
