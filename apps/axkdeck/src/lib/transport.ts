import type { DiskTreeItem } from './types';
import type {
    DirectoryListing,
    DirectoryLocation,
    DirectoryRef,
    FileRef,
    FileLocation,
    InputFileLocation,
    SandboxRoot,
    ClientUploadLocation,
    UploadKind,
} from './storageLocations';
import type { ClientUploadSource } from './clientUploadSource';

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
    tree: DiskTreeItem[];
    validation: ValidationSummary;
    objects: SamplerObject[];
    objectTotalCount: number;
    initialVolume: DiskTreeItem | null;
    volumeMutationsAvailable: boolean;
    partitionMutationsAvailable: boolean;
    objectDeletionAvailable: boolean;
}

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

export interface ObjectDeletionNotice {
    code: string;
    message: string;
    objectIds: string[];
}

export interface ObjectDeletionImpact {
    objectId: string;
    objectType: 'SBAC' | 'SBNK' | 'SMPL';
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
    valid: boolean;
    imageId: string;
    revision: number;
    targetObjectId: string;
    selectedObjectIds: string[];
    impacts: ObjectDeletionImpact[];
    references: ObjectDeletionReference[];
    blockers: ObjectDeletionNotice[];
    warnings: ObjectDeletionNotice[];
    estimatedFreedBytes: number;
    estimatedFreedClusters: number;
}

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

export interface SamplerRelationship {
    id: string;
    sourceObjectId: string;
    targetObjectId?: string;
    candidateObjectIds: string[];
    relationshipType: string;
    quality: string;
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

export interface AuditionDescriptor {
    auditionId: string;
    objectId: string;
    sampleRate: number;
    channels: number;
    sampleWidthBytes: number;
    frameCount: number;
    wavSizeBytes: number;
    loopMode: number;
    loopModeLabel: string;
    loopStartFrame: number;
    loopLengthFrames: number;
    warnings: string[];
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
}

export interface ClientDownload {
    filename: string;
    blob: Blob;
}

export interface PackageInspection {
    schemaVersion: '1.0';
    packageId: string;
    packageKind: string;
    requiredExtension: string;
    sourceMediaKind: string;
    valid: boolean;
    payloadsVerified: boolean;
    roots: { kind: string; displayName: string; nodeIds: string[] }[];
    objects: unknown[];
    relationshipCount: number;
    issues: unknown[];
}

export interface PackageImportDestination {
    packageIndex: number;
    rootIndex: number;
    partitionIndex?: number;
    groupName?: string;
    volumeName?: string;
    rawGroup?: string;
    rawVolume?: string;
    create?: boolean;
}

export interface PackageImportPlan {
    schemaVersion: '1.0';
    planToken: string;
    expiresInSeconds: number;
    planId: string;
    targetKind: 'sfs' | 'fat12-floppy' | 'iso9660';
    targetSnapshotId: string;
    valid: boolean;
    warnings: unknown[];
    conflicts: unknown[];
    actions: unknown[];
    allocation: unknown[];
}

export interface InputBinding {
    logicalPath: string;
    source: InputFileLocation;
}

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
    valid: boolean;
    issues: { code: string; message: string; fatal?: boolean }[];
}

export interface AudioImportCapabilities {
    supportedSampleRates: number[];
    defaultUnsupportedSampleRate: number;
    supportedOutputSampleWidthsBits: (8 | 16)[];
    sampleWidthPolicy: 'PRESERVE_PCM16_EXPAND_PCM8';
}

export interface AudioImportItem {
    source: ClientUploadLocation;
    sampleName: string;
    waveformNames: string[];
    rootKey: number;
    targetSampleRate: number;
}

export interface AudioImportTarget {
    partitionIndex: number;
    volumeName: string;
}

export interface ImageTransport {
    readonly storageMode: 'server' | 'unavailable';
    readonly supportsClientUploads: boolean;
    sandboxRoots(): Promise<SandboxRoot[]>;
    sandboxDirectory(directory: DirectoryRef, cursor?: string): Promise<DirectoryListing>;
    createSandboxDirectory(parent: DirectoryRef, name: string): Promise<void>;
    renameSandboxEntry(entry: FileRef, name: string): Promise<void>;
    deleteSandboxEntry(entry: FileRef): Promise<void>;
    openImage(source: FileLocation): Promise<OpenedImage>;
    refreshImage(sessionId: number): Promise<OpenedImage>;
    contentChildren(sessionId: number, parentId: string, offset: number, limit: number): Promise<ContentPage>;
    objectPage(sessionId: number, offset: number, limit: number, filter?: ObjectPageFilter): Promise<ObjectPage>;
    relationshipPage(
        sessionId: number,
        offset: number,
        limit: number,
        filter?: RelationshipPageFilter,
    ): Promise<RelationshipPage>;
    closeImage(sessionId: number): Promise<void>;
    startVolumeMutation(sessionId: number, mutation: VolumeMutation): Promise<JobState>;
    startPartitionMutation(sessionId: number, mutation: PartitionMutation): Promise<JobState>;
    inspectObjectDeletion(
        sessionId: number,
        targetObjectId: string,
        includedDependentObjectIds: string[],
    ): Promise<ObjectDeletionInspection>;
    startObjectDeletion(
        sessionId: number,
        targetObjectId: string,
        includedDependentObjectIds: string[],
    ): Promise<JobState>;
    preview(sessionId: number, objectKey: string, binCount: number): Promise<PreviewEnvelope>;
    prepareAudition(sessionId: number, objectKey: string): Promise<AuditionDescriptor>;
    readAuditionAudio(auditionId: string, wavSizeBytes: number, signal?: AbortSignal): Promise<ArrayBuffer>;
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
    startAudioImport(sessionId: number, target: AudioImportTarget, items: AudioImportItem[]): Promise<JobState>;
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
    hardDiskCreationProfiles(): Promise<HardDiskCreationProfile[]>;
    planHardDiskCreation(
        profileId: HardDiskCreationProfileId,
        partitionCount: number,
        output: FileLocation,
    ): Promise<PlanSummary>;
    startHardDiskCreation(planToken: string): Promise<JobState>;
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
