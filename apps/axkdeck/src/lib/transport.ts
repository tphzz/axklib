import type { DiskTreeItem } from './types';
import type {
    DirectoryListing,
    DirectoryLocation,
    DirectoryRef,
    FileRef,
    FileLocation,
    ImageLocation,
    InputFileLocation,
    SandboxRoot,
    ClientUploadLocation,
    UploadKind,
} from './storageLocations';
import type { ClientUploadSource } from './clientUploadSource';
import type { components } from './generated/axklibApiV1';

export interface ValidationSummary {
    valid: boolean;
    issueCount: number;
    errorCount: number;
    warningCount: number;
    objectCount: number;
    relationshipCount: number;
}

export interface OpenedImage {
    sessionId: number;
    revision: number;
    companionSources: ImageLocation[];
    floppySet: FloppySetSummary | null;
    tree: DiskTreeItem[];
    validation: ValidationSummary;
    objects: SamplerObject[];
    objectTotalCount: number;
    initialVolume: DiskTreeItem | null;
    volumeMutationsAvailable: boolean;
    partitionMutationsAvailable: boolean;
    objectRenameAvailable: boolean;
    objectDeletionAvailable: boolean;
    waveDataCleanupAvailable: boolean;
    programGenerationAvailable: boolean;
    packageImportAvailable: boolean;
    packageExportAvailable: boolean;
    volumePackageExportAvailable: boolean;
    volumeFloppyExportAvailable: boolean;
    audioExportAvailable: boolean;
    sequenceExportAvailable: boolean;
    mediaConversionAvailable: boolean;
    extentLayoutRepairAvailable: boolean;
    allocationInspectionAvailable: boolean;
}

export interface AllocationMapReference {
    imageId: string;
    revision: number;
}

export type ImageValidationIssue = components['schemas']['ImageValidationItem'];

export type CompanionSelection = { kind: 'sources'; sources: ImageLocation[] } | { kind: 'immediate-siblings' };

export type FloppySetSummary = components['schemas']['ImageFloppySet'];

export type VolumeMutation =
    | { kind: 'add'; partitionIndex: number; volumeName: string }
    | { kind: 'rename'; partitionIndex: number; volumeName: string; newVolumeName: string }
    | { kind: 'delete'; partitionIndex: number; volumeName: string };

export interface PartitionMutation {
    kind: 'rename';
    partitionIndex: number;
    partitionName: string;
    newPartitionName: string;
}

export type ObjectRenameMutation =
    | {
          kind: 'sequence';
          partitionIndex: number;
          volumeName: string;
          sequenceName: string;
          newSequenceName: string;
      }
    | {
          kind: 'program';
          partitionIndex: number;
          volumeName: string;
          programNumber: number;
          newProgramName: string;
      }
    | {
          kind: 'sample-bank';
          partitionIndex: number;
          volumeName: string;
          sampleBankName: string;
          newSampleBankName: string;
      }
    | {
          kind: 'sample';
          partitionIndex: number;
          volumeName: string;
          sampleName: string;
          newSampleName: string;
      }
    | {
          kind: 'wave-data';
          partitionIndex: number;
          volumeName: string;
          waveformName: string;
          newWaveformName: string;
      };

export interface ObjectDeletionNotice {
    code: string;
    message: string;
    objectIds: string[];
}

export interface ObjectDeletionImpact {
    objectId: string;
    objectType: 'PROG' | 'SEQU' | 'SBAC' | 'SBNK' | 'SMPL';
    objectName: string;
    partitionIndex: number | null;
    partitionName: string;
    volumeName: string;
    role: 'TARGET' | 'DEPENDENCY';
    status: 'REQUIRED' | 'OPTIONAL' | 'PRESERVED' | 'BLOCKED';
    selected: boolean;
    storedSizeBytes: number;
    freedClusters: number;
    prerequisiteObjectIds: string[];
    reason: string;
}

export interface ObjectDeletionReference {
    sourceObjectId: string;
    sourceObjectType: 'PROG' | 'SBAC' | 'SBNK' | 'SMPL' | 'SEQU' | 'PRF3' | 'UNKNOWN';
    sourceObjectName: string;
    targetObjectId: string | null;
    targetObjectType: 'PROG' | 'SBAC' | 'SBNK' | 'SMPL' | 'SEQU' | 'PRF3' | 'UNKNOWN' | null;
    targetObjectName: string | null;
    type: string;
    quality: string;
    effect: 'BLOCKING' | 'REMOVED' | 'PRESERVED';
}

