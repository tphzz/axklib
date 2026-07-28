import {
    AxklibApiError,
    AxklibHttpApiClient,
    type ApiJobEvent,
    type ApiJobSnapshot,
    type AxklibApiConnection,
    type DownloadArchiveSnapshot,
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
    AuditionBundleDescriptor,
    ClientDownload,
    CompanionDirectorySelection,
    HardDiskCreationProfile,
    HardDiskCreationProfileId,
    ImageTransport,
    ImageSessionPackageExportDestination,
    ImageSessionExportRoot,
    ImageSessionAudioExportDestination,
    ImageSessionAudioExportInspection,
    ImageSessionPackageImportPlan,
    ImageSessionPackageRename,
    InputBinding,
    JobState,
    ObjectPage,
    ObjectPageFilter,
    ObjectDeletionInspection,
    WaveDataOrphanInspection,
    ObjectRenameMutation,
    OpenedImage,
    PackageImportDestination,
    PackageImportPlan,
    PackageInspection,
    PartitionMutation,
    PlanSummary,
    PreviewEnvelope,
    RelationshipPage,
    RelationshipPageFilter,
    RelationshipQuality,
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
    type ImageLocation,
    type InputFileLocation,
    type SandboxRoot,
    type ServerDirectoryLocation,
    type ServerFileLocation,
    type UploadKind,
} from './storageLocations';
import type { ClientUploadSource } from './clientUploadSource';

const ALTERATION_MANIFEST_SCHEMA_VERSION = '1.0';

type HttpImageTransportConnection = AxklibApiConnection;
type WireInputBinding = components['schemas']['InputBinding'];
type WireInputRef = components['schemas']['InputRef'];

interface ApiImageSummary {
    imageId: string;
    revision: number;
    source: components['schemas']['ImageSourceRef'];
    companionDirectories?: DirectoryRef[];
    format: string;
    rootCount: number;
    objectCount: number;
    relationshipCount: number;
    availableOperations?: string[];
    validation: { valid: boolean; infoCount: number; warningCount: number; errorCount: number };
}

type ApiContentItem = components['schemas']['ImageContentItem'];
type ApiObjectItem = components['schemas']['ImageObjectItem'];
type ApiRelationshipItem = components['schemas']['ImageRelationshipItem'];
type ApiObjectDeletionInspection = components['schemas']['ImageObjectDeletionInspection'];
type ApiWaveDataOrphanInspection = components['schemas']['ImageWaveDataOrphanInspection'];

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
    revision: number;
    source: ImageLocation;
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
        scopeRole: item.scopeRole,
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
        quality: relationshipQuality(item.quality),
        basis: item.basis,
        notes: item.notes,
        assignmentIndex: item.assignmentIndex ?? undefined,
        assignmentName: item.assignmentName,
        assignmentState: item.assignmentState,
        receiveChannelDisplay: item.receiveChannelDisplay,
    };
}

