import type {
    AudioImportItem,
    AudioImportTarget,
    AudioSourceInfo,
    AudioImportCapabilities,
    AuditionDescriptor,
    ClientDownload,
    ContentPage,
    HardDiskCreationProfile,
    HardDiskCreationProfileId,
    ImageTransport,
    InputBinding,
    JobState,
    ObjectPage,
    ObjectPageFilter,
    OpenedImage,
    PackageImportDestination,
    PackageImportPlan,
    PackageInspection,
    PartitionMutation,
    PlanSummary,
    PreviewEnvelope,
    RelationshipPage,
    RelationshipPageFilter,
    VolumeMutation,
} from '../transport';
import type {
    ClientUploadLocation,
    DirectoryListing,
    DirectoryLocation,
    DirectoryRef,
    FileLocation,
    FileRef,
    InputFileLocation,
    SandboxRoot,
    UploadKind,
} from '../storageLocations';
import type { ClientUploadSource } from '../clientUploadSource';

export interface InMemoryImageTransportOptions {
    storageMode?: ImageTransport['storageMode'];
    supportsClientUploads?: boolean;
    opened: Omit<OpenedImage, 'sessionId' | 'volumeMutationsAvailable' | 'partitionMutationsAvailable'> & {
        volumeMutationsAvailable?: boolean;
        partitionMutationsAvailable?: boolean;
    };
    preview?: PreviewEnvelope;
    onClose?: (sessionId: number) => void;
    operations?: Partial<ImageTransport>;
}

export class InMemoryImageTransport implements ImageTransport {
    readonly storageMode: ImageTransport['storageMode'];
    readonly supportsClientUploads: boolean;
    readonly calls: string[] = [];
    private nextSessionId = 1;

    constructor(private readonly options: InMemoryImageTransportOptions) {
        this.storageMode = options.storageMode ?? 'server';
        this.supportsClientUploads = options.supportsClientUploads ?? false;
    }

    sandboxRoots(): Promise<SandboxRoot[]> {
        return this.invoke('sandboxRoots', []);
    }

    sandboxDirectory(directory: DirectoryRef, cursor?: string): Promise<DirectoryListing> {
        return this.invoke('sandboxDirectory', [directory, cursor]);
    }

    createSandboxDirectory(parent: DirectoryRef, name: string): Promise<void> {
        return this.invoke('createSandboxDirectory', [parent, name]);
    }

    renameSandboxEntry(entry: FileRef, name: string): Promise<void> {
        return this.invoke('renameSandboxEntry', [entry, name]);
    }

    deleteSandboxEntry(entry: FileRef): Promise<void> {
        return this.invoke('deleteSandboxEntry', [entry]);
    }

    async openImage(source: FileLocation): Promise<OpenedImage> {
        this.calls.push('openImage');
        const configured = this.options.operations?.openImage;
        if (configured) return configured(source);
        return {
            ...this.options.opened,
            sessionId: this.nextSessionId++,
            volumeMutationsAvailable: this.options.opened.volumeMutationsAvailable ?? false,
            partitionMutationsAvailable: this.options.opened.partitionMutationsAvailable ?? false,
        };
    }

    contentChildren(sessionId: number, parentId: string, offset: number, limit: number): Promise<ContentPage> {
        return this.invoke('contentChildren', [sessionId, parentId, offset, limit]);
    }

    objectPage(sessionId: number, offset: number, limit: number, filter?: ObjectPageFilter): Promise<ObjectPage> {
        return this.invoke('objectPage', [sessionId, offset, limit, filter]);
    }

    relationshipPage(
        sessionId: number,
        offset: number,
        limit: number,
        filter?: RelationshipPageFilter,
    ): Promise<RelationshipPage> {
        return this.invoke('relationshipPage', [sessionId, offset, limit, filter]);
    }

    async closeImage(sessionId: number): Promise<void> {
        this.calls.push('closeImage');
        const configured = this.options.operations?.closeImage;
        if (configured) return configured(sessionId);
        this.options.onClose?.(sessionId);
    }

    startVolumeMutation(source: FileLocation, mutation: VolumeMutation): Promise<JobState> {
        return this.invoke('startVolumeMutation', [source, mutation]);
    }

    startPartitionMutation(source: FileLocation, mutation: PartitionMutation): Promise<JobState> {
        return this.invoke('startPartitionMutation', [source, mutation]);
    }

    async preview(sessionId: number, objectKey: string, binCount: number): Promise<PreviewEnvelope> {
        this.calls.push('preview');
        const configured = this.options.operations?.preview;
        if (configured) return configured(sessionId, objectKey, binCount);
        if (this.options.preview) return this.options.preview;
        throw this.unsupported('preview');
    }

    prepareAudition(sessionId: number, objectKey: string): Promise<AuditionDescriptor> {
        return this.invoke('prepareAudition', [sessionId, objectKey]);
    }