export interface ObjectDeletionInspection {
    canApply: boolean;
    imageId: string;
    revision: number;
    targetObjectIds: string[];
    selectedObjectIds: string[];
    impacts: ObjectDeletionImpact[];
    references: ObjectDeletionReference[];
    blockers: ObjectDeletionNotice[];
    warnings: ObjectDeletionNotice[];
    estimatedFreedBytes: number;
    estimatedFreedClusters: number;
}

export type VolumeDeletionInspection = components['schemas']['ImageVolumeDeletionInspection'];
export type ProgramGenerationCandidate = components['schemas']['ImageProgramGenerationCandidate'];
export type ProgramGenerationInspection = components['schemas']['ImageProgramGenerationInspection'];
export type ProgramGenerationSelection = components['schemas']['ImageProgramGenerationSelection'];
export type ProgramGenerationResult = components['schemas']['ImageProgramGenerationResult'];
export type GeneratedProgram = components['schemas']['ImageGeneratedProgram'];
export type PlacementRepairScope = components['schemas']['ImagePlacementScope'];
export type PlacementRepairInspection = components['schemas']['ImagePlacementInspection'];

export type ImageSessionVolumeFloppyExportDestination =
    components['schemas']['ImageSessionVolumeFloppyExportDestination'];
export type ImageSessionVolumeFloppyExportInspection =
    components['schemas']['ImageSessionVolumeFloppyExportInspection'];
export type ImageSessionVolumeFloppyExportResult = components['schemas']['ImageSessionVolumeFloppyExportResult'];

export type WaveDataOrphanCandidate = components['schemas']['ImageWaveDataOrphanCandidate'];
export type WaveDataOrphanInspection = components['schemas']['ImageWaveDataOrphanInspection'];

export interface ContentPage {
    items: DiskTreeItem[];
    totalCount: number;
}

export interface ObjectPage {
    objects: SamplerObject[];
    totalCount: number;
}

export interface ObjectPageFilter {
    objectType?: string;
    scopeId?: string;
}

export interface RelationshipPage {
    relationships: SamplerRelationship[];
    totalCount: number;
}

export interface RelationshipPageFilter {
    scopeId?: string;
    sourceObjectId?: string;
    targetObjectId?: string;
    relationshipType?: string;
}

export type RelationshipQuality = 'KNOWN' | 'LIKELY' | 'TENTATIVE' | 'UNKNOWN';
export type SystemProgramContext = components['schemas']['SystemProgramContext'];
export type SystemProgramContexts = components['schemas']['SystemProgramContexts'];
export type SystemProgramPart = components['schemas']['SystemProgramPart'];

export interface SamplerRelationship {
    id: string;
    sourceObjectId: string;
    targetObjectId?: string;
    candidateObjectIds: string[];
    relationshipType: string;
    quality: RelationshipQuality;
    basis: string;
    notes: string[];
    assignmentIndex?: number;
    assignmentName: string;
    assignmentState: string;
    receiveChannelDisplay: string;
}

export interface SamplerObject {
    key: string;
    objectType: string;
    name: string;
    partitionIndex: number;
    partitionName: string;
    volumeName: string;
    categoryName: string;
    sfsId: number;
    storedSizeBytes: number;
    sampleRate: number;
    rootKey: number;
    frameCount: number;
    sampleWidthBytes: number;
    fineTuneCents?: number;
    loopModeLabel?: string;
    loopStartFrame?: number;
    loopLengthFrames?: number;
    sequence?: {
        formatVersion: number;
        ticksPerQuarterNote: number;
        firstTick: number;
        endTick: number;
        eventCount: number;
        headerTempoBpm?: number;
        effectiveInitialTempoMicrosecondsPerQuarterNote: number;
        tempoEvents: readonly {
            tick: number;
            microsecondsPerQuarterNote: number;
        }[];
    };
}

export interface PreviewEnvelope {
    frameCount: number;
    lanes: readonly PreviewLane[];
}

export interface PreviewLane {
    role: 'MONO' | 'LEFT' | 'RIGHT';
    sourceObjectId: string;
    frameCount: number;
    bins: readonly { minimum: number; maximum: number }[];
}