function relationshipQuality(value: string): RelationshipQuality {
    switch (value) {
        case 'KNOWN':
        case 'LIKELY':
        case 'TENTATIVE':
        case 'UNKNOWN':
            return value;
        default:
            throw new Error(`Unsupported relationship quality: ${value}`);
    }
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
    private readonly activeJobs = new Map<number, string>();
    private readonly terminalJobs = new Map<number, string>();
    private readonly createPlans = new Map<string, ApiWritePlan>();
    private static readonly maximumRetainedTerminalJobs = 128;
    private static readonly cancellationConcurrency = 4;
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

    async startAudioImport(sessionId: number, target: AudioImportTarget, items: AudioImportItem[]): Promise<JobState> {
        const session = this.session(sessionId);
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
                sample: {
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
            'images.alter',
            {
                imageId: session.remoteId,
                expectedRevision: session.revision,
                manifest: {
                    inline: {
                        schema_version: ALTERATION_MANIFEST_SCHEMA_VERSION,
                        operations,
                    },
                },
                inputBindings,
            },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.isJob(job)) throw new Error('images.alter did not return a job');
        return this.mapJob(job);
    }

    async downloadFile(location: FileLocation): Promise<ClientDownload> {
        const source = this.serverFile(location);
        const metadata = await this.client.inspectDownload(source.reference);
        const limits = await this.client.serverLimits();
        const rangeLimit = limits.maximumDownloadRangeBytes;
        if (!Number.isSafeInteger(rangeLimit) || rangeLimit <= 0) {
            throw new Error('axklib-server advertised an invalid download range limit');
        }
        const parts: ArrayBuffer[] = [];
        for (let start = 0; start < metadata.size; start += rangeLimit) {
            const end = Math.min(metadata.size, start + rangeLimit) - 1;
            const response = await this.client.openDownload(source.reference, { start, end }, metadata.revision);
            const part = await response.arrayBuffer();
            const expectedSize = end - start + 1;
            if (part.byteLength !== expectedSize) {
                throw new Error(
                    `Download range ${start}-${end} returned ${part.byteLength} bytes; expected ${expectedSize}`,
                );
            }
            parts.push(part);
        }
        const filename = source.reference.relativePath.split('/').pop() || source.displayName;
        return { filename, blob: new Blob(parts, { type: 'application/octet-stream' }) };
    }

    async downloadDirectory(location: DirectoryLocation): Promise<ClientDownload> {
        const source = this.serverDirectory(location);
        const submitted = await this.client.invoke<never>(
            'files.archive',
            { directory: source.reference },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.isJob(submitted)) throw new Error('files.archive did not return a job');
        const localJob = this.mapJob(submitted);
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

    async planImagePackageImport(
        sessionId: number,
        source: InputFileLocation,
        partitionIndex: number,
        volumeName: string,
        renames: ImageSessionPackageRename[] = [],
        replacePlanToken?: string,
    ): Promise<ImageSessionPackageImportPlan> {
        const session = this.session(sessionId);
        const result = await this.client.invoke<ImageSessionPackageImportPlan>('images.package_import.plan', {
            imageId: session.remoteId,
            expectedRevision: session.revision,
            package: this.serverInput(source),
            partitionIndex,
            volumeName,
            renames,
            ...(replacePlanToken ? { replacePlanToken } : {}),
        });
        if (this.isJob(result)) throw new Error('image package import planning unexpectedly returned a job');
        return result;
    }

    async releaseImagePackageImportPlan(planToken: string): Promise<void> {
        const result = await this.client.invoke<{ released: true }>('images.package_import.release', { planToken });
        if (this.isJob(result)) throw new Error('image package plan release unexpectedly returned a job');
    }

    async startImagePackageImport(planToken: string): Promise<JobState> {
        const job = await this.client.invoke<never>(
            'images.package_import',
            { planToken },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.isJob(job)) throw new Error('images.package_import did not return a job');
        return this.mapJob(job);
    }

    async startImagePackageExport(
        sessionId: number,
        roots: ImageSessionExportRoot[],
        destination: ImageSessionPackageExportDestination,
    ): Promise<JobState> {
        const session = this.session(sessionId);
        const job = await this.client.invoke<never>(
            'images.package_export',
            {
                imageId: session.remoteId,
                expectedRevision: session.revision,
                roots,
                destination,
            },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.isJob(job)) throw new Error('images.package_export did not return a job');
        return this.mapJob(job);
    }

    async inspectImageAudioExport(
        sessionId: number,
        roots: ImageSessionExportRoot[],
    ): Promise<ImageSessionAudioExportInspection> {
        const session = this.session(sessionId);
        const result = await this.client.invoke<ImageSessionAudioExportInspection>('images.audio_export.inspect', {
            imageId: session.remoteId,
            expectedRevision: session.revision,
            roots,
        });
        if (this.isJob(result)) throw new Error('images.audio_export.inspect unexpectedly returned a job');
        return result;
    }

    async startImageAudioExport(
        sessionId: number,
        roots: ImageSessionExportRoot[],
        format: 'SFZ' | 'WAV',
        destination: ImageSessionAudioExportDestination,
    ): Promise<JobState> {
        const session = this.session(sessionId);
        const job = await this.client.invoke<never>(
            'images.audio_export',
            {
                imageId: session.remoteId,
                expectedRevision: session.revision,
                roots,
                format,
                destination,
            },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.isJob(job)) throw new Error('images.audio_export did not return a job');
        return this.mapJob(job);
    }

    deleteRetainedPackage(download: components['schemas']['RetainedDownload']): Promise<void> {
        return this.client.deleteRetainedDownload(download.contentPath);
    }

    async openImage(location: ImageLocation): Promise<OpenedImage> {
        if (location.kind !== 'server-file' && location.kind !== 'axk-object-directory') {
            throw new Error('Opening images requires a server sandbox file selection or AXK object directory');
        }
        const wireSource: components['schemas']['ImageSourceRef'] =
            location.kind === 'server-file'
                ? { kind: 'FILE', file: location.reference }
                : { kind: 'AXK_OBJECT_DIRECTORY', directory: location.reference };
        const summary = await this.client.request<ApiImageSummary>('POST', '/images', {
            source: wireSource,
        });
        const sessionId = this.nextSessionId++;
        this.sessions.set(sessionId, {
            remoteId: summary.imageId,
            revision: summary.revision,
            source: location,
            contentCursors: new Map(),
            contentItems: new Map(),
            objectCursors: new Map(),
            relationshipCursors: new Map(),
        });
        try {
            return await this.openedImage(sessionId, summary);
        } catch (error) {
            await this.closeImage(sessionId).catch(() => undefined);
            throw error;
        }
    }

    async refreshImage(sessionId: number): Promise<OpenedImage> {
        const session = this.session(sessionId);
        const summary = await this.client.request<ApiImageSummary>(
            'GET',
            `/images/${encodeURIComponent(session.remoteId)}`,
        );
        session.revision = summary.revision;
        session.contentCursors.clear();
        session.contentItems.clear();
        session.objectCursors.clear();
        session.relationshipCursors.clear();
        return this.openedImage(sessionId, summary);
    }

    async attachCompanionDirectories(sessionId: number, selection: CompanionDirectorySelection): Promise<OpenedImage> {
        const session = this.session(sessionId);
        const wireSelection: components['schemas']['CompanionDirectorySelection'] =
            selection.kind === 'directories'
                ? { kind: 'DIRECTORIES', directories: selection.directories }
                : { kind: 'IMMEDIATE_SIBLINGS' };
        const summary = await this.client.request<ApiImageSummary>(
            'POST',
            `/images/${encodeURIComponent(session.remoteId)}/companion-directories`,
            {
                expectedRevision: session.revision,
                selection: wireSelection,
            },
        );
        session.revision = summary.revision;
        session.contentCursors.clear();
        session.contentItems.clear();
        session.objectCursors.clear();
        session.relationshipCursors.clear();
        return this.openedImage(sessionId, summary);
    }

    private async openedImage(sessionId: number, summary: ApiImageSummary): Promise<OpenedImage> {
        const session = this.session(sessionId);
        const roots = await this.allContentChildren(sessionId, '');
        const disk: DiskTreeItem = {
            id: `session:${sessionId}`,
            name: session.source.displayName,
            kind: 'disk',
            children: roots.items,
            childCount: roots.totalCount,
        };
        const initialVolume = await this.loadVolumeHierarchy(sessionId, roots.items);
        return {
            sessionId,
            companionDirectories: summary.companionDirectories ?? [],
            validation: validationSummary(summary),
            objects: [],
            objectTotalCount: 0,
            initialVolume,
            volumeMutationsAvailable: (summary.availableOperations ?? []).includes('images.alter.volumes'),
            partitionMutationsAvailable: (summary.availableOperations ?? []).includes('images.alter.partitions'),
            objectRenameAvailable: (summary.availableOperations ?? []).includes('images.alter.objects'),
            objectDeletionAvailable: (summary.availableOperations ?? []).includes('images.alter.objects'),
            waveDataCleanupAvailable: (summary.availableOperations ?? []).includes('images.deletion.orphans.inspect'),
            packageImportAvailable: (summary.availableOperations ?? []).includes('images.package.import'),
            packageExportAvailable: (summary.availableOperations ?? []).includes('images.package.export'),
            audioExportAvailable: (summary.availableOperations ?? []).includes('images.audio_export'),
            tree: [disk],
        };
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

    async startVolumeMutation(sessionId: number, mutation: VolumeMutation): Promise<JobState> {
        return this.startImageMutation(sessionId, this.volumeMutationOperation(mutation));
    }

    async startPartitionMutation(sessionId: number, mutation: PartitionMutation): Promise<JobState> {
        return this.startImageMutation(sessionId, {
            id: 'partition-rename',
            type: 'rename_partition',
            partition_index: mutation.partitionIndex,
            partition_name: mutation.partitionName,
            new_partition_name: mutation.newPartitionName,
        });
    }

    async startObjectRename(sessionId: number, mutation: ObjectRenameMutation): Promise<JobState> {
        return this.startImageMutation(sessionId, this.objectRenameOperation(mutation));
    }

    async inspectObjectDeletion(
        sessionId: number,
        targetObjectIds: string[],
        cleanupObjectIds: string[],
    ): Promise<ObjectDeletionInspection> {
        const session = this.session(sessionId);
        const result = await this.client.invoke<ApiObjectDeletionInspection>('images.deletion.inspect', {
            imageId: session.remoteId,
            expectedRevision: session.revision,
            targetObjectIds,
            cleanupObjectIds,
        });
        if (this.isJob(result)) throw new Error('images.deletion.inspect unexpectedly returned a job');
        return result;
    }

    async inspectWaveDataOrphans(sessionId: number, contentScopeId: string): Promise<WaveDataOrphanInspection> {
        const session = this.session(sessionId);
        const result = await this.client.invoke<ApiWaveDataOrphanInspection>('images.deletion.orphans.inspect', {
            imageId: session.remoteId,
            expectedRevision: session.revision,
            contentScopeId,
        });
        if (this.isJob(result)) throw new Error('images.deletion.orphans.inspect unexpectedly returned a job');
        return result;
    }

    async startObjectDeletion(
        sessionId: number,
        targetObjectIds: string[],
        cleanupObjectIds: string[],
    ): Promise<JobState> {
        const session = this.session(sessionId);
        const result = await this.client.invoke<never>(
            'images.delete',
            {
                imageId: session.remoteId,
                expectedRevision: session.revision,
                targetObjectIds,
                cleanupObjectIds,
            },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.isJob(result)) throw new Error('images.delete did not return a job');
        return this.mapJob(result);
    }

    private async startImageMutation(sessionId: number, operation: Record<string, unknown>): Promise<JobState> {
        const session = this.session(sessionId);
        const job = await this.client.invoke<never>(
            'images.alter',
            {
                imageId: session.remoteId,
                expectedRevision: session.revision,
                manifest: {
                    inline: {
                        schema_version: ALTERATION_MANIFEST_SCHEMA_VERSION,
                        operations: [operation],
                    },
                },
                inputBindings: [],
            },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.isJob(job)) throw new Error('images.alter did not return a job');
        return this.mapJob(job);
    }

    preview(sessionId: number, objectKey: string, binCount: number): Promise<PreviewEnvelope> {
        const session = this.session(sessionId);
        const query = new URLSearchParams({ objectId: objectKey, bins: String(binCount) });
        return this.client.request('GET', `/images/${encodeURIComponent(session.remoteId)}/preview?${query}`);
    }

    async prepareAuditionBundle(
        sessionId: number,
        objectKeys: readonly string[],
        signal?: AbortSignal,
    ): Promise<AuditionBundleDescriptor> {
        const session = this.session(sessionId);
        const submitted = await this.client.invoke<never>('auditions.prepare', {
            imageId: session.remoteId,
            objectIds: objectKeys,
        });
        if (!this.isJob(submitted)) throw new Error('auditions.prepare did not return a job');
        const localJob = this.mapJob(submitted);
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
        const remoteId = this.remoteJobId(jobId);
        if (!remoteId) throw new Error('Job is closed or unknown');
        const job = await this.client.request<ApiJobSnapshot>('GET', `/jobs/${encodeURIComponent(remoteId)}`);
        return this.mapJob(job, jobId);
    }

    waitForJob(jobId: number, onUpdate: (job: JobState) => void, signal?: AbortSignal): Promise<JobState> {
        const remoteId = this.remoteJobId(jobId);
        if (!remoteId) return Promise.reject(new Error('Job is closed or unknown'));

        return new Promise((resolve, reject) => {
            let afterSequence = 0;
            let connection: EventConnection | undefined;
            let reconnectAttempts = 0;
            let stableConnectionTimer: ReturnType<typeof setTimeout> | undefined;
            let settled = false;
            let cancellationRequested = false;
            let work = Promise.resolve();
            const handleAbort = (): void => {
                if (settled || cancellationRequested) return;
                cancellationRequested = true;
                enqueue(async () => {
                    try {
                        await this.cancelJob(jobId);
                    } catch (reason) {
                        await reconcile();
                        if (!settled) throw reason;
                        return;
                    }
                    await reconcile();
                });
            };

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
                signal?.removeEventListener('abort', handleAbort);
                connection?.close();
                resolve(job);
            };

            const fail = (reason: unknown): void => {
                if (settled) return;
                settled = true;
                clearStableConnectionTimer();
                signal?.removeEventListener('abort', handleAbort);
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
            signal?.addEventListener('abort', handleAbort, { once: true });
            if (signal?.aborted) handleAbort();
        });
    }

    async cancelJob(jobId?: number): Promise<void> {
        if (jobId !== undefined) {
            const remoteId = this.activeJobs.get(jobId);
            if (remoteId) await this.client.request('DELETE', `/jobs/${encodeURIComponent(remoteId)}`);
            return;
        }
        const remoteIds = [...new Set(this.activeJobs.values())];
        for (let offset = 0; offset < remoteIds.length; offset += HttpImageTransport.cancellationConcurrency) {
            const batch = remoteIds.slice(offset, offset + HttpImageTransport.cancellationConcurrency);
            await Promise.all(
                batch.map((remoteId) => this.client.request('DELETE', `/jobs/${encodeURIComponent(remoteId)}`)),
            );
        }
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
        const progress = job.progress as
            { phase?: string; completed?: number; total?: number | null; message?: string } | undefined;
        const error = job.error as { code?: string; message?: string; context?: unknown } | undefined;
        const mapped: JobState = {
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
            errorCode: error?.code,
            errorContext: error?.context,
        };
        this.trackJob(jobId, job.jobId, this.terminal(mapped));
        return mapped;
    }

    private mapJobEvent(event: ApiJobEvent, jobId: number): JobState {
        const progress = event.progress;
        const mapped: JobState = {
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
        this.trackJob(jobId, event.jobId, this.terminal(mapped));
        return mapped;
    }

    private remoteJobId(jobId: number): string | undefined {
        return this.activeJobs.get(jobId) ?? this.terminalJobs.get(jobId);
    }

    private trackJob(jobId: number, remoteId: string, terminal: boolean): void {
        if (!terminal) {
            this.terminalJobs.delete(jobId);
            this.activeJobs.set(jobId, remoteId);
            return;
        }
        this.activeJobs.delete(jobId);
        this.terminalJobs.delete(jobId);
        this.terminalJobs.set(jobId, remoteId);
        while (this.terminalJobs.size > HttpImageTransport.maximumRetainedTerminalJobs) {
            const oldest = this.terminalJobs.keys().next().value as number | undefined;
            if (oldest === undefined) break;
            this.terminalJobs.delete(oldest);
        }
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

    private serverInput(location: InputFileLocation): WireInputRef {
        if (location.kind === 'client-upload') {
            return { uploadRef: location.reference };
        }
        return { fileRef: this.serverFile(location).reference };
    }

    private serverInputBindings(inputBindings: InputBinding[]): WireInputBinding[] {
        return inputBindings.map((binding) => ({
            manifestPath: binding.logicalPath,
            input: this.serverInput(binding.source),
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
                    samples: [],
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

    private objectRenameOperation(mutation: ObjectRenameMutation): Record<string, unknown> {
        const common = {
            id: `rename-${mutation.kind}`,
            partition_index: mutation.partitionIndex,
            volume_name: mutation.volumeName,
        };
        if (mutation.kind === 'program') {
            return {
                ...common,
                type: 'rename_program',
                program_number: mutation.programNumber,
                new_program_name: mutation.newProgramName,
            };
        }
        if (mutation.kind === 'sample-bank') {
            return {
                ...common,
                type: 'rename_sbac',
                sample_bank_name: mutation.sampleBankName,
                new_sample_bank_name: mutation.newSampleBankName,
            };
        }
        if (mutation.kind === 'sample') {
            return {
                ...common,
                type: 'rename_sbnk',
                sample_name: mutation.sampleName,
                new_sample_name: mutation.newSampleName,
            };
        }
        return {
            ...common,
            type: 'rename_waveform',
            waveform_name: mutation.waveformName,
            new_waveform_name: mutation.newWaveformName,
        };
    }
}
