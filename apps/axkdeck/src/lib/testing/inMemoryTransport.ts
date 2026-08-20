import type {
    AudioImportGrouping,
    AllocationMapReference,
    AudioImportItem,
    AudioImportDestination,
    AudioImportTarget,
    AudioSourceInfo,
    AudioImportCapabilities,
    AuditionBundleDescriptor,
    ClientDownload,
    CompanionSelection,
    ContentPage,
    HardDiskCreationProfile,
    HardDiskCreationProfileId,
    ImageTransport,
    ImageSessionPackageExportDestination,
    ImageSessionVolumePackageExportDestination,
    ImageSessionVolumePackageExportInspection,
    ImageSessionVolumeFloppyExportDestination,
    ImageSessionVolumeFloppyExportInspection,
    ImageSessionExportRoot,
    ImageSessionAudioExportDestination,
    ImageSessionAudioExportInspection,
    ImageSessionSequenceExportDestination,
    ImageSessionMediaConversionDestination,
    ImageSessionExtentLayoutRepairDestination,
    ImageSessionMediaConversionInspection,
    ImageSessionMediaConversionSelection,
    ImageSessionPackageImportDestination,
    ImageSessionPackageImportPlan,
    InputBinding,
    JobState,
    ObjectPage,
    ObjectPageFilter,
    ObjectDeletionInspection,
    ObjectRenameMutation,
    OpenedImage,
    PackageImportDestination,
    PackageImportPlan,
    PackageInspection,
    PackageOpaqueSequenceDecision,
    PackageProgramSlotAssignment,
    PackageRename,
    ProgramGenerationInspection,
    ProgramGenerationSelection,
    RetainedDownload,
    PartitionMutation,
    PlanSummary,
    PlacementRepairInspection,
    PlacementRepairScope,
    PreviewEnvelope,
    RelationshipPage,
    RelationshipPageFilter,
    SystemProgramContexts,
    SampleBankAssignment,
    SampleBankCreation,
    SequenceImportItem,
    SequenceImportTarget,
    SequenceSystemExclusivePolicy,
    MidiInspection,
    Tx16wImportInspection,
    Tx16wImportMode,
    VolumeMutation,
    VolumeDeletionInspection,
    WaveDataOrphanInspection,
} from '../transport';
import type {
    ClientUploadLocation,
    DirectoryListing,
    DirectoryLocation,
    DirectoryRef,
    FileLocation,
    ImageLocation,
    FileRef,
    InputFileLocation,
    SandboxRoot,
    UploadKind,
} from '../storageLocations';
import type { ClientUploadSource } from '../clientUploadSource';

export interface InMemoryImageTransportOptions {
    storageMode?: ImageTransport['storageMode'];
    connectionMode?: ImageTransport['connectionMode'];
    supportsClientUploads?: boolean;
    opened: Omit<
        OpenedImage,
        | 'sessionId'
        | 'revision'
        | 'companionSources'
        | 'floppySet'
        | 'volumeMutationsAvailable'
        | 'partitionMutationsAvailable'
        | 'objectRenameAvailable'
        | 'objectDeletionAvailable'
        | 'waveDataCleanupAvailable'
        | 'programGenerationAvailable'
        | 'packageImportAvailable'
        | 'packageExportAvailable'
        | 'volumePackageExportAvailable'
        | 'volumeFloppyExportAvailable'
        | 'audioExportAvailable'
        | 'sequenceExportAvailable'
        | 'mediaConversionAvailable'
        | 'extentLayoutRepairAvailable'
        | 'allocationInspectionAvailable'
    > & {
        revision?: number;
        volumeMutationsAvailable?: boolean;
        partitionMutationsAvailable?: boolean;
        objectRenameAvailable?: boolean;
        objectDeletionAvailable?: boolean;
        waveDataCleanupAvailable?: boolean;
        programGenerationAvailable?: boolean;
        packageImportAvailable?: boolean;
        packageExportAvailable?: boolean;
        volumePackageExportAvailable?: boolean;
        volumeFloppyExportAvailable?: boolean;
        audioExportAvailable?: boolean;
        sequenceExportAvailable?: boolean;
        mediaConversionAvailable?: boolean;
        extentLayoutRepairAvailable?: boolean;
        allocationInspectionAvailable?: boolean;
        companionSources?: ImageLocation[];
        floppySet?: OpenedImage['floppySet'];
    };
    preview?: PreviewEnvelope;
    onClose?: (sessionId: number) => void;
    operations?: Partial<ImageTransport>;
}