export interface AuditionLaneDescriptor {
    role: 'MONO' | 'LEFT' | 'RIGHT';
    sourceObjectId: string;
    sampleRate: number;
    sampleWidthBytes: number;
    frameCount: number;
    wavSizeBytes: number;
    contentOffsetBytes: number;
    loopStartFrame: number;
    loopLengthFrames: number;
}

export interface AuditionClipDescriptor {
    objectId: string;
    loopMode: number;
    loopModeLabel: string;
    warnings: string[];
    lanes: AuditionLaneDescriptor[];
}

export interface AuditionBundleDescriptor {
    auditionId: string;
    contentSizeBytes: number;
    clips: AuditionClipDescriptor[];
}

export interface PlanSummary {
    partitionCount: number;
    operationCount: number;
    sizeBytes: number;
    appliesChanges: boolean;
    planToken?: string;
}

export type HardDiskCreationProfileId = 'FLOPPY_SCALE' | 'CD_R_650' | 'CD_R_700' | 'HDS_1_GIB' | 'HDS_2_GIB';

export interface HardDiskCreationPartitionOption {
    partitionCount: number;
    partitionSizeBytes: number;
    unusedTailBytes: number;
}

export interface HardDiskCreationProfile {
    profileId: HardDiskCreationProfileId;
    sizeBytes: number;
    defaultPartitionCount: number;
    partitionOptions: HardDiskCreationPartitionOption[];
}

export interface JobState {
    jobId: number;
    kind: string;
    status: 'queued' | 'running' | 'cancelling' | 'cancelled' | 'failed' | 'completed';
    progress?: { phase: number; completed: number; total?: number; label: string; outputPath?: string };
    result?: unknown;
    error?: string;
    errorCode?: string;
    errorContext?: unknown;
}

export interface ClientDownload {
    filename: string;
    blob: Blob;
}

export type PackageInspection = components['schemas']['PackageInspection'];
export type PackageImportDestination = components['schemas']['PackageDestination'];
export type PackageImportPlan = components['schemas']['PackageImportPlan'];
export type PackageRename = components['schemas']['PackageRename'];
export type PackageProgramSlotAssignment = components['schemas']['PackageProgramSlotAssignment'];
export type ImageSessionPackageImportDestination = components['schemas']['ImageSessionPackageImportDestination'];
export type PackageOpaqueSequenceDecision = components['schemas']['PackageOpaqueSequenceDecision'];
export type ImageSessionPackageImportPlan = components['schemas']['ImageSessionPackageImportPlan'];
export type ImageSessionPackageImportResult = components['schemas']['ImageSessionPackageImportResult'];
export type ImageSessionPackageExportDestination = components['schemas']['ImageSessionPackageExportDestination'];
export type ImageSessionExportRoot = components['schemas']['ImageSessionExportRoot'];
export type ImageSessionPackageExportResult = components['schemas']['ImageSessionPackageExportResult'];
export type ImageSessionVolumePackageExportDestination =
    components['schemas']['ImageSessionVolumePackageExportDestination'];
export type ImageSessionVolumePackageExportInspection =
    components['schemas']['ImageSessionVolumePackageExportInspection'];
export type ImageSessionVolumePackageExportResult = components['schemas']['ImageSessionVolumePackageExportResult'];
export type ImageSessionAudioExportDestination = components['schemas']['ImageSessionAudioExportDestination'];
export type ImageSessionAudioExportInspection = components['schemas']['ImageSessionAudioExportInspection'];
export type ImageSessionAudioExportResult = components['schemas']['ImageSessionAudioExportResult'];
export type ImageSessionSequenceExportDestination = components['schemas']['ImageSessionAudioExportDestination'];
export type ImageSessionSequenceExportResult = components['schemas']['ImageSessionSequenceExportResult'];
export type ImageSessionMediaConversionInspection = components['schemas']['ImageSessionMediaConversionInspection'];
export type ImageSessionMediaConversionDestination = components['schemas']['ImageSessionMediaConversionDestination'];
export type ImageSessionMediaConversionResult = components['schemas']['ImageSessionMediaConversionResult'];
export type ImageSessionExtentLayoutRepairDestination = components['schemas']['ImageSessionMediaConversionDestination'];
export type ImageSessionExtentLayoutRepairResult = components['schemas']['ImageSessionExtentLayoutRepairResult'];
export type RetainedDownload = components['schemas']['RetainedDownload'];