    readAuditionAudio(auditionId: string, wavSizeBytes: number, signal?: AbortSignal): Promise<ArrayBuffer> {
        return this.invoke('readAuditionAudio', [auditionId, wavSizeBytes, signal]);
    }

    deleteAudition(auditionId: string): Promise<void> {
        return this.invoke('deleteAudition', [auditionId]);
    }

    uploadClientFile(
        file: ClientUploadSource,
        kind: UploadKind,
        onProgress?: (sent: number, total: number) => void,
        signal?: AbortSignal,
    ): Promise<ClientUploadLocation> {
        return this.invoke('uploadClientFile', [file, kind, onProgress, signal]);
    }

    releaseClientUpload(source: ClientUploadLocation): Promise<void> {
        return this.invoke('releaseClientUpload', [source]);
    }

    audioImportCapabilities(): Promise<AudioImportCapabilities> {
        return this.invoke('audioImportCapabilities', []);
    }

    inspectAudio(source: InputFileLocation, targetSampleRate?: number): Promise<AudioSourceInfo> {
        return this.invoke('inspectAudio', [source, targetSampleRate]);
    }

    startAudioImport(source: FileLocation, target: AudioImportTarget, items: AudioImportItem[]): Promise<JobState> {
        return this.invoke('startAudioImport', [source, target, items]);
    }

    downloadFile(source: FileLocation): Promise<ClientDownload> {
        return this.invoke('downloadFile', [source]);
    }

    downloadDirectory(source: DirectoryLocation): Promise<ClientDownload> {
        return this.invoke('downloadDirectory', [source]);
    }

    inspectPackage(source: InputFileLocation, verify: boolean): Promise<PackageInspection> {
        return this.invoke('inspectPackage', [source, verify]);
    }

    planPackageImport(
        target: FileLocation,
        output: FileLocation,
        packages: InputFileLocation[],
        destinations: PackageImportDestination[],
        overwrite: boolean,
    ): Promise<PackageImportPlan> {
        return this.invoke('planPackageImport', [target, output, packages, destinations, overwrite]);
    }

    startPackageImport(planToken: string): Promise<JobState> {
        return this.invoke('startPackageImport', [planToken]);
    }

    hardDiskCreationProfiles(): Promise<HardDiskCreationProfile[]> {
        return this.invoke('hardDiskCreationProfiles', []);
    }

    planHardDiskCreation(
        profileId: HardDiskCreationProfileId,
        partitionCount: number,
        output: FileLocation,
    ): Promise<PlanSummary> {
        return this.invoke('planHardDiskCreation', [profileId, partitionCount, output]);
    }

    startHardDiskCreation(planToken: string): Promise<JobState> {
        return this.invoke('startHardDiskCreation', [planToken]);
    }

    planCreate(
        manifest: InputFileLocation,
        output: FileLocation,
        overwrite: boolean,
        inputBindings?: InputBinding[],
    ): Promise<PlanSummary> {
        return this.invoke('planCreate', [manifest, output, overwrite, inputBindings]);
    }

    planAlter(
        source: FileLocation,
        manifest: InputFileLocation,
        output: FileLocation,
        overwrite: boolean,
        inputBindings?: InputBinding[],
    ): Promise<PlanSummary> {
        return this.invoke('planAlter', [source, manifest, output, overwrite, inputBindings]);
    }

    startCreate(
        manifest: InputFileLocation,
        output: FileLocation,
        overwrite: boolean,
        inputBindings?: InputBinding[],
    ): Promise<JobState> {
        return this.invoke('startCreate', [manifest, output, overwrite, inputBindings]);
    }

    startAlter(
        source: FileLocation,
        manifest: InputFileLocation,
        output: FileLocation,
        inputBindings?: InputBinding[],
    ): Promise<JobState> {
        return this.invoke('startAlter', [source, manifest, output, inputBindings]);
    }

    startExport(
        sessionId: number,
        outputDirectory: DirectoryLocation,
        overwrite: boolean,
        includeSfz: boolean,
    ): Promise<JobState> {
        return this.invoke('startExport', [sessionId, outputDirectory, overwrite, includeSfz]);
    }

    jobStatus(jobId: number): Promise<JobState> {
        return this.invoke('jobStatus', [jobId]);
    }

    waitForJob(jobId: number, onUpdate: (job: JobState) => void): Promise<JobState> {
        return this.invoke('waitForJob', [jobId, onUpdate]);
    }

    cancelJob(jobId?: number): Promise<void> {
        return this.invoke('cancelJob', [jobId]);
    }

    private invoke<Result>(name: keyof ImageTransport, arguments_: unknown[]): Promise<Result> {
        this.calls.push(String(name));
        const operation = this.options.operations?.[name];
        if (typeof operation !== 'function') return Promise.reject(this.unsupported(String(name)));
        return Reflect.apply(operation, this.options.operations, arguments_) as Promise<Result>;
    }

    private unsupported(operation: string): Error {
        return new Error(`${operation} is not configured`);
    }
}
