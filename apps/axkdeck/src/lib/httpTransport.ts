import {
    AxklibHttpApiClient,
    type ApiJobEvent,
    type ApiJobSnapshot,
    type AxklibApiConnection,
    type EventConnection,
} from './httpApiClient';
import type { components } from './generated/axklibApiV1';
import type { DiskTreeItem } from './types';
import type {
    AudioImportItem,
    AudioImportCapabilities,
    AudioImportTarget,
    AudioSourceInfo,
    ContentPage,
    AuditionDescriptor,
    ClientDownload,
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
    SamplerObject,
    SamplerRelationship,
    ValidationSummary,
    VolumeMutation,
} from './transport';
import {
    locationKey,
    inputLocationKey,
    clientUploadLocation,
    type ClientUploadLocation,
    type DirectoryListing,
    type DirectoryLocation,
    type DirectoryRef,
    type FileLocation,
    type FileRef,
    type InputFileLocation,
    type SandboxRoot,
    type ServerDirectoryLocation,
    type ServerFileLocation,
    type UploadKind,
} from './storageLocations';

type HttpImageTransportConnection = AxklibApiConnection;

interface ApiImageSummary {
    imageId: string;
    source: { rootId: string; relativePath: string };
    format: string;
    rootCount: number;
    objectCount: number;
    relationshipCount: number;
    availableOperations?: string[];
    validation: { valid: boolean; infoCount: number; warningCount: number; errorCount: number };
}

interface ApiContentItem {
    id: string;
    parentId: string | null;
    kind: string;
    name?: string;
    displayName: string;
    childCount: number;
    partitionIndex: number | null;
    objectId: string | null;
    objectType: string | null;
}

type ApiObjectItem = components['schemas']['ImageObjectItem'];
type ApiRelationshipItem = components['schemas']['ImageRelationshipItem'];

interface ApiPage<Item> {
    items: Item[];
    totalCount: number;
    nextCursor: string | null;
}

interface ApiWritePlan {
    planToken: string;
    kind: string;
    summary: Record<string, unknown>;
}

interface ApiAlterationInspection {
    kind: string;
    summary: Record<string, unknown>;
}

interface SessionState {
    remoteId: string;
    source: ServerFileLocation;
    contentCursors: Map<string, Map<number, string | null>>;
    contentItems: Map<string, DiskTreeItem>;
    objectCursors: Map<string, Map<number, string | null>>;
    relationshipCursors: Map<string, Map<number, string | null>>;
}

function itemKind(kind: string): DiskTreeItem['kind'] {
    if (kind === 'partition') return 'partition';
    if (kind === 'volume') return 'volume';
    if (kind === 'category') return 'category';
    return 'object';
}

function mapContentItem(item: ApiContentItem, parent?: DiskTreeItem): DiskTreeItem {
    const kind = itemKind(item.kind);
    const name = item.name ?? item.displayName;
    const volumeId = kind === 'volume' ? item.id : parent?.volumeId;
    const volumeName = kind === 'volume' ? name : parent?.volumeName;
    const partitionIndex = item.partitionIndex ?? parent?.partitionIndex;
    return {
        id: item.id,
        name,
        kind,
        childCount: item.childCount,
        objectId: item.objectId ?? undefined,
        objectType: item.objectType ?? undefined,
        volumeId,
        volumeName,
        partitionIndex,
    };
}

function mapObject(item: ApiObjectItem): SamplerObject {
    return {
        key: item.id,
        objectType: item.type,
        name: item.name,
        partitionIndex: item.partitionIndex ?? 0,
        partitionName: item.partitionName,
        volumeName: item.volumeName,
        categoryName: item.categoryName,
        sfsId: 0,
        storedSizeBytes: item.sizeBytes,
        sampleRate: item.waveform?.sampleRate ?? 0,
        rootKey: item.waveform?.rootKey ?? 0,
        frameCount: item.waveform?.frameCount ?? 0,
        sampleWidthBytes: item.waveform?.sampleWidthBytes ?? 0,
        fineTuneCents: item.waveform?.fineTuneCents,
        loopModeLabel: item.waveform?.loopModeLabel,
        loopStartFrame: item.waveform?.loopStartFrame,
        loopLengthFrames: item.waveform?.loopLengthFrames,
    };
}

