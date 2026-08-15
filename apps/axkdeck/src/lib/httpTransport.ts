import {
    AxklibApiError,
    AxklibHttpApiClient,
    type AxklibApiConnection,
    type DownloadArchiveSnapshot,
} from './httpApiClient';
import type { components } from './generated/axklibApiV1';
import type {
    AudioImportGrouping,
    AudioImportItem,
    AudioImportCapabilities,
    AudioImportTarget,
    SampleBankCreation,
    SampleBankAssignment,
    AudioSourceInfo,
    ContentPage,
    AuditionBundleDescriptor,
    ClientDownload,
    CompanionSelection,
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
    ImageSessionMediaConversionInspection,
    ImageSessionMediaConversionSelection,
    ImageSessionPackageImportDestination,
    ImageSessionPackageImportPlan,
    InputBinding,
    JobState,
    ObjectPage,
    ObjectPageFilter,
    ObjectDeletionInspection,
    ProgramGenerationInspection,
    ProgramGenerationSelection,
    WaveDataOrphanInspection,
    ObjectRenameMutation,
    OpenedImage,
    PackageImportDestination,
    PackageImportPlan,
    PackageInspection,
    PackageOpaqueSequenceDecision,
    PackageProgramSlotAssignment,
    PackageRename,
    PartitionMutation,
    PlanSummary,
    PlacementRepairInspection,
    PlacementRepairScope,
    PreviewEnvelope,
    RelationshipPage,
    RelationshipPageFilter,
    SequenceImportItem,
    SequenceImportTarget,
    SequenceSystemExclusivePolicy,
    MidiInspection,
    Tx16wImportInspection,
    Tx16wImportMode,
    VolumeMutation,
    VolumeDeletionInspection,
} from './transport';
import {
    clientUploadLocation,
    type ClientUploadLocation,
    type DirectoryListing,
    type DirectoryLocation,
    type DirectoryRef,
    type FileLocation,
    type FileRef,
    type ImageLocation,
    type InputFileLocation,
    type SandboxRoot,
    type UploadKind,
} from './storageLocations';
import type { ClientUploadSource } from './clientUploadSource';
import { downloadServerFile, readDirectoryArchive } from './httpDownloads';
import { HttpImageSessions } from './httpImageSessions';
import { HttpImportOperations } from './httpImportOperations';
import { HttpJobController } from './httpJobController';
import { HttpPackageOperations } from './httpPackageOperations';
import type { ApiAlterationInspection, ApiWritePlan } from './httpTransportModels';
import { planSummary } from './httpTransportModels';
import {
    createPlanKey,
    objectRenameOperation,
    randomIdempotencyKey,
    serverDirectory,
    serverFile,
    serverInput,
    serverInputBindings,
    volumeMutationOperation,
} from './httpTransportWire';
type HttpImageTransportConnection = AxklibApiConnection;
export class HttpImageTransport implements ImageTransport {
    readonly storageMode = 'server' as const;
    readonly supportsClientUploads = true;
    private readonly client: AxklibHttpApiClient;
    private readonly imageSessions: HttpImageSessions;
    private readonly jobs: HttpJobController;
    private readonly imports: HttpImportOperations;
    private readonly packages: HttpPackageOperations;
    private readonly createPlans = new Map<string, ApiWritePlan>();

    constructor(connection: HttpImageTransportConnection) {
        this.client = new AxklibHttpApiClient(connection);
        this.jobs = new HttpJobController(this.client);
        this.imageSessions = new HttpImageSessions(this.client, this.jobs);
        this.imports = new HttpImportOperations(this.client, this.jobs, this.imageSessions);
        this.packages = new HttpPackageOperations(this.client, this.jobs, this.imageSessions);
    }
    sandboxRoots(): Promise<SandboxRoot[]> {
        return this.client.roots();
    }
    sandboxDirectory(directory: DirectoryRef, cursor?: string): Promise<DirectoryListing> {
        return this.client.listDirectory(directory, { cursor });
    }
    inspectSandboxMediaSource(directory: DirectoryRef): Promise<'AXK_OBJECT_DIRECTORY' | null> {
        return this.client.inspectMediaSource(directory);
    }
    async createSandboxDirectory(parent: DirectoryRef, name: string): Promise<void> {
        await this.client.createDirectory(parent, name);
    }
    async renameSandboxEntry(entry: FileRef, name: string): Promise<void> {
        await this.client.renameEntry(entry, name);
    }
    async deleteSandboxEntry(entry: FileRef): Promise<void> {
        await this.client.deleteEntry(entry);
    }