export type ImageSessionMediaConversionSelection =
    | { format: 'ISO9660'; partitionIndex: number; isoVolumeId?: string }
    | { format: 'FAT12_FLOPPY'; partitionIndex: number; volumeDirectoryId: number };

export interface InputBinding {
    logicalPath: string;
    source: InputFileLocation;
}

export type AudioSamplerSettings = components['schemas']['AudioSamplerSettings'];

export interface AudioSourceInfo {
    sourceFormat: string;
    sourceSubtype: string;
    channels: 1 | 2;
    frameCount: number;
    sourceSampleRate: number;
    outputSampleRate: number;
    sourceSampleWidthBits: 8 | 16 | 24 | 32 | 64;
    outputSampleWidthBits: 16;
    durationSeconds: number;
    resampled: boolean;
    quantized: boolean;
    sampleWidthConverted: boolean;
    ditherAlgorithm: string;
    projectedOutputFrameCount: number;
    projectedOutputBytesPerChannel: number;
    projectedOutputBytesTotal: number;
    maximumOutputFrameCountPerChannel: number;
    maximumOutputBytesPerChannel: number;
    samplerDefaults: AudioSamplerSettings;
    valid: boolean;
    issues: { code: string; message: string; fatal?: boolean }[];
}

export interface AudioImportCapabilities {
    supportedSampleRates: number[];
    defaultUnsupportedSampleRate: number;
    supportedOutputSampleWidthsBits: (8 | 16)[];
    sampleWidthPolicy: 'PRESERVE_PCM16_EXPAND_PCM8';
    maximumUploads: number;
}

export interface AudioImportItem {
    source: InputFileLocation;
    sampleName: string;
    waveformNames: string[];
    rootKey: number;
    fineTuneCents: number;
    keyLow: number;
    keyHigh: number;
    velocityLow: number;
    velocityHigh: number;
    loopMode: 1 | 4;
    loopStartFrame: number;
    loopLengthFrames: number;
    targetSampleRate: number;
}

export interface AudioImportTarget {
    partitionIndex: number;
    volumeName: string;
}

export type AudioImportGrouping = { kind: 'SAMPLES' } | { kind: 'SAMPLE_BANK'; sampleBankName: string };

export interface SampleBankCreation {
    partitionIndex: number;
    volumeName: string;
    sampleBankName: string;
    sampleNames: string[];
}

export interface SampleBankAssignment {
    partitionIndex: number;
    volumeName: string;
    sampleBankName: string;
    sampleNames: string[];
}

export interface SequenceImportItem {
    source: InputFileLocation;
    sequenceName: string;
}

export type MidiInspection = components['schemas']['MidiInspection'];
export type Tx16wImportInspection = components['schemas']['ImageSessionTx16wImportInspection'];
export type Tx16wImportMode = Tx16wImportInspection['importMode'];
export type SequenceSystemExclusivePolicy = 'exclude' | 'preserve';

export interface SequenceImportTarget {
    partitionIndex: number;
    volumeName: string;
}