function mapRelationship(item: ApiRelationshipItem): SamplerRelationship {
    return {
        id: item.id,
        sourceObjectId: item.sourceObjectId,
        targetObjectId: item.targetObjectId ?? undefined,
        candidateObjectIds: item.candidateObjectIds,
        relationshipType: item.type,
        quality: item.quality,
        basis: item.basis,
        notes: item.notes,
        assignmentIndex: item.assignmentIndex ?? undefined,
        assignmentName: item.assignmentName,
        assignmentState: item.assignmentState,
        receiveChannelDisplay: item.receiveChannelDisplay,
    };
}

function validationSummary(summary: ApiImageSummary): ValidationSummary {
    const validation = summary.validation;
    return {
        valid: validation.valid,
        issueCount: validation.infoCount + validation.warningCount + validation.errorCount,
        errorCount: validation.errorCount,
        warningCount: validation.warningCount,
        objectCount: summary.objectCount,
        relationshipCount: summary.relationshipCount,
    };
}

function planSummary(plan: ApiWritePlan): PlanSummary {
    return {
        partitionCount: Number(plan.summary.partitionCount ?? 0),
        operationCount: Number(plan.summary.operationCount ?? 0),
        sizeBytes: Number(plan.summary.sizeBytes ?? 0),
        appliesChanges: true,
        planToken: plan.planToken,
    };
}

function randomIdempotencyKey(): string {
    return globalThis.crypto?.randomUUID?.() ?? `axkdeck-${Date.now()}-${Math.random().toString(16).slice(2)}`;
}

export class HttpImageTransport implements ImageTransport {
    readonly storageMode = 'server' as const;
    readonly supportsClientUploads = true;
    private readonly client: AxklibHttpApiClient;
    private readonly sessions = new Map<number, SessionState>();
    private readonly jobs = new Map<number, string>();
    private readonly createPlans = new Map<string, ApiWritePlan>();
    private nextSessionId = 1;
    private nextJobId = 1;

    constructor(connection: HttpImageTransportConnection) {
        this.client = new AxklibHttpApiClient(connection);
    }

    sandboxRoots(): Promise<SandboxRoot[]> {
        return this.client.roots();
    }