    async uploadClientFile(
        file: ClientUploadSource,
        kind: UploadKind,
        onProgress?: (sent: number, total: number) => void,
        signal?: AbortSignal,
    ): Promise<ClientUploadLocation> {
        const uploaded = await this.client.uploadSource(
            file,
            {
                filename: file.name,
                kind,
                mediaType: file.type || undefined,
            },
            { onProgress, signal },
        );
        return clientUploadLocation({ uploadId: uploaded.uploadId }, kind, file.name);
    }
    async releaseClientUpload(source: ClientUploadLocation): Promise<void> {
        await this.client.deleteUpload(source.reference);
    }
    async audioImportCapabilities(): Promise<AudioImportCapabilities> {
        return this.imports.capabilities();
    }
    async inspectAudio(source: InputFileLocation, targetSampleRate?: number): Promise<AudioSourceInfo> {
        return this.imports.inspectAudio(source, targetSampleRate);
    }

    inspectMidi(source: InputFileLocation): Promise<MidiInspection> {
        return this.imports.inspectMidi(source);
    }
    async inspectTx16wDiskSet(
        sessionId: number,
        sources: InputFileLocation[],
        target: AudioImportTarget,
        importMode: Tx16wImportMode,
    ): Promise<Tx16wImportInspection> {
        return this.imports.inspectTx16wDiskSet(sessionId, sources, target, importMode);
    }
    startAudioImport(
        sessionId: number,
        target: AudioImportTarget,
        items: AudioImportItem[],
        grouping: AudioImportGrouping,
    ): Promise<JobState> {
        return this.imports.startAudioImport(sessionId, target, items, grouping);
    }

    startSampleBankCreation(sessionId: number, creation: SampleBankCreation): Promise<JobState> {
        return this.imports.startSampleBankCreation(sessionId, creation);
    }
    startSampleBankAssignment(sessionId: number, assignment: SampleBankAssignment): Promise<JobState> {
        return this.imports.startSampleBankAssignment(sessionId, assignment);
    }
    startSequenceImport(
        sessionId: number,
        target: SequenceImportTarget,
        items: SequenceImportItem[],
        systemExclusivePolicy: SequenceSystemExclusivePolicy,
    ): Promise<JobState> {
        return this.imports.startSequenceImport(sessionId, target, items, systemExclusivePolicy);
    }