export class InMemoryImageTransport implements ImageTransport {
    readonly storageMode: ImageTransport['storageMode'];
    readonly connectionMode: ImageTransport['connectionMode'];
    readonly supportsClientUploads: boolean;
    readonly calls: string[] = [];
    private nextSessionId = 1;

    constructor(private readonly options: InMemoryImageTransportOptions) {
        this.storageMode = options.storageMode ?? 'server';
        this.connectionMode = options.connectionMode ?? 'remote';
        this.supportsClientUploads = options.supportsClientUploads ?? false;
    }

    sandboxRoots(): Promise<SandboxRoot[]> {
        return this.invoke('sandboxRoots', []);
    }

    sandboxDirectory(directory: DirectoryRef, cursor?: string): Promise<DirectoryListing> {
        return this.invoke('sandboxDirectory', [directory, cursor]);
    }

    inspectSandboxMediaSource(directory: DirectoryRef): Promise<'AXK_OBJECT_DIRECTORY' | null> {
        return this.invoke('inspectSandboxMediaSource', [directory]);
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

    async openImage(source: ImageLocation): Promise<OpenedImage> {
        this.calls.push('openImage');
        const configured = this.options.operations?.openImage;
        if (configured) return configured(source);
        return {
            ...this.options.opened,
            sessionId: this.nextSessionId++,
            revision: this.options.opened.revision ?? 1,
            companionSources: this.options.opened.companionSources ?? [],
            floppySet: this.options.opened.floppySet ?? null,
            volumeMutationsAvailable: this.options.opened.volumeMutationsAvailable ?? false,
            partitionMutationsAvailable: this.options.opened.partitionMutationsAvailable ?? false,
            objectRenameAvailable: this.options.opened.objectRenameAvailable ?? false,
            objectDeletionAvailable: this.options.opened.objectDeletionAvailable ?? false,
            waveDataCleanupAvailable: this.options.opened.waveDataCleanupAvailable ?? false,
            programGenerationAvailable: this.options.opened.programGenerationAvailable ?? false,
            packageImportAvailable: this.options.opened.packageImportAvailable ?? false,
            packageExportAvailable: this.options.opened.packageExportAvailable ?? false,
            volumePackageExportAvailable: this.options.opened.volumePackageExportAvailable ?? false,
            volumeFloppyExportAvailable: this.options.opened.volumeFloppyExportAvailable ?? false,
            audioExportAvailable: this.options.opened.audioExportAvailable ?? false,
            sequenceExportAvailable: this.options.opened.sequenceExportAvailable ?? false,
            mediaConversionAvailable: this.options.opened.mediaConversionAvailable ?? false,
            extentLayoutRepairAvailable: this.options.opened.extentLayoutRepairAvailable ?? false,
            allocationInspectionAvailable: this.options.opened.allocationInspectionAvailable ?? false,
        };
    }

    async refreshImage(sessionId: number): Promise<OpenedImage> {
        this.calls.push('refreshImage');
        const configured = this.options.operations?.refreshImage;
        if (configured) return configured(sessionId);
        return {
            ...this.options.opened,
            sessionId,
            revision: this.options.opened.revision ?? 1,
            companionSources: this.options.opened.companionSources ?? [],
            floppySet: this.options.opened.floppySet ?? null,
            volumeMutationsAvailable: this.options.opened.volumeMutationsAvailable ?? false,
            partitionMutationsAvailable: this.options.opened.partitionMutationsAvailable ?? false,
            objectRenameAvailable: this.options.opened.objectRenameAvailable ?? false,
            objectDeletionAvailable: this.options.opened.objectDeletionAvailable ?? false,
            waveDataCleanupAvailable: this.options.opened.waveDataCleanupAvailable ?? false,
            programGenerationAvailable: this.options.opened.programGenerationAvailable ?? false,
            packageImportAvailable: this.options.opened.packageImportAvailable ?? false,
            packageExportAvailable: this.options.opened.packageExportAvailable ?? false,
            volumePackageExportAvailable: this.options.opened.volumePackageExportAvailable ?? false,
            volumeFloppyExportAvailable: this.options.opened.volumeFloppyExportAvailable ?? false,
            audioExportAvailable: this.options.opened.audioExportAvailable ?? false,
            sequenceExportAvailable: this.options.opened.sequenceExportAvailable ?? false,
            mediaConversionAvailable: this.options.opened.mediaConversionAvailable ?? false,
            extentLayoutRepairAvailable: this.options.opened.extentLayoutRepairAvailable ?? false,
            allocationInspectionAvailable: this.options.opened.allocationInspectionAvailable ?? false,
        };
    }

    async keepImageAlive(sessionId: number): Promise<void> {
        this.calls.push('keepImageAlive');
        await this.options.operations?.keepImageAlive?.(sessionId);
    }

    attachCompanions(sessionId: number, selection: CompanionSelection): Promise<OpenedImage> {
        return this.invoke('attachCompanions', [sessionId, selection]);
    }

    contentChildren(sessionId: number, parentId: string, offset: number, limit: number): Promise<ContentPage> {
        return this.invoke('contentChildren', [sessionId, parentId, offset, limit]);
    }

    validationIssues(sessionId: number): ReturnType<ImageTransport['validationIssues']> {
        return this.invoke('validationIssues', [sessionId]);
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

    systemProgramContexts(sessionId: number, partitionIndex: number): Promise<SystemProgramContexts> {
        return this.invoke('systemProgramContexts', [sessionId, partitionIndex]);
    }

    allocationMapReference(sessionId: number): Promise<AllocationMapReference> {
        return Promise.resolve({ imageId: `in-memory-${sessionId}`, revision: this.options.opened.revision ?? 1 });
    }

    async closeImage(sessionId: number): Promise<void> {
        this.calls.push('closeImage');
        const configured = this.options.operations?.closeImage;
        if (configured) return configured(sessionId);
        this.options.onClose?.(sessionId);
    }

    startVolumeMutation(sessionId: number, mutation: VolumeMutation): Promise<JobState> {
        return this.invoke('startVolumeMutation', [sessionId, mutation]);
    }

    startPartitionMutation(sessionId: number, mutation: PartitionMutation): Promise<JobState> {
        return this.invoke('startPartitionMutation', [sessionId, mutation]);
    }

    startObjectRename(sessionId: number, mutation: ObjectRenameMutation): Promise<JobState> {
        return this.invoke('startObjectRename', [sessionId, mutation]);
    }

    inspectVolumeDeletion(
        sessionId: number,
        partitionIndex: number,
        volumeName: string,
    ): Promise<VolumeDeletionInspection> {
        return this.invoke('inspectVolumeDeletion', [sessionId, partitionIndex, volumeName]);
    }

    inspectPlacement(
        sessionId: number,
        scope: PlacementRepairScope,
        recoveryVolumeName?: string,
    ): Promise<PlacementRepairInspection> {
        return this.invoke('inspectPlacement', [sessionId, scope, recoveryVolumeName]);
    }

    startPlacementRepair(
        sessionId: number,
        scope: PlacementRepairScope,
        recoveryVolumeName?: string,
    ): Promise<JobState> {
        return this.invoke('startPlacementRepair', [sessionId, scope, recoveryVolumeName]);
    }

    inspectObjectDeletion(
        sessionId: number,
        targetObjectIds: string[],
        cleanupObjectIds: string[],
    ): Promise<ObjectDeletionInspection> {
        return this.invoke('inspectObjectDeletion', [sessionId, targetObjectIds, cleanupObjectIds]);
    }

    inspectWaveDataOrphans(sessionId: number, contentScopeId: string): Promise<WaveDataOrphanInspection> {
        return this.invoke('inspectWaveDataOrphans', [sessionId, contentScopeId]);
    }

    startObjectDeletion(sessionId: number, targetObjectIds: string[], cleanupObjectIds: string[]): Promise<JobState> {
        return this.invoke('startObjectDeletion', [sessionId, targetObjectIds, cleanupObjectIds]);
    }

    inspectProgramGeneration(sessionId: number, contentScopeId: string): Promise<ProgramGenerationInspection> {
        return this.invoke('inspectProgramGeneration', [sessionId, contentScopeId]);
    }

    startProgramGeneration(
        sessionId: number,
        contentScopeId: string,
        programs: ProgramGenerationSelection[],
    ): Promise<JobState> {
        return this.invoke('startProgramGeneration', [sessionId, contentScopeId, programs]);
    }

    async preview(sessionId: number, objectKey: string, binCount: number): Promise<PreviewEnvelope> {
        this.calls.push('preview');
        const configured = this.options.operations?.preview;
        if (configured) return configured(sessionId, objectKey, binCount);
        if (this.options.preview) return this.options.preview;
        throw this.unsupported('preview');
    }

    prepareAuditionBundle(
        sessionId: number,
        objectKeys: readonly string[],
        signal?: AbortSignal,
    ): Promise<AuditionBundleDescriptor> {
        return this.invoke('prepareAuditionBundle', [sessionId, objectKeys, signal]);
    }

    readAuditionContent(auditionId: string, contentSizeBytes: number, signal?: AbortSignal): Promise<ArrayBuffer> {
        return this.invoke('readAuditionContent', [auditionId, contentSizeBytes, signal]);
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

    inspectMidi(source: InputFileLocation): Promise<MidiInspection> {
        return this.invoke('inspectMidi', [source]);
    }

    inspectTx16wDiskSet(
        sessionId: number,
        sources: InputFileLocation[],
        target: AudioImportTarget,
        importMode: Tx16wImportMode,
    ): Promise<Tx16wImportInspection> {
        return this.invoke('inspectTx16wDiskSet', [sessionId, sources, target, importMode]);
    }

    startAudioImport(
        sessionId: number,
        target: AudioImportDestination,
        items: AudioImportItem[],
        grouping: AudioImportGrouping,
    ): Promise<JobState> {
        return this.invoke('startAudioImport', [sessionId, target, items, grouping]);
    }

    startSampleBankCreation(sessionId: number, creation: SampleBankCreation): Promise<JobState> {
        return this.invoke('startSampleBankCreation', [sessionId, creation]);
    }

    startSampleBankAssignment(sessionId: number, assignment: SampleBankAssignment): Promise<JobState> {
        return this.invoke('startSampleBankAssignment', [sessionId, assignment]);
    }

    startSequenceImport(
        sessionId: number,
        target: SequenceImportTarget,
        items: SequenceImportItem[],
        systemExclusivePolicy: SequenceSystemExclusivePolicy,
    ): Promise<JobState> {
        return this.invoke('startSequenceImport', [sessionId, target, items, systemExclusivePolicy]);
    }

    startTx16wDiskSetImport(
        sessionId: number,
        sources: InputFileLocation[],
        target: AudioImportTarget,
        importMode: Tx16wImportMode,
    ): Promise<JobState> {
        return this.invoke('startTx16wDiskSetImport', [sessionId, sources, target, importMode]);
    }

    startImageSequenceExport(
        sessionId: number,
        objectIds: string[],
        destination: ImageSessionSequenceExportDestination,
    ): Promise<JobState> {
        return this.invoke('startImageSequenceExport', [sessionId, objectIds, destination]);
    }

    inspectImageMediaConversion(
        sessionId: number,
        selection: ImageSessionMediaConversionSelection,
    ): Promise<ImageSessionMediaConversionInspection> {
        return this.invoke('inspectImageMediaConversion', [sessionId, selection]);
    }

    startImageMediaConversion(
        sessionId: number,
        selection: ImageSessionMediaConversionSelection,
        destination: ImageSessionMediaConversionDestination,
    ): Promise<JobState> {
        return this.invoke('startImageMediaConversion', [sessionId, selection, destination]);
    }

    startExtentLayoutRepair(
        sessionId: number,
        destination: ImageSessionExtentLayoutRepairDestination,
    ): Promise<JobState> {
        return this.invoke('startExtentLayoutRepair', [sessionId, destination]);
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

    planImagePackageImport(
        sessionId: number,
        sources: InputFileLocation[],
        destination: ImageSessionPackageImportDestination,
        renames?: PackageRename[],
        programSlotAssignments?: PackageProgramSlotAssignment[],
        replacePlanToken?: string,
        opaqueSequenceDecisions?: PackageOpaqueSequenceDecision[],
    ): Promise<ImageSessionPackageImportPlan> {
        return this.invoke('planImagePackageImport', [
            sessionId,
            sources,
            destination,
            renames,
            programSlotAssignments,
            replacePlanToken,
            opaqueSequenceDecisions,
        ]);
    }

    releaseImagePackageImportPlan(planToken: string): Promise<void> {
        return this.invoke('releaseImagePackageImportPlan', [planToken]);
    }

    startImagePackageImport(planToken: string): Promise<JobState> {
        return this.invoke('startImagePackageImport', [planToken]);
    }

    startImagePackageExport(
        sessionId: number,
        roots: ImageSessionExportRoot[],
        destination: ImageSessionPackageExportDestination,
    ): Promise<JobState> {
        return this.invoke('startImagePackageExport', [sessionId, roots, destination]);
    }

    inspectImageVolumePackageExport(
        sessionId: number,
        scopeId: string,
    ): Promise<ImageSessionVolumePackageExportInspection> {
        return this.invoke('inspectImageVolumePackageExport', [sessionId, scopeId]);
    }

    startImageVolumePackageExport(
        sessionId: number,
        scopeId: string,
        destination: ImageSessionVolumePackageExportDestination,
    ): Promise<JobState> {
        return this.invoke('startImageVolumePackageExport', [sessionId, scopeId, destination]);
    }

    inspectImageVolumeFloppyExport(
        sessionId: number,
        scopeId: string,
    ): Promise<ImageSessionVolumeFloppyExportInspection> {
        return this.invoke('inspectImageVolumeFloppyExport', [sessionId, scopeId]);
    }

    startImageVolumeFloppyExport(
        sessionId: number,
        scopeId: string,
        destination: ImageSessionVolumeFloppyExportDestination,
    ): Promise<JobState> {
        return this.invoke('startImageVolumeFloppyExport', [sessionId, scopeId, destination]);
    }

    inspectImageAudioExport(
        sessionId: number,
        roots: ImageSessionExportRoot[],
    ): Promise<ImageSessionAudioExportInspection> {
        return this.invoke('inspectImageAudioExport', [sessionId, roots]);
    }

    startImageAudioExport(
        sessionId: number,
        roots: ImageSessionExportRoot[],
        format: 'SFZ' | 'WAV',
        destination: ImageSessionAudioExportDestination,
    ): Promise<JobState> {
        return this.invoke('startImageAudioExport', [sessionId, roots, format, destination]);
    }

    deleteRetainedPackage(download: RetainedDownload): Promise<void> {
        return this.invoke('deleteRetainedPackage', [download]);
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

    planFloppyCreation(output: FileLocation): Promise<PlanSummary> {
        return this.invoke('planFloppyCreation', [output]);
    }

    startFloppyCreation(planToken: string): Promise<JobState> {
        return this.invoke('startFloppyCreation', [planToken]);
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