    sandboxDirectory(directory: DirectoryRef, cursor?: string): Promise<DirectoryListing> {
        return this.client.listDirectory(directory, { cursor });
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
        file: File,
        kind: UploadKind,
        onProgress?: (sent: number, total: number) => void,
        signal?: AbortSignal,
    ): Promise<ClientUploadLocation> {
        const uploaded = await this.client.uploadBlob(
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
        const capabilities = (await this.client.serverCapabilities()).audioImport;
        if (!capabilities) throw new Error('The connected server does not publish audio import capabilities');
        return capabilities;
    }

    async inspectAudio(source: InputFileLocation, targetSampleRate?: number): Promise<AudioSourceInfo> {
        const result = await this.client.invoke<AudioSourceInfo>('audio.inspect', {
            source: this.serverInput(source),
            ...(targetSampleRate === undefined ? {} : { targetSampleRate }),
        });
        if (this.isJob(result)) throw new Error('audio.inspect unexpectedly returned a job');
        return result;
    }

    async startAudioImport(
        source: FileLocation,
        target: AudioImportTarget,
        items: AudioImportItem[],
    ): Promise<JobState> {
        const sourceFile = this.serverFile(source);
        const operations: Record<string, unknown>[] = [];
        const inputBindings: Record<string, unknown>[] = [];
        items.forEach((item, index) => {
            const logicalPath = `audio/import-${index}`;
            operations.push({
                id: `wave-${index}`,
                type: 'insert_waveform',
                partition_index: target.partitionIndex,
                volume_name: target.volumeName,
                audio: {
                    path: logicalPath,
                    waveform_names: item.waveformNames,
                    root_key: item.rootKey,
                    target_sample_rate: item.targetSampleRate,
                },
            });
            operations.push({
                id: `sample-${index}`,
                type: 'insert_sbnk',
                partition_index: target.partitionIndex,
                volume_name: target.volumeName,
                sample_bank: {
                    name: item.sampleName,
                    waveform_name: item.waveformNames[0],
                    ...(item.waveformNames[1] ? { right_waveform_name: item.waveformNames[1] } : {}),
                    root_key: item.rootKey,
                    key_low: 0,
                    key_high: 127,
                    level: 100,
                },
            });
            inputBindings.push({ manifestPath: logicalPath, input: this.serverInput(item.source) });
        });
        const job = await this.client.invoke<never>(
            'alter.hds',
            {
                source: sourceFile.reference,
                manifest: { inline: { schema_version: '1.0', operations } },
                inputBindings,
                output: sourceFile.reference,
                replaceSource: true,
            },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.isJob(job)) throw new Error('alter.hds did not return a job');
        return this.mapJob(job);
    }

    async downloadFile(location: FileLocation): Promise<ClientDownload> {
        const source = this.serverFile(location);
        const response = await this.client.openDownload(source.reference);
        const filename = source.reference.relativePath.split('/').pop() || source.displayName;
        return { filename, blob: await response.blob() };
    }

    async downloadDirectory(location: DirectoryLocation): Promise<ClientDownload> {
        const source = this.serverDirectory(location);
        const archive = await this.client.createDirectoryArchive(source.reference);
        try {
            const response = await this.client.openDirectoryArchive(archive);
            return { filename: archive.filename, blob: await response.blob() };
        } finally {
            await this.client.deleteDirectoryArchive(archive).catch(() => undefined);
        }
    }

    async inspectPackage(source: InputFileLocation, verify: boolean): Promise<PackageInspection> {
        const result = await this.client.invoke<PackageInspection>(verify ? 'package.verify' : 'package.inspect', {
            package: this.serverInput(source),
        });
        if (this.isJob(result)) throw new Error('package inspection unexpectedly returned a job');
        return result;
    }

    async planPackageImport(
        target: FileLocation,
        output: FileLocation,
        packages: InputFileLocation[],
        destinations: PackageImportDestination[],
        overwrite: boolean,
    ): Promise<PackageImportPlan> {
        const result = await this.client.invoke<PackageImportPlan>('package.plan_import', {
            target: this.serverFile(target).reference,
            output: this.serverFile(output).reference,
            packages: packages.map((source) => this.serverInput(source)),
            destinations,
            renames: [],
            overwrite,
        });
        if (this.isJob(result)) throw new Error('package import planning unexpectedly returned a job');
        return result;
    }

    async startPackageImport(planToken: string): Promise<JobState> {
        const job = await this.client.invoke<never>(
            'package.import',
            { planToken },
            {
                idempotencyKey: randomIdempotencyKey(),
            },
        );
        if (!this.isJob(job)) throw new Error('package.import did not return a job');
        return this.mapJob(job);
    }

    async openImage(location: FileLocation): Promise<OpenedImage> {
        const source = this.serverFile(location);
        const summary = await this.client.request<ApiImageSummary>('POST', '/images', {
            source: source.reference,
        });
        const sessionId = this.nextSessionId++;
        this.sessions.set(sessionId, {
            remoteId: summary.imageId,
            source,
            contentCursors: new Map(),
            contentItems: new Map(),
            objectCursors: new Map(),
            relationshipCursors: new Map(),
        });
        try {
            const roots = await this.allContentChildren(sessionId, '');
            const disk: DiskTreeItem = {
                id: `session:${sessionId}`,
                name: source.displayName,
                kind: 'disk',
                children: roots.items,
                childCount: roots.totalCount,
            };
            const initialVolume = await this.loadVolumeHierarchy(sessionId, roots.items);
            return {
                sessionId,
                validation: validationSummary(summary),
                objects: [],
                objectTotalCount: 0,
                initialVolume,
                volumeMutationsAvailable: (summary.availableOperations ?? []).includes('images.alter.volumes'),
                partitionMutationsAvailable: (summary.availableOperations ?? []).includes('images.alter.partitions'),
                tree: [disk],
            };
        } catch (error) {
            await this.closeImage(sessionId).catch(() => undefined);
            throw error;
        }
    }

    async contentChildren(sessionId: number, parentId: string, offset: number, limit: number): Promise<ContentPage> {
        const session = this.session(sessionId);
        let cursors = session.contentCursors.get(parentId);
        if (!cursors) {
            cursors = new Map([[0, null]]);
            session.contentCursors.set(parentId, cursors);
        }
        if (!cursors.has(offset)) throw new Error('Content pages must be requested in cursor order');
        const query = new URLSearchParams({ limit: String(limit) });
        const cursor = cursors.get(offset);
        if (cursor) query.set('cursor', cursor);
        if (parentId) query.set('parentId', parentId);
        const page = await this.client.request<ApiPage<ApiContentItem>>(
            'GET',
            `/images/${encodeURIComponent(session.remoteId)}/content?${query}`,
        );
        if (page.nextCursor) cursors.set(offset + page.items.length, page.nextCursor);
        const parent = session.contentItems.get(parentId);
        const items = page.items.map((item) => mapContentItem(item, parent));
        for (const item of items) session.contentItems.set(item.id, item);
        return { items, totalCount: page.totalCount };
    }

    async objectPage(
        sessionId: number,
        offset: number,
        limit: number,
        filter: ObjectPageFilter = {},
    ): Promise<ObjectPage> {
        const session = this.session(sessionId);
        const filterKey = `${filter.scopeId ?? ''}\n${filter.objectType ?? ''}`;
        let cursors = session.objectCursors.get(filterKey);
        if (!cursors) {
            cursors = new Map([[0, null]]);
            session.objectCursors.set(filterKey, cursors);
        }
        if (!cursors.has(offset)) throw new Error('Object pages must be requested in cursor order');
        const query = new URLSearchParams({ limit: String(limit) });
        const cursor = cursors.get(offset);
        if (cursor) query.set('cursor', cursor);
        if (filter.objectType) query.set('type', filter.objectType);
        if (filter.scopeId) query.set('scopeId', filter.scopeId);
        const page = await this.client.request<ApiPage<ApiObjectItem>>(
            'GET',
            `/images/${encodeURIComponent(session.remoteId)}/objects?${query}`,
        );
        if (page.nextCursor) cursors.set(offset + page.items.length, page.nextCursor);
        return { objects: page.items.map(mapObject), totalCount: page.totalCount };
    }

    async relationshipPage(
        sessionId: number,
        offset: number,
        limit: number,
        filter: RelationshipPageFilter = {},
    ): Promise<RelationshipPage> {
        const session = this.session(sessionId);
        const filterKey = [
            filter.scopeId ?? '',
            filter.sourceObjectId ?? '',
            filter.targetObjectId ?? '',
            filter.relationshipType ?? '',
        ].join('\n');
        let cursors = session.relationshipCursors.get(filterKey);
        if (!cursors) {
            cursors = new Map([[0, null]]);
            session.relationshipCursors.set(filterKey, cursors);
        }
        if (!cursors.has(offset)) throw new Error('Relationship pages must be requested in cursor order');
        const query = new URLSearchParams({ limit: String(limit) });
        const cursor = cursors.get(offset);
        if (cursor) query.set('cursor', cursor);
        if (filter.scopeId) query.set('scopeId', filter.scopeId);
        if (filter.sourceObjectId) query.set('sourceObjectId', filter.sourceObjectId);
        if (filter.targetObjectId) query.set('targetObjectId', filter.targetObjectId);
        if (filter.relationshipType) query.set('type', filter.relationshipType);
        const page = await this.client.request<ApiPage<ApiRelationshipItem>>(
            'GET',
            `/images/${encodeURIComponent(session.remoteId)}/relationships?${query}`,
        );
        if (page.nextCursor) cursors.set(offset + page.items.length, page.nextCursor);
        return { relationships: page.items.map(mapRelationship), totalCount: page.totalCount };
    }

    private async loadVolumeHierarchy(sessionId: number, nodes: DiskTreeItem[]): Promise<DiskTreeItem | null> {
        let firstVolume: DiskTreeItem | null = null;
        for (const node of nodes) {
            if (node.kind === 'volume') {
                firstVolume ??= node;
                continue;
            }
            if (node.childCount === 0) continue;
            const children = await this.allContentChildren(sessionId, node.id);
            node.children = children.items;
            firstVolume ??= await this.loadVolumeHierarchy(sessionId, children.items);
        }
        return firstVolume;
    }

    private async allContentChildren(sessionId: number, parentId: string): Promise<ContentPage> {
        const items: DiskTreeItem[] = [];
        let totalCount = 1;
        while (items.length < totalCount) {
            const page = await this.contentChildren(sessionId, parentId, items.length, 256);
            items.push(...page.items);
            totalCount = page.totalCount;
        }
        return { items, totalCount };
    }

    async closeImage(sessionId: number): Promise<void> {
        const session = this.sessions.get(sessionId);
        if (!session) return;
        await this.client.request('DELETE', `/images/${encodeURIComponent(session.remoteId)}`);
        this.sessions.delete(sessionId);
    }

    async startVolumeMutation(source: FileLocation, mutation: VolumeMutation): Promise<JobState> {
        return this.startImageMutation(source, this.volumeMutationOperation(mutation));
    }

    async startPartitionMutation(source: FileLocation, mutation: PartitionMutation): Promise<JobState> {
        return this.startImageMutation(source, {
            id: 'partition-rename',
            type: 'rename_partition',
            partition_index: mutation.partitionIndex,
            partition_name: mutation.partitionName,
            new_partition_name: mutation.newPartitionName,
        });
    }

    private async startImageMutation(source: FileLocation, operation: Record<string, unknown>): Promise<JobState> {
        const sourceFile = this.serverFile(source);
        const job = await this.client.invoke<never>(
            'alter.hds',
            {
                source: sourceFile.reference,
                manifest: { inline: { schema_version: '1.0', operations: [operation] } },
                inputBindings: [],
                output: sourceFile.reference,
                replaceSource: true,
            },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.isJob(job)) throw new Error('alter.hds did not return a job');
        return this.mapJob(job);
    }

    preview(sessionId: number, objectKey: string, binCount: number): Promise<PreviewEnvelope> {
        const session = this.session(sessionId);
        const query = new URLSearchParams({ objectId: objectKey, bins: String(binCount) });
        return this.client.request('GET', `/images/${encodeURIComponent(session.remoteId)}/preview?${query}`);
    }

    async prepareAudition(sessionId: number, objectKey: string): Promise<AuditionDescriptor> {
        const session = this.session(sessionId);
        const submitted = await this.client.invoke<never>('auditions.prepare', {
            imageId: session.remoteId,
            objectId: objectKey,
        });
        if (!this.isJob(submitted)) throw new Error('auditions.prepare did not return a job');
        const localJob = this.mapJob(submitted);
        const completed = await this.waitForJob(localJob.jobId, () => undefined);
        if (completed.status !== 'completed' || !completed.result) {
            throw new Error(completed.error ?? 'Audio preparation did not complete');
        }
        return completed.result as AuditionDescriptor;
    }

    async readAuditionAudio(auditionId: string, wavSizeBytes: number, signal?: AbortSignal): Promise<ArrayBuffer> {
        if (!Number.isSafeInteger(wavSizeBytes) || wavSizeBytes <= 0) {
            throw new Error('Audition WAV size must be a positive safe integer');
        }
        const limits = await this.client.serverLimits();
        const rangeLimit = limits.maximumDownloadRangeBytes;
        if (!Number.isSafeInteger(rangeLimit) || rangeLimit <= 0) {
            throw new Error('axklib-server advertised an invalid audition range limit');
        }

        const audio = new Uint8Array(wavSizeBytes);
        for (let start = 0; start < wavSizeBytes; start += rangeLimit) {
            signal?.throwIfAborted();
            const end = Math.min(wavSizeBytes, start + rangeLimit) - 1;
            const response = await this.client.openAuditionAudio(auditionId, start, end, signal);
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
        const outputFile = this.serverFile(output);
        const key = this.createPlanKey(manifest, outputFile, overwrite, inputBindings);
        const plan = await this.client.invoke<ApiWritePlan>('create.plan', {
            kind: 'HDS',
            manifest: this.serverInput(manifest),
            inputBindings: this.serverInputBindings(inputBindings),
            output: outputFile.reference,
            overwrite,
        });
        if (this.isJob(plan)) throw new Error('create.plan unexpectedly returned a job');
        this.createPlans.set(key, plan);
        return planSummary(plan);
    }

    async hardDiskCreationProfiles(): Promise<HardDiskCreationProfile[]> {
        const result = await this.client.invoke<components['schemas']['HardDiskCreationProfiles']>(
            'create.hds.profiles',
            {},
        );
        if (this.isJob(result)) throw new Error('create.hds.profiles unexpectedly returned a job');
        return result.profiles;
    }

    async planHardDiskCreation(
        profileId: HardDiskCreationProfileId,
        partitionCount: number,
        output: FileLocation,
    ): Promise<PlanSummary> {
        const outputFile = this.serverFile(output);
        const plan = await this.client.invoke<ApiWritePlan>('create.hds.plan', {
            profileId,
            partitionCount,
            output: outputFile.reference,
            overwrite: false,
        });
        if (this.isJob(plan)) throw new Error('create.hds.plan unexpectedly returned a job');
        return planSummary(plan);
    }

    async startHardDiskCreation(planToken: string): Promise<JobState> {
        const job = await this.client.invoke<never>(
            'create.hds',
            { planToken },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.isJob(job)) throw new Error('create.hds did not return a job');
        return this.mapJob(job);
    }

    async planAlter(
        source: FileLocation,
        manifest: InputFileLocation,
        output: FileLocation,
        overwrite: boolean,
        inputBindings: InputBinding[] = [],
    ): Promise<PlanSummary> {
        const sourceFile = this.serverFile(source);
        void output;
        void overwrite;
        const inspection = await this.client.invoke<ApiAlterationInspection>('alter.inspect', {
            source: sourceFile.reference,
            manifest: this.serverInput(manifest),
            inputBindings: this.serverInputBindings(inputBindings),
        });
        if (this.isJob(inspection)) throw new Error('alter.inspect unexpectedly returned a job');
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
        const outputFile = this.serverFile(output);
        const key = this.createPlanKey(manifest, outputFile, overwrite, inputBindings);
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
        if (!this.isJob(job)) throw new Error('create.hds did not return a job');
        return this.mapJob(job);
    }

    async startAlter(
        source: FileLocation,
        manifest: InputFileLocation,
        output: FileLocation,
        inputBindings: InputBinding[] = [],
    ): Promise<JobState> {
        const sourceFile = this.serverFile(source);
        const outputFile = this.serverFile(output);
        const job = await this.client.invoke<never>(
            'alter.hds',
            {
                source: sourceFile.reference,
                manifest: this.serverInput(manifest),
                inputBindings: this.serverInputBindings(inputBindings),
                output: outputFile.reference,
                overwrite: false,
            },
            {
                idempotencyKey: randomIdempotencyKey(),
            },
        );
        if (!this.isJob(job)) throw new Error('alter.hds did not return a job');
        return this.mapJob(job);
    }

    async startExport(
        sessionId: number,
        outputDirectory: DirectoryLocation,
        overwrite: boolean,
        includeSfz: boolean,
    ): Promise<JobState> {
        const session = this.session(sessionId);
        const destination = this.serverDirectory(outputDirectory);
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
        if (!this.isJob(job)) throw new Error('extraction did not return a job');
        return this.mapJob(job);
    }

    async jobStatus(jobId: number): Promise<JobState> {
        const remoteId = this.jobs.get(jobId);
        if (!remoteId) throw new Error('Job is closed or unknown');
        const job = await this.client.request<ApiJobSnapshot>('GET', `/jobs/${encodeURIComponent(remoteId)}`);
        return this.mapJob(job, jobId);
    }

    waitForJob(jobId: number, onUpdate: (job: JobState) => void): Promise<JobState> {
        const remoteId = this.jobs.get(jobId);
        if (!remoteId) return Promise.reject(new Error('Job is closed or unknown'));

        return new Promise((resolve, reject) => {
            let afterSequence = 0;
            let connection: EventConnection | undefined;
            let reconnectAttempts = 0;
            let stableConnectionTimer: ReturnType<typeof setTimeout> | undefined;
            let settled = false;
            let work = Promise.resolve();

            const clearStableConnectionTimer = (): void => {
                if (stableConnectionTimer !== undefined) clearTimeout(stableConnectionTimer);
                stableConnectionTimer = undefined;
            };

            const markConnectionHealthy = (): void => {
                reconnectAttempts = 0;
                clearStableConnectionTimer();
            };

            const finish = (job: JobState): void => {
                if (settled) return;
                settled = true;
                clearStableConnectionTimer();
                connection?.close();
                resolve(job);
            };

            const fail = (reason: unknown): void => {
                if (settled) return;
                settled = true;
                clearStableConnectionTimer();
                connection?.close();
                reject(reason);
            };

            const publishSnapshot = (snapshot: ApiJobSnapshot): JobState => {
                afterSequence = Math.max(afterSequence, snapshot.latestSequence ?? afterSequence);
                const mapped = this.mapJob(snapshot, jobId);
                onUpdate(mapped);
                if (this.terminal(mapped)) finish(mapped);
                return mapped;
            };

            const publishEvent = (event: ApiJobEvent): void => {
                afterSequence = event.sequence;
                const mapped = this.mapJobEvent(event, jobId);
                onUpdate(mapped);
            };

            const reconcile = async (): Promise<void> => {
                if (settled) return;
                const replay = await this.client.replayJobEvents(remoteId, afterSequence);
                for (const event of replay.events) {
                    if (event.sequence <= afterSequence) continue;
                    if (event.sequence !== afterSequence + 1) {
                        throw new Error(`Job event replay is discontinuous after sequence ${afterSequence}`);
                    }
                    publishEvent(event);
                }
                const snapshot = await this.client.request<ApiJobSnapshot>(
                    'GET',
                    `/jobs/${encodeURIComponent(remoteId)}`,
                );
                publishSnapshot(snapshot);
            };

            const enqueue = (task: () => Promise<void>): void => {
                work = work.then(task).catch(fail);
            };

            const handleEvent = (event: ApiJobEvent): void => {
                if (settled || event.jobId !== remoteId || event.sequence <= afterSequence) return;
                markConnectionHealthy();
                if (event.sequence !== afterSequence + 1) {
                    enqueue(reconcile);
                    return;
                }
                publishEvent(event);
                if (this.terminalState(event.state)) {
                    enqueue(async () => {
                        const snapshot = await this.client.request<ApiJobSnapshot>(
                            'GET',
                            `/jobs/${encodeURIComponent(remoteId)}`,
                        );
                        publishSnapshot(snapshot);
                    });
                }
            };

            const connect = async (): Promise<void> => {
                if (settled) return;
                try {
                    connection = await this.client.connectEvents(handleEvent, () => {
                        connection = undefined;
                        clearStableConnectionTimer();
                        if (settled) return;
                        enqueue(async () => {
                            reconnectAttempts += 1;
                            await reconcile();
                            if (settled) return;
                            if (reconnectAttempts > 6) {
                                fail(new Error('Lost the axklib-server event connection'));
                                return;
                            }
                            const delay = Math.min(2_000, 100 * 2 ** (reconnectAttempts - 1));
                            setTimeout(() => void connect(), delay);
                        });
                    });
                    await connection.opened;
                    clearStableConnectionTimer();
                    stableConnectionTimer = setTimeout(markConnectionHealthy, 10_000);
                    enqueue(reconcile);
                } catch (reason) {
                    reconnectAttempts += 1;
                    await reconcile();
                    if (settled) return;
                    if (reconnectAttempts > 6) {
                        fail(reason);
                        return;
                    }
                    const delay = Math.min(2_000, 100 * 2 ** (reconnectAttempts - 1));
                    setTimeout(() => void connect(), delay);
                }
            };

            enqueue(async () => {
                await reconcile();
                if (!settled) await connect();
            });
        });
    }

    async cancelJob(jobId?: number): Promise<void> {
        if (jobId !== undefined) {
            const remoteId = this.jobs.get(jobId);
            if (remoteId) await this.client.request('DELETE', `/jobs/${encodeURIComponent(remoteId)}`);
            return;
        }
        await Promise.all(
            [...this.jobs.values()].map((remoteId) =>
                this.client.request('DELETE', `/jobs/${encodeURIComponent(remoteId)}`),
            ),
        );
    }

    private session(sessionId: number): SessionState {
        const session = this.sessions.get(sessionId);
        if (!session) throw new Error('Image session is closed or unknown');
        return session;
    }

    private isJob(value: unknown): value is ApiJobSnapshot {
        return typeof value === 'object' && value !== null && 'jobId' in value && 'state' in value;
    }

    private mapJob(job: ApiJobSnapshot, existingId?: number): JobState {
        const jobId = existingId ?? this.nextJobId++;
        this.jobs.set(jobId, job.jobId);
        const progress = job.progress as
            { phase?: string; completed?: number; total?: number | null; message?: string } | undefined;
        const error = job.error as { message?: string } | undefined;
        return {
            jobId,
            kind: job.operationId,
            status: job.state.toLocaleLowerCase() as JobState['status'],
            progress: progress
                ? {
                      phase: 0,
                      completed: progress.completed ?? 0,
                      total: progress.total ?? undefined,
                      label: progress.message ?? progress.phase ?? job.state,
                  }
                : undefined,
            result: job.result,
            error: error?.message,
        };
    }

    private mapJobEvent(event: ApiJobEvent, jobId: number): JobState {
        this.jobs.set(jobId, event.jobId);
        const progress = event.progress;
        return {
            jobId,
            kind: event.operationId,
            status: event.state.toLocaleLowerCase() as JobState['status'],
            progress: progress
                ? {
                      phase: 0,
                      completed: progress.completed,
                      total: progress.total ?? undefined,
                      label: progress.message || progress.phase || event.state,
                  }
                : undefined,
        };
    }

    private terminal(job: JobState): boolean {
        return job.status === 'completed' || job.status === 'failed' || job.status === 'cancelled';
    }

    private terminalState(state: string): boolean {
        return state === 'COMPLETED' || state === 'FAILED' || state === 'CANCELLED';
    }

    private createPlanKey(
        manifest: InputFileLocation,
        output: ServerFileLocation,
        overwrite: boolean,
        inputBindings: InputBinding[],
    ): string {
        return JSON.stringify([
            inputLocationKey(manifest),
            locationKey(output),
            overwrite,
            inputBindings.map((binding) => [binding.logicalPath, inputLocationKey(binding.source)]),
        ]);
    }

    private serverFile(location: FileLocation): ServerFileLocation {
        if (location.kind !== 'server-file') {
            throw new Error('HTTP transport requires a server sandbox file selection');
        }
        return location;
    }

    private serverInput(location: InputFileLocation): Record<string, unknown> {
        if (location.kind === 'client-upload') {
            return { uploadRef: location.reference };
        }
        return { fileRef: this.serverFile(location).reference };
    }

    private serverInputBindings(inputBindings: InputBinding[]): Record<string, unknown>[] {
        return inputBindings.map((binding) => ({
            logicalPath: binding.logicalPath,
            source: this.serverInput(binding.source),
        }));
    }

    private serverDirectory(location: DirectoryLocation): ServerDirectoryLocation {
        if (location.kind !== 'server-directory') {
            throw new Error('HTTP transport requires a server sandbox directory selection');
        }
        return location;
    }

    private volumeMutationOperation(mutation: VolumeMutation): Record<string, unknown> {
        const common = {
            id: `volume-${mutation.kind}`,
            partition_index: mutation.partitionIndex,
        };
        if (mutation.kind === 'add') {
            return {
                ...common,
                type: 'insert_volume',
                volume: {
                    name: mutation.volumeName,
                    waveforms: [],
                    sample_banks: [],
                },
            };
        }
        if (mutation.kind === 'rename') {
            return {
                ...common,
                type: 'rename_volume',
                volume_name: mutation.volumeName,
                new_volume_name: mutation.newVolumeName,
            };
        }
        return {
            ...common,
            type: 'delete_volume',
            volume_name: mutation.volumeName,
        };
    }
}