    startTx16wDiskSetImport(
        sessionId: number,
        sources: InputFileLocation[],
        target: AudioImportTarget,
        importMode: Tx16wImportMode,
    ): Promise<JobState> {
        return this.imports.startTx16wDiskSetImport(sessionId, sources, target, importMode);
    }
    async downloadFile(location: FileLocation): Promise<ClientDownload> {
        const source = serverFile(location);
        return downloadServerFile(this.client, source.reference, source.displayName);
    }
    async downloadDirectory(location: DirectoryLocation): Promise<ClientDownload> {
        const source = serverDirectory(location);
        const submitted = await this.client.invoke<never>(
            'files.archive',
            { directory: source.reference },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.jobs.isJob(submitted)) throw new Error('files.archive did not return a job');
        const localJob = this.jobs.map(submitted);
        const completed = await this.waitForJob(localJob.jobId, () => undefined);
        if (completed.status !== 'completed' || !completed.result) {
            throw new AxklibApiError(
                completed.errorCode ?? 'archive_create_failed',
                completed.error ?? 'Directory archive creation did not complete',
                422,
                undefined,
                completed.errorContext,
            );
        }
        const archive = completed.result as DownloadArchiveSnapshot;
        try {
            return await readDirectoryArchive(this.client, archive);
        } finally {
            await this.client.deleteDirectoryArchive(archive).catch(() => undefined);
        }
    }
    inspectPackage(source: InputFileLocation, verify: boolean): Promise<PackageInspection> {
        return this.packages.inspect(source, verify);
    }
    planPackageImport(
        target: FileLocation,
        output: FileLocation,
        packages: InputFileLocation[],
        destinations: PackageImportDestination[],
        overwrite: boolean,
    ): Promise<PackageImportPlan> {
        return this.packages.planImport(target, output, packages, destinations, overwrite);
    }
    startPackageImport(planToken: string): Promise<JobState> {
        return this.packages.startImport(planToken);
    }
    planImagePackageImport(
        sessionId: number,
        sources: InputFileLocation[],
        destination: ImageSessionPackageImportDestination,
        renames: PackageRename[] = [],
        programSlotAssignments: PackageProgramSlotAssignment[] = [],
        replacePlanToken?: string,
        opaqueSequenceDecisions: PackageOpaqueSequenceDecision[] = [],
    ): Promise<ImageSessionPackageImportPlan> {
        return this.packages.planImageImport(
            sessionId,
            sources,
            destination,
            renames,
            programSlotAssignments,
            replacePlanToken,
            opaqueSequenceDecisions,
        );
    }
    releaseImagePackageImportPlan(planToken: string): Promise<void> {
        return this.packages.releaseImageImportPlan(planToken);
    }
    startImagePackageImport(planToken: string): Promise<JobState> {
        return this.packages.startImageImport(planToken);
    }
    startImagePackageExport(
        sessionId: number,
        roots: ImageSessionExportRoot[],
        destination: ImageSessionPackageExportDestination,
    ): Promise<JobState> {
        return this.packages.startImageExport(sessionId, roots, destination);
    }
    inspectImageVolumePackageExport(
        sessionId: number,
        scopeId: string,
    ): Promise<ImageSessionVolumePackageExportInspection> {
        return this.packages.inspectVolumePackageExport(sessionId, scopeId);
    }
    startImageVolumePackageExport(
        sessionId: number,
        scopeId: string,
        destination: ImageSessionVolumePackageExportDestination,
    ): Promise<JobState> {
        return this.packages.startVolumePackageExport(sessionId, scopeId, destination);
    }
    inspectImageVolumeFloppyExport(
        sessionId: number,
        scopeId: string,
    ): Promise<ImageSessionVolumeFloppyExportInspection> {
        return this.packages.inspectVolumeFloppyExport(sessionId, scopeId);
    }
    startImageVolumeFloppyExport(
        sessionId: number,
        scopeId: string,
        destination: ImageSessionVolumeFloppyExportDestination,
    ): Promise<JobState> {
        return this.packages.startVolumeFloppyExport(sessionId, scopeId, destination);
    }
    inspectImageAudioExport(
        sessionId: number,
        roots: ImageSessionExportRoot[],
    ): Promise<ImageSessionAudioExportInspection> {
        return this.packages.inspectAudioExport(sessionId, roots);
    }
    startImageAudioExport(
        sessionId: number,
        roots: ImageSessionExportRoot[],
        format: 'SFZ' | 'WAV',
        destination: ImageSessionAudioExportDestination,
    ): Promise<JobState> {
        return this.packages.startAudioExport(sessionId, roots, format, destination);
    }
    startImageSequenceExport(
        sessionId: number,
        objectIds: string[],
        destination: ImageSessionSequenceExportDestination,
    ): Promise<JobState> {
        return this.packages.startSequenceExport(sessionId, objectIds, destination);
    }
    inspectImageMediaConversion(
        sessionId: number,
        selection: ImageSessionMediaConversionSelection,
    ): Promise<ImageSessionMediaConversionInspection> {
        return this.packages.inspectMediaConversion(sessionId, selection);
    }
    startImageMediaConversion(
        sessionId: number,
        selection: ImageSessionMediaConversionSelection,
        destination: ImageSessionMediaConversionDestination,
    ): Promise<JobState> {
        return this.packages.startMediaConversion(sessionId, selection, destination);
    }
    deleteRetainedPackage(download: components['schemas']['RetainedDownload']): Promise<void> {
        return this.packages.deleteRetained(download);
    }
    openImage(location: ImageLocation): Promise<OpenedImage> {
        return this.imageSessions.open(location);
    }
    refreshImage(sessionId: number): Promise<OpenedImage> {
        return this.imageSessions.refresh(sessionId);
    }