export interface ImageTransport {
    readonly storageMode: 'server' | 'unavailable';
    readonly supportsClientUploads: boolean;
    sandboxRoots(): Promise<SandboxRoot[]>;
    sandboxDirectory(directory: DirectoryRef, cursor?: string): Promise<DirectoryListing>;
    inspectSandboxMediaSource(directory: DirectoryRef): Promise<'AXK_OBJECT_DIRECTORY' | null>;
    createSandboxDirectory(parent: DirectoryRef, name: string): Promise<void>;
    renameSandboxEntry(entry: FileRef, name: string): Promise<void>;
    deleteSandboxEntry(entry: FileRef): Promise<void>;
    openImage(source: ImageLocation): Promise<OpenedImage>;
    refreshImage(sessionId: number): Promise<OpenedImage>;
    attachCompanions(sessionId: number, selection: CompanionSelection): Promise<OpenedImage>;
    contentChildren(sessionId: number, parentId: string, offset: number, limit: number): Promise<ContentPage>;
    validationIssues(sessionId: number): Promise<ImageValidationIssue[]>;
    objectPage(sessionId: number, offset: number, limit: number, filter?: ObjectPageFilter): Promise<ObjectPage>;
    relationshipPage(
        sessionId: number,
        offset: number,
        limit: number,
        filter?: RelationshipPageFilter,
    ): Promise<RelationshipPage>;
    systemProgramContexts(sessionId: number, partitionIndex: number): Promise<SystemProgramContexts>;
    allocationMapReference(sessionId: number): Promise<AllocationMapReference>;
    closeImage(sessionId: number): Promise<void>;
    startVolumeMutation(sessionId: number, mutation: VolumeMutation): Promise<JobState>;
    startPartitionMutation(sessionId: number, mutation: PartitionMutation): Promise<JobState>;
    startObjectRename(sessionId: number, mutation: ObjectRenameMutation): Promise<JobState>;
    inspectVolumeDeletion(
        sessionId: number,
        partitionIndex: number,
        volumeName: string,
    ): Promise<VolumeDeletionInspection>;
    inspectPlacement(
        sessionId: number,
        scope: PlacementRepairScope,
        recoveryVolumeName?: string,
    ): Promise<PlacementRepairInspection>;
    startPlacementRepair(
        sessionId: number,
        scope: PlacementRepairScope,
        recoveryVolumeName?: string,
    ): Promise<JobState>;
    inspectObjectDeletion(
        sessionId: number,
        targetObjectIds: string[],
        cleanupObjectIds: string[],
    ): Promise<ObjectDeletionInspection>;
    inspectWaveDataOrphans(sessionId: number, contentScopeId: string): Promise<WaveDataOrphanInspection>;
    startObjectDeletion(sessionId: number, targetObjectIds: string[], cleanupObjectIds: string[]): Promise<JobState>;
    inspectProgramGeneration(sessionId: number, contentScopeId: string): Promise<ProgramGenerationInspection>;
    startProgramGeneration(
        sessionId: number,
        contentScopeId: string,
        programs: ProgramGenerationSelection[],
    ): Promise<JobState>;
    preview(sessionId: number, objectKey: string, binCount: number): Promise<PreviewEnvelope>;
    prepareAuditionBundle(
        sessionId: number,
        objectKeys: readonly string[],
        signal?: AbortSignal,
    ): Promise<AuditionBundleDescriptor>;
    readAuditionContent(auditionId: string, contentSizeBytes: number, signal?: AbortSignal): Promise<ArrayBuffer>;
    deleteAudition(auditionId: string): Promise<void>;
    uploadClientFile(
        file: ClientUploadSource,
        kind: UploadKind,
        onProgress?: (sent: number, total: number) => void,
        signal?: AbortSignal,
    ): Promise<ClientUploadLocation>;
    releaseClientUpload(source: ClientUploadLocation): Promise<void>;
    audioImportCapabilities(): Promise<AudioImportCapabilities>;
    inspectAudio(source: InputFileLocation, targetSampleRate?: number): Promise<AudioSourceInfo>;
    inspectMidi(source: InputFileLocation): Promise<MidiInspection>;
    inspectTx16wDiskSet(
        sessionId: number,
        sources: InputFileLocation[],
        target: AudioImportTarget,
        importMode: Tx16wImportMode,
    ): Promise<Tx16wImportInspection>;
    startAudioImport(
        sessionId: number,
        target: AudioImportTarget,
        items: AudioImportItem[],
        grouping: AudioImportGrouping,
    ): Promise<JobState>;
    startSampleBankCreation(sessionId: number, creation: SampleBankCreation): Promise<JobState>;
    startSampleBankAssignment(sessionId: number, assignment: SampleBankAssignment): Promise<JobState>;
    startSequenceImport(
        sessionId: number,
        target: SequenceImportTarget,
        items: SequenceImportItem[],
        systemExclusivePolicy: SequenceSystemExclusivePolicy,
    ): Promise<JobState>;
    startTx16wDiskSetImport(
        sessionId: number,
        sources: InputFileLocation[],
        target: AudioImportTarget,
        importMode: Tx16wImportMode,
    ): Promise<JobState>;
    downloadFile(source: FileLocation): Promise<ClientDownload>;
    downloadDirectory(source: DirectoryLocation): Promise<ClientDownload>;
    inspectPackage(source: InputFileLocation, verify: boolean): Promise<PackageInspection>;
    planPackageImport(
        target: FileLocation,
        output: FileLocation,
        packages: InputFileLocation[],
        destinations: PackageImportDestination[],
        overwrite: boolean,
    ): Promise<PackageImportPlan>;
    startPackageImport(planToken: string): Promise<JobState>;
    planImagePackageImport(
        sessionId: number,
        sources: InputFileLocation[],
        destination: ImageSessionPackageImportDestination,
        renames?: PackageRename[],
        programSlotAssignments?: PackageProgramSlotAssignment[],
        replacePlanToken?: string,
        opaqueSequenceDecisions?: PackageOpaqueSequenceDecision[],
    ): Promise<ImageSessionPackageImportPlan>;
    releaseImagePackageImportPlan(planToken: string): Promise<void>;
    startImagePackageImport(planToken: string): Promise<JobState>;
    startImagePackageExport(
        sessionId: number,
        roots: ImageSessionExportRoot[],
        destination: ImageSessionPackageExportDestination,
    ): Promise<JobState>;
    inspectImageVolumePackageExport(
        sessionId: number,
        scopeId: string,
    ): Promise<ImageSessionVolumePackageExportInspection>;
    startImageVolumePackageExport(
        sessionId: number,
        scopeId: string,
        destination: ImageSessionVolumePackageExportDestination,
    ): Promise<JobState>;
    inspectImageVolumeFloppyExport(
        sessionId: number,
        scopeId: string,
    ): Promise<ImageSessionVolumeFloppyExportInspection>;
    startImageVolumeFloppyExport(
        sessionId: number,
        scopeId: string,
        destination: ImageSessionVolumeFloppyExportDestination,
    ): Promise<JobState>;
    inspectImageAudioExport(
        sessionId: number,
        roots: ImageSessionExportRoot[],
    ): Promise<ImageSessionAudioExportInspection>;
    startImageAudioExport(
        sessionId: number,
        roots: ImageSessionExportRoot[],
        format: 'SFZ' | 'WAV',
        destination: ImageSessionAudioExportDestination,
    ): Promise<JobState>;
    startImageSequenceExport(
        sessionId: number,
        objectIds: string[],
        destination: ImageSessionSequenceExportDestination,
    ): Promise<JobState>;
    inspectImageMediaConversion(
        sessionId: number,
        selection: ImageSessionMediaConversionSelection,
    ): Promise<ImageSessionMediaConversionInspection>;
    startImageMediaConversion(
        sessionId: number,
        selection: ImageSessionMediaConversionSelection,
        destination: ImageSessionMediaConversionDestination,
    ): Promise<JobState>;
    startExtentLayoutRepair(
        sessionId: number,
        destination: ImageSessionExtentLayoutRepairDestination,
    ): Promise<JobState>;
    deleteRetainedPackage(download: RetainedDownload): Promise<void>;
    hardDiskCreationProfiles(): Promise<HardDiskCreationProfile[]>;
    planHardDiskCreation(
        profileId: HardDiskCreationProfileId,
        partitionCount: number,
        output: FileLocation,
    ): Promise<PlanSummary>;
    startHardDiskCreation(planToken: string): Promise<JobState>;
    planFloppyCreation(output: FileLocation): Promise<PlanSummary>;
    startFloppyCreation(planToken: string): Promise<JobState>;
    planCreate(
        manifest: InputFileLocation,
        output: FileLocation,
        overwrite: boolean,
        inputBindings?: InputBinding[],
    ): Promise<PlanSummary>;
    planAlter(
        source: FileLocation,
        manifest: InputFileLocation,
        output: FileLocation,
        overwrite: boolean,
        inputBindings?: InputBinding[],
    ): Promise<PlanSummary>;
    startCreate(
        manifest: InputFileLocation,
        output: FileLocation,
        overwrite: boolean,
        inputBindings?: InputBinding[],
    ): Promise<JobState>;
    startAlter(
        source: FileLocation,
        manifest: InputFileLocation,
        output: FileLocation,
        inputBindings?: InputBinding[],
    ): Promise<JobState>;
    startExport(
        sessionId: number,
        outputDirectory: DirectoryLocation,
        overwrite: boolean,
        includeSfz: boolean,
    ): Promise<JobState>;
    jobStatus(jobId: number): Promise<JobState>;
    waitForJob(jobId: number, onUpdate: (job: JobState) => void): Promise<JobState>;
    cancelJob(jobId?: number): Promise<void>;
}