    attachCompanions(sessionId: number, selection: CompanionSelection): Promise<OpenedImage> {
        return this.imageSessions.attachCompanions(sessionId, selection);
    }

    contentChildren(sessionId: number, parentId: string, offset: number, limit: number): Promise<ContentPage> {
        return this.imageSessions.contentChildren(sessionId, parentId, offset, limit);
    }

    objectPage(sessionId: number, offset: number, limit: number, filter: ObjectPageFilter = {}): Promise<ObjectPage> {
        return this.imageSessions.objectPage(sessionId, offset, limit, filter);
    }

    relationshipPage(
        sessionId: number,
        offset: number,
        limit: number,
        filter: RelationshipPageFilter = {},
    ): Promise<RelationshipPage> {
        return this.imageSessions.relationshipPage(sessionId, offset, limit, filter);
    }

    closeImage(sessionId: number): Promise<void> {
        return this.imageSessions.close(sessionId);
    }

    async startVolumeMutation(sessionId: number, mutation: VolumeMutation): Promise<JobState> {
        return this.imageSessions.startMutation(sessionId, volumeMutationOperation(mutation));
    }

    async startPartitionMutation(sessionId: number, mutation: PartitionMutation): Promise<JobState> {
        return this.imageSessions.startMutation(sessionId, {
            id: 'partition-rename',
            type: 'rename_partition',
            partition_index: mutation.partitionIndex,
            partition_name: mutation.partitionName,
            new_partition_name: mutation.newPartitionName,
        });
    }

    async startObjectRename(sessionId: number, mutation: ObjectRenameMutation): Promise<JobState> {
        return this.imageSessions.startMutation(sessionId, objectRenameOperation(mutation));
    }

    inspectVolumeDeletion(
        sessionId: number,
        partitionIndex: number,
        volumeName: string,
    ): Promise<VolumeDeletionInspection> {
        return this.imageSessions.inspectVolumeDeletion(sessionId, partitionIndex, volumeName);
    }

    inspectPlacement(
        sessionId: number,
        scope: PlacementRepairScope,
        recoveryVolumeName?: string,
    ): Promise<PlacementRepairInspection> {
        return this.imageSessions.inspectPlacement(sessionId, scope, recoveryVolumeName);
    }

    startPlacementRepair(
        sessionId: number,
        scope: PlacementRepairScope,
        recoveryVolumeName?: string,
    ): Promise<JobState> {
        return this.imageSessions.startPlacementRepair(sessionId, scope, recoveryVolumeName);
    }

    inspectObjectDeletion(
        sessionId: number,
        targetObjectIds: string[],
        cleanupObjectIds: string[],
    ): Promise<ObjectDeletionInspection> {
        return this.imageSessions.inspectObjectDeletion(sessionId, targetObjectIds, cleanupObjectIds);
    }

    inspectWaveDataOrphans(sessionId: number, contentScopeId: string): Promise<WaveDataOrphanInspection> {
        return this.imageSessions.inspectWaveDataOrphans(sessionId, contentScopeId);
    }

    startObjectDeletion(sessionId: number, targetObjectIds: string[], cleanupObjectIds: string[]): Promise<JobState> {
        return this.imageSessions.startObjectDeletion(sessionId, targetObjectIds, cleanupObjectIds);
    }

    inspectProgramGeneration(sessionId: number, contentScopeId: string): Promise<ProgramGenerationInspection> {
        return this.imageSessions.inspectProgramGeneration(sessionId, contentScopeId);
    }

    startProgramGeneration(
        sessionId: number,
        contentScopeId: string,
        programs: ProgramGenerationSelection[],
    ): Promise<JobState> {
        return this.imageSessions.startProgramGeneration(sessionId, contentScopeId, programs);
    }

    preview(sessionId: number, objectKey: string, binCount: number): Promise<PreviewEnvelope> {
        const session = this.imageSessions.get(sessionId);
        const query = new URLSearchParams({ objectId: objectKey, bins: String(binCount) });
        return this.client.request('GET', `/images/${encodeURIComponent(session.remoteId)}/preview?${query}`);
    }

    async prepareAuditionBundle(
        sessionId: number,
        objectKeys: readonly string[],
        signal?: AbortSignal,
    ): Promise<AuditionBundleDescriptor> {
        const session = this.imageSessions.get(sessionId);
        const submitted = await this.client.invoke<never>('auditions.prepare', {
            imageId: session.remoteId,
            objectIds: objectKeys,
        });
        if (!this.jobs.isJob(submitted)) throw new Error('auditions.prepare did not return a job');
        const localJob = this.jobs.map(submitted);
        const completed = await this.waitForJob(localJob.jobId, () => undefined, signal);
        if (completed.status !== 'completed' || !completed.result) {
            throw new AxklibApiError(
                completed.errorCode ?? 'audition_prepare_failed',
                completed.error ?? 'Audio preparation did not complete',
                422,
                undefined,
                completed.errorContext,
            );
        }
        return completed.result as AuditionBundleDescriptor;
    }

    async readAuditionContent(
        auditionId: string,
        contentSizeBytes: number,
        signal?: AbortSignal,
    ): Promise<ArrayBuffer> {
        if (!Number.isSafeInteger(contentSizeBytes) || contentSizeBytes <= 0) {
            throw new Error('Audition content size must be a positive safe integer');
        }
        const limits = await this.client.serverLimits();
        const rangeLimit = limits.maximumDownloadRangeBytes;
        if (!Number.isSafeInteger(rangeLimit) || rangeLimit <= 0) {
            throw new Error('axklib-server advertised an invalid audition range limit');
        }

        const audio = new Uint8Array(contentSizeBytes);
        for (let start = 0; start < contentSizeBytes; start += rangeLimit) {
            signal?.throwIfAborted();
            const end = Math.min(contentSizeBytes, start + rangeLimit) - 1;
            const response = await this.client.openAuditionContent(auditionId, start, end, signal);
            const bytes = new Uint8Array(await response.arrayBuffer());
            const expectedBytes = end - start + 1;
            if (bytes.byteLength !== expectedBytes) {
                throw new Error(
                    `Audition range ${start}-${end} returned ${bytes.byteLength} bytes; expected ${expectedBytes}`,
                );
            }
            audio.set(bytes, start);
        }
        return audio.buffer;
    }

    deleteAudition(auditionId: string): Promise<void> {
        return this.client.deleteAudition(auditionId);
    }

    async planCreate(
        manifest: InputFileLocation,
        output: FileLocation,
        overwrite: boolean,
        inputBindings: InputBinding[] = [],
    ): Promise<PlanSummary> {
        const outputFile = serverFile(output);
        const key = createPlanKey(manifest, outputFile, overwrite, inputBindings);
        const plan = await this.client.invoke<ApiWritePlan>('create.plan', {
            kind: 'HDS',
            manifest: serverInput(manifest),
            inputBindings: serverInputBindings(inputBindings),
            output: outputFile.reference,
            overwrite,
        });
        if (this.jobs.isJob(plan)) throw new Error('create.plan unexpectedly returned a job');
        this.createPlans.set(key, plan);
        return planSummary(plan);
    }

    async hardDiskCreationProfiles(): Promise<HardDiskCreationProfile[]> {
        const result = await this.client.invoke<components['schemas']['HardDiskCreationProfiles']>(
            'create.hds.profiles',
            {},
        );
        if (this.jobs.isJob(result)) throw new Error('create.hds.profiles unexpectedly returned a job');
        return result.profiles;
    }

    async planHardDiskCreation(
        profileId: HardDiskCreationProfileId,
        partitionCount: number,
        output: FileLocation,
    ): Promise<PlanSummary> {
        const outputFile = serverFile(output);
        const plan = await this.client.invoke<ApiWritePlan>('create.hds.plan', {
            profileId,
            partitionCount,
            output: outputFile.reference,
            overwrite: false,
        });
        if (this.jobs.isJob(plan)) throw new Error('create.hds.plan unexpectedly returned a job');
        return planSummary(plan);
    }

    async startHardDiskCreation(planToken: string): Promise<JobState> {
        const job = await this.client.invoke<never>(
            'create.hds',
            { planToken },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.jobs.isJob(job)) throw new Error('create.hds did not return a job');
        return this.jobs.map(job);
    }

    async planFloppyCreation(output: FileLocation): Promise<PlanSummary> {
        const outputFile = serverFile(output);
        const plan = await this.client.invoke<ApiWritePlan>('create.floppy.plan', {
            output: outputFile.reference,
            overwrite: false,
        });
        if (this.jobs.isJob(plan)) throw new Error('create.floppy.plan unexpectedly returned a job');
        return planSummary(plan);
    }

    async startFloppyCreation(planToken: string): Promise<JobState> {
        const job = await this.client.invoke<never>(
            'create.floppy',
            { planToken },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.jobs.isJob(job)) throw new Error('create.floppy did not return a job');
        return this.jobs.map(job);
    }

    async planAlter(
        source: FileLocation,
        manifest: InputFileLocation,
        output: FileLocation,
        overwrite: boolean,
        inputBindings: InputBinding[] = [],
    ): Promise<PlanSummary> {
        const sourceFile = serverFile(source);
        void output;
        void overwrite;
        const inspection = await this.client.invoke<ApiAlterationInspection>('alter.inspect', {
            source: sourceFile.reference,
            manifest: serverInput(manifest),
            inputBindings: serverInputBindings(inputBindings),
        });
        if (this.jobs.isJob(inspection)) throw new Error('alter.inspect unexpectedly returned a job');
        return {
            partitionCount: Number(inspection.summary.partitionCount ?? 0),
            operationCount: Number(inspection.summary.operationCount ?? 0),
            sizeBytes: Number(inspection.summary.sizeBytes ?? 0),
            appliesChanges: true,
        };
    }

    async startCreate(
        manifest: InputFileLocation,
        output: FileLocation,
        overwrite: boolean,
        inputBindings: InputBinding[] = [],
    ): Promise<JobState> {
        const outputFile = serverFile(output);
        const key = createPlanKey(manifest, outputFile, overwrite, inputBindings);
        let plan = this.createPlans.get(key);
        if (!plan) {
            await this.planCreate(manifest, outputFile, overwrite, inputBindings);
            plan = this.createPlans.get(key);
        }
        if (!plan) throw new Error('create plan was not retained');
        this.createPlans.delete(key);
        const job = await this.client.invoke<never>(
            'create.hds',
            { planToken: plan.planToken },
            {
                idempotencyKey: randomIdempotencyKey(),
            },
        );
        if (!this.jobs.isJob(job)) throw new Error('create.hds did not return a job');
        return this.jobs.map(job);
    }

    async startAlter(
        source: FileLocation,
        manifest: InputFileLocation,
        output: FileLocation,
        inputBindings: InputBinding[] = [],
    ): Promise<JobState> {
        const sourceFile = serverFile(source);
        const outputFile = serverFile(output);
        const job = await this.client.invoke<never>(
            'alter.hds',
            {
                source: sourceFile.reference,
                manifest: serverInput(manifest),
                inputBindings: serverInputBindings(inputBindings),
                output: outputFile.reference,
                overwrite: false,
            },
            {
                idempotencyKey: randomIdempotencyKey(),
            },
        );
        if (!this.jobs.isJob(job)) throw new Error('alter.hds did not return a job');
        return this.jobs.map(job);
    }

    async startExport(
        sessionId: number,
        outputDirectory: DirectoryLocation,
        overwrite: boolean,
        includeSfz: boolean,
    ): Promise<JobState> {
        const session = this.imageSessions.get(sessionId);
        const destination = serverDirectory(outputDirectory);
        const job = await this.client.invoke<never>(
            includeSfz ? 'extract.sfz' : 'extract.wav',
            {
                sources: [session.source.reference],
                destination: destination.reference,
                scope: 'FILE',
                selectors: [],
                overwrite,
                strict: true,
            },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.jobs.isJob(job)) throw new Error('extraction did not return a job');
        return this.jobs.map(job);
    }

    jobStatus(jobId: number): Promise<JobState> {
        return this.jobs.status(jobId);
    }

    waitForJob(jobId: number, onUpdate: (job: JobState) => void, signal?: AbortSignal): Promise<JobState> {
        return this.jobs.wait(jobId, onUpdate, signal);
    }

    cancelJob(jobId?: number): Promise<void> {
        return this.jobs.cancel(jobId);
    }
}
