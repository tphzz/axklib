import type { AxklibHttpApiClient } from './httpApiClient';
import type { components } from './generated/axklibApiV1';
import { imageSourceReference, imageLocation } from './httpImageSources';
import { AxklibApiError } from './httpErrors';
import { HttpJobController } from './httpJobController';
import { HttpCapacityGate } from './httpCapacityGate';
import {
    type ApiContentItem,
    type ApiImageSummary,
    type ApiObjectItem,
    type ApiObjectDeletionInspection,
    type ApiPage,
    type ApiRelationshipItem,
    type ApiWaveDataOrphanInspection,
    type SessionState,
    mapContentItem,
    mapObject,
    mapRelationship,
    validationSummary,
} from './httpTransportModels';
import { aSeriesHierarchy, documentCapabilities } from '../features/devices/a-series/openedDocument';
import {
    documentPath,
    documentSession,
    documentTarget,
    selectedDocument,
    refreshedDocumentSession,
} from './httpNativeDocuments';
import { HttpNativeDocumentReads } from './httpNativeDocumentReads';
import type { ImageLocation, InputFileLocation } from './storageLocations';
import type {
    CompanionSelection,
    AllocationMapReference,
    ContentPage,
    ObjectPage,
    ObjectPageFilter,
    ObjectDetail,
    OpenedImage,
    RelationshipPage,
    RelationshipPageFilter,
    SystemProgramContexts,
    ImageValidationIssue,
    ImageOpenOptions,
    ImageSessionExtentLayoutRepairDestination,
    JobState,
    ObjectDeletionInspection,
    PlacementRepairInspection,
    PlacementRepairScope,
    ProgramGenerationInspection,
    ProgramGenerationSelection,
    ProgramAssignmentCleanupInspection,
    ProgramAssignmentCleanupSelection,
    VolumeDeletionInspection,
    VolumeCapacityInspection,
    WaveDataOrphanInspection,
} from './transport';
import { randomIdempotencyKey, serverInput } from './httpTransportWire';
import { inspectVolumeCapacity, inspectVolumeDeletion } from './httpVolumeOperations';
import { startSu700Import } from './httpSu700Import';
import { inspectFilesystemInputs, inspectFilesystemImage, filesystemEditWire } from './httpFilesystemInputs';
import type { Su700Request } from './su700Import';
import type { FilesystemEdit, FilesystemPage, FilesystemQuery, FilesystemImportEntry } from './filesystem';
import type {
    FilesystemExportDestination,
    FilesystemExportInspection,
    FilesystemExportLayout,
} from './filesystemExport';

const ALTERATION_MANIFEST_SCHEMA_VERSION = '1.0';

export class HttpImageSessions {
    startSu700Import(request: Su700Request): Promise<JobState> {
        return startSu700Import(this.client, this.jobs, (id) => this.get(id).remoteId, request);
    }
    async startFilesystemImportInspection(
        sessionId: number,
        expectedRevision: number,
        parentEntryId: string,
        entries: FilesystemImportEntry[],
    ): Promise<JobState> {
        if (!entries.length || entries.length > 10000) throw new Error('Choose between 1 and 10000 import entries');
        const job = await this.client.invoke<never>('images.filesystem.import.inspect', {
            imageId: this.get(sessionId).remoteId,
            expectedRevision,
            parentEntryId,
            entries,
        });
        if (!this.jobs.isJob(job)) throw new Error('images.filesystem.import.inspect did not return a job');
        return this.jobs.map(job);
    }
    async startFilesystemInputInspection(inputs: InputFileLocation[]): Promise<JobState> {
        return inspectFilesystemInputs(this.client, this.jobs, inputs);
    }
    async startFilesystemImageInspection(source: InputFileLocation): Promise<JobState> {
        return inspectFilesystemImage(this.client, this.jobs, source);
    }
    async releaseFilesystemImageInspection(inspectionToken: string): Promise<void> {
        await this.client.invoke('filesystem.images.release', { inspectionToken });
    }
    async inspectFilesystemExport(
        sessionId: number,
        expectedRevision: number,
        entryIds: string[],
        layout: FilesystemExportLayout = 'SELECTED_ENTRIES',
    ): Promise<FilesystemExportInspection> {
        if (!entryIds.length) throw new Error('At least one filesystem entry is required');
        const session = this.get(sessionId);
        const result = await this.client.invoke<FilesystemExportInspection>('images.filesystem.export.inspect', {
            imageId: session.remoteId,
            expectedRevision,
            entryIds,
            layout,
        });
        if (this.jobs.isJob(result)) throw new Error('images.filesystem.export.inspect unexpectedly returned a job');
        return result;
    }

    async startFilesystemExport(
        sessionId: number,
        expectedRevision: number,
        entryIds: string[],
        destination: FilesystemExportDestination,
        layout: FilesystemExportLayout = 'SELECTED_ENTRIES',
    ): Promise<JobState> {
        if (!entryIds.length) throw new Error('At least one filesystem entry is required');
        const session = this.get(sessionId);
        const job = await this.client.invoke<never>(
            'images.filesystem.export',
            {
                imageId: session.remoteId,
                expectedRevision,
                entryIds,
                destination,
                layout,
            },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.jobs.isJob(job)) throw new Error('images.filesystem.export did not return a job');
        return this.jobs.map(job);
    }

    async filesystem(sessionId: number, filter: FilesystemQuery = {}): Promise<FilesystemPage> {
        const session = this.get(sessionId);
        const query = new URLSearchParams({ expectedRevision: String(session.revision), limit: '200' });
        if (filter.objectId || filter.contentScopeId) query.set('documentId', documentTarget(session).documentId);
        for (const [key, value] of Object.entries(filter)) {
            if (value !== undefined) query.set(key, String(value));
        }
        const page = await this.client.request<components['schemas']['ImageFilesystemPage']>(
            'GET',
            `/images/${encodeURIComponent(session.remoteId)}/filesystem?${query}`,
        );
        this.assertCurrent(sessionId, session);
        const kinds = { ROOT: 'root', PARTITION: 'partition', DIRECTORY: 'directory', FILE: 'file' } as const;
        return { ...page, items: page.items.map((entry) => ({ ...entry, kind: kinds[entry.kind] })) };
    }
    async startFilesystemEdits(
        sessionId: number,
        expectedRevision: number,
        edits: FilesystemEdit[],
        policy?: import('./importCapacity').CapacityPolicy,
    ): Promise<JobState> {
        if (!edits.length) throw new Error('At least one filesystem edit is required');
        const session = this.get(sessionId);
        return this.mutations.start(
            {
                imageId: session.remoteId,
                // Entry identities belong to the reviewed revision, not the latest heartbeat.
                expectedRevision,
                acknowledgeDeviceRelationships: true,
                edits: edits.map(filesystemEditWire),
                ...(policy ? { capacityPolicy: policy } : {}),
            },
            'images.filesystem.edit',
            'images.filesystem.edit.inspect',
        );
    }

    private readonly sessions = new Map<number, SessionState>();
    private nextSessionId = 1;
    readonly mutations: HttpCapacityGate;
    readonly native: HttpNativeDocumentReads;

    constructor(
        private readonly client: AxklibHttpApiClient,
        private readonly jobs: HttpJobController,
        mutations?: HttpCapacityGate,
    ) {
        this.mutations = mutations ?? new HttpCapacityGate(client, jobs);
        this.native = new HttpNativeDocumentReads(client, (id) => this.get(id), jobs);
    }

    async open(locations: ImageLocation[], options: ImageOpenOptions = {}): Promise<OpenedImage> {
        if (
            !locations.length ||
            locations.length > 32 ||
            locations.some((location) => !['server-file', 'server-directory', 'device'].includes(location.kind)) ||
            (locations.length > 1 && locations.some((location) => location.kind === 'device'))
        )
            throw new Error('Select one to 32 files or folders, or one physical device');
        const submitted = await this.client.request<components['schemas']['Job']>('POST', '/images', {
            sources: locations.map(imageSourceReference),
        });
        if (!this.jobs.isJob(submitted)) throw new Error('images.open did not return a job');
        const localJob = this.jobs.map(submitted);
        options.onUpdate?.(localJob);
        const completed =
            localJob.status === 'completed' || localJob.status === 'failed' || localJob.status === 'cancelled'
                ? localJob
                : await this.jobs.wait(localJob.jobId, (job) => options.onUpdate?.(job), options.signal);
        if (completed.status === 'cancelled') {
            throw new DOMException('Image opening was cancelled', 'AbortError');
        }
        if (completed.status !== 'completed' || !completed.result) {
            throw new AxklibApiError(
                completed.errorCode ?? 'image_open_failed',
                completed.error ?? 'Image opening did not complete',
                422,
                undefined,
                completed.errorContext,
            );
        }
        const summary = completed.result as ApiImageSummary;
        const sessionId = this.nextSessionId++;
        if (locations[0].kind === 'device') this.client.deviceWrites.bind(summary.imageId, locations[0]);
        this.sessions.set(sessionId, { ...documentSession(summary), sources: locations, source: locations[0] });
        try {
            return await this.openedImage(sessionId, summary);
        } catch (error) {
            await this.close(sessionId).catch(() => undefined);
            throw error;
        }
    }

    async refresh(sessionId: number): Promise<OpenedImage> {
        const session = this.get(sessionId);
        const summary = await this.client.request<ApiImageSummary>(
            'GET',
            `/images/${encodeURIComponent(session.remoteId)}`,
        );
        this.assertCurrent(sessionId, session);
        this.sessions.set(sessionId, refreshedDocumentSession(summary, session));
        return this.openedImage(sessionId, summary);
    }

    async keepAlive(sessionId: number): Promise<void> {
        const session = this.get(sessionId);
        const summary = await this.client.request<ApiImageSummary>(
            'GET',
            `/images/${encodeURIComponent(session.remoteId)}`,
        );
        this.assertCurrent(sessionId, session);
        if (summary.revision > session.revision)
            this.sessions.set(sessionId, refreshedDocumentSession(summary, session));
    }

    async selectDocument(
        sessionId: number,
        documentId: string | null | undefined,
        sourceMemberId?: string,
    ): Promise<OpenedImage> {
        const previous = this.get(sessionId);
        const member =
            sourceMemberId ??
            (!documentId ||
            previous.summary.documents
                .find((item) => item.id === documentId)
                ?.sourceMemberIds.includes(previous.activeSourceMemberId)
                ? previous.activeSourceMemberId
                : undefined);
        const next = documentSession(previous.summary, documentId, member);
        this.sessions.set(sessionId, next);
        try {
            return await this.openedImage(sessionId, previous.summary);
        } catch (error) {
            if (this.get(sessionId) === next) this.sessions.set(sessionId, previous);
            throw error;
        }
    }

    async resolveDocuments(sessionId: number, selectedFormats: string[]): Promise<OpenedImage> {
        const session = this.get(sessionId);
        const summary = await this.client.request<ApiImageSummary>(
            'POST',
            `/images/${encodeURIComponent(session.remoteId)}/documents`,
            {
                expectedRevision: session.revision,
                selectedFormats,
            },
        );
        this.assertCurrent(sessionId, session);
        this.sessions.set(sessionId, refreshedDocumentSession(summary, session));
        return this.openedImage(sessionId, summary);
    }

    async attachCompanions(sessionId: number, selection: CompanionSelection): Promise<OpenedImage> {
        const session = this.get(sessionId);
        const wireSelection: components['schemas']['CompanionSelection'] =
            selection.kind === 'sources'
                ? { kind: 'SOURCES', sources: selection.sources.map(imageSourceReference) }
                : { kind: 'IMMEDIATE_SIBLINGS' };
        const summary = await this.client.request<ApiImageSummary>('POST', `${documentPath(session)}/companions`, {
            expectedRevision: session.revision,
            selection: wireSelection,
        });
        this.assertCurrent(sessionId, session);
        this.sessions.set(sessionId, refreshedDocumentSession(summary, session));
        return this.openedImage(sessionId, summary);
    }

    async contentChildren(sessionId: number, parentId: string, offset: number, limit: number): Promise<ContentPage> {
        const session = this.get(sessionId);
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
            `${documentPath(session)}/content?${query}`,
        );
        this.assertCurrent(sessionId, session);
        if (page.nextCursor) cursors.set(offset + page.items.length, page.nextCursor);
        const parent = session.contentItems.get(parentId);
        const items = page.items.map((item) => mapContentItem(item, parent));
        for (const item of items) session.contentItems.set(item.id, item);
        return { items, totalCount: page.totalCount };
    }

    async validationIssues(sessionId: number): Promise<ImageValidationIssue[]> {
        const session = this.get(sessionId);
        const issues: ImageValidationIssue[] = [];
        let cursor: string | null = null;
        for (let request = 0; request < 256; request += 1) {
            const query = new URLSearchParams({ limit: '200' });
            if (cursor) query.set('cursor', cursor);
            const page = await this.client.request<ApiPage<ImageValidationIssue>>(
                'GET',
                `${documentPath(session)}/validation/issues?${query}`,
            );
            this.assertCurrent(sessionId, session);
            issues.push(...page.items);
            cursor = page.nextCursor;
            if (!cursor) return issues;
        }
        throw new Error('Image validation issue pagination exceeded its safety limit');
    }

    async allocationMapReference(sessionId: number): Promise<AllocationMapReference> {
        const session = this.get(sessionId);
        const document = selectedDocument(session);
        if (!document || document.sourceMemberIds.length !== 1) throw new Error('Select one allocation source');
        return { imageId: session.remoteId, sourceMemberId: document.sourceMemberIds[0], revision: session.revision };
    }

    async objectPage(
        sessionId: number,
        offset: number,
        limit: number,
        filter: ObjectPageFilter = {},
    ): Promise<ObjectPage> {
        const session = this.get(sessionId);
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
            `${documentPath(session)}/objects?${query}`,
        );
        this.assertCurrent(sessionId, session);
        if (page.nextCursor) cursors.set(offset + page.items.length, page.nextCursor);
        return { objects: page.items.map(mapObject), totalCount: page.totalCount };
    }

    async objectDetail(sessionId: number, objectId: string): Promise<ObjectDetail> {
        const detail = await this.native.detail(sessionId, objectId);
        if (detail.formatId !== 'yamaha.a-series') throw new Error('The A-series editor cannot open this document');
        return detail;
    }

    async relationshipPage(
        sessionId: number,
        offset: number,
        limit: number,
        filter: RelationshipPageFilter = {},
    ): Promise<RelationshipPage> {
        const session = this.get(sessionId);
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
            `${documentPath(session)}/relationships?${query}`,
        );
        this.assertCurrent(sessionId, session);
        if (page.nextCursor) cursors.set(offset + page.items.length, page.nextCursor);
        return { relationships: page.items.map(mapRelationship), totalCount: page.totalCount };
    }

    async systemProgramContexts(sessionId: number, partitionIndex: number): Promise<SystemProgramContexts> {
        const session = this.get(sessionId);
        const query = new URLSearchParams({ partitionIndex: String(partitionIndex) });
        const result = await this.client.request<SystemProgramContexts>(
            'GET',
            `${documentPath(session)}/system-program-contexts?${query}`,
        );
        this.assertCurrent(sessionId, session);
        return result;
    }

    async close(sessionId: number): Promise<void> {
        const session = this.sessions.get(sessionId);
        if (!session) return;
        await this.client.request('DELETE', `/images/${encodeURIComponent(session.remoteId)}`);
        this.client.deviceWrites.unbind(session.remoteId);
        this.sessions.delete(sessionId);
        const source = session.source;
        if (
            source.kind === 'device' &&
            ![...this.sessions.values()].some(
                (other) =>
                    other.source.kind === 'device' && other.source.reference.sessionId === source.reference.sessionId,
            )
        )
            await this.client.invoke('devices.close', { sessionId: source.reference.sessionId });
    }

    async inspectVolumeDeletion(
        sessionId: number,
        targets: components['schemas']['ImageVolumeDeletionTarget'][],
    ): Promise<VolumeDeletionInspection> {
        return inspectVolumeDeletion(this.client, this.jobs, this.get(sessionId), targets);
    }

    inspectVolumeCapacity(sessionId: number, contentScopeId: string): Promise<VolumeCapacityInspection> {
        return inspectVolumeCapacity(this.client, this.jobs, this.get(sessionId), contentScopeId);
    }

    async inspectPlacement(
        sessionId: number,
        scope: PlacementRepairScope,
        recoveryVolumeName?: string,
    ): Promise<PlacementRepairInspection> {
        const session = this.get(sessionId);
        const result = await this.client.invoke<PlacementRepairInspection>('images.placement.inspect', {
            ...documentTarget(session),
            expectedRevision: session.revision,
            scope,
            ...(recoveryVolumeName ? { recoveryVolumeName } : {}),
        });
        if (this.jobs.isJob(result)) throw new Error('images.placement.inspect unexpectedly returned a job');
        return result;
    }

    async startPlacementRepair(
        sessionId: number,
        scope: PlacementRepairScope,
        recoveryVolumeName?: string,
    ): Promise<JobState> {
        const session = this.get(sessionId);
        const result = await this.client.invoke<Record<string, unknown>>('images.placement.repair.prepare', {
            ...documentTarget(session),
            expectedRevision: session.revision,
            scope,
            ...(recoveryVolumeName ? { recoveryVolumeName } : {}),
        });
        if (this.jobs.isJob(result)) throw new Error('Placement preparation unexpectedly returned a job');
        return this.mutations.start(result);
    }

    async inspectObjectDeletion(
        sessionId: number,
        targetObjectIds: string[],
        referrerObjectIds: string[],
        cleanupObjectIds: string[],
    ): Promise<ObjectDeletionInspection> {
        const session = this.get(sessionId);
        const result = await this.client.invoke<ApiObjectDeletionInspection>('images.deletion.inspect', {
            ...documentTarget(session),
            expectedRevision: session.revision,
            targetObjectIds,
            referrerObjectIds,
            cleanupObjectIds,
        });
        if (this.jobs.isJob(result)) throw new Error('images.deletion.inspect unexpectedly returned a job');
        return result;
    }

    async inspectWaveDataOrphans(sessionId: number, contentScopeId: string): Promise<WaveDataOrphanInspection> {
        const session = this.get(sessionId);
        const result = await this.client.invoke<ApiWaveDataOrphanInspection>('images.deletion.orphans.inspect', {
            ...documentTarget(session),
            expectedRevision: session.revision,
            contentScopeId,
        });
        if (this.jobs.isJob(result)) throw new Error('images.deletion.orphans.inspect unexpectedly returned a job');
        return result;
    }

    async startObjectDeletion(
        sessionId: number,
        targetObjectIds: string[],
        referrerObjectIds: string[],
        cleanupObjectIds: string[],
    ): Promise<JobState> {
        const session = this.get(sessionId);
        const result = await this.client.invoke<never>(
            'images.delete',
            {
                ...documentTarget(session),
                expectedRevision: session.revision,
                targetObjectIds,
                referrerObjectIds,
                cleanupObjectIds,
            },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.jobs.isJob(result)) throw new Error('images.delete did not return a job');
        return this.jobs.map(result);
    }

    async inspectProgramGeneration(sessionId: number, contentScopeId: string): Promise<ProgramGenerationInspection> {
        const session = this.get(sessionId);
        const result = await this.client.invoke<ProgramGenerationInspection>('images.programs.generate.inspect', {
            ...documentTarget(session),
            expectedRevision: session.revision,
            contentScopeId,
        });
        if (this.jobs.isJob(result)) {
            throw new Error('images.programs.generate.inspect unexpectedly returned a job');
        }
        return result;
    }

    async startProgramGeneration(
        sessionId: number,
        contentScopeId: string,
        programs: ProgramGenerationSelection[],
    ): Promise<JobState> {
        const session = this.get(sessionId);
        const result = await this.client.invoke<Record<string, unknown>>('images.programs.generate.prepare', {
            ...documentTarget(session),
            expectedRevision: session.revision,
            contentScopeId,
            programs,
        });
        if (this.jobs.isJob(result)) throw new Error('Program preparation unexpectedly returned a job');
        return this.mutations.start(result);
    }

    async inspectProgramAssignmentCleanup(
        sessionId: number,
        contentScopeId: string,
    ): Promise<ProgramAssignmentCleanupInspection> {
        const session = this.get(sessionId);
        const result = await this.client.invoke<ProgramAssignmentCleanupInspection>(
            'images.program_assignments.cleanup.inspect',
            { ...documentTarget(session), expectedRevision: session.revision, contentScopeId },
        );
        if (this.jobs.isJob(result)) {
            throw new Error('images.program_assignments.cleanup.inspect unexpectedly returned a job');
        }
        return result;
    }

    async startProgramAssignmentCleanup(
        sessionId: number,
        contentScopeId: string,
        assignments: ProgramAssignmentCleanupSelection[],
    ): Promise<JobState> {
        const session = this.get(sessionId);
        const result = await this.client.invoke<never>(
            'images.program_assignments.cleanup',
            { ...documentTarget(session), expectedRevision: session.revision, contentScopeId, assignments },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.jobs.isJob(result)) {
            throw new Error('images.program_assignments.cleanup did not return a job');
        }
        return this.jobs.map(result);
    }

    async startExtentLayoutRepair(
        sessionId: number,
        destination: ImageSessionExtentLayoutRepairDestination,
    ): Promise<JobState> {
        const session = this.get(sessionId);
        const result = await this.client.invoke<never>(
            'images.extent_layout.repair',
            {
                ...documentTarget(session),
                expectedRevision: session.revision,
                destination,
            },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.jobs.isJob(result)) throw new Error('images.extent_layout.repair did not return a job');
        return this.jobs.map(result);
    }

    async startMutations(
        sessionId: number,
        operations: Record<string, unknown>[],
        expectedRevision?: number,
    ): Promise<JobState> {
        if (operations.length === 0) throw new Error('at least one image mutation is required');
        const session = this.get(sessionId);
        return this.mutations.start({
            ...documentTarget(session),
            expectedRevision: expectedRevision ?? session.revision,
            manifest: {
                inline: {
                    schema_version: ALTERATION_MANIFEST_SCHEMA_VERSION,
                    operations,
                },
            },
            inputBindings: [],
        });
    }

    get(sessionId: number): SessionState {
        const session = this.sessions.get(sessionId);
        if (!session) throw new Error('Image session is closed or unknown');
        return session;
    }

    private async openedImage(sessionId: number, summary: ApiImageSummary): Promise<OpenedImage> {
        const session = this.get(sessionId);
        const document = selectedDocument(session);
        const source = summary.sources.find((member) => member.id === session.activeSourceMemberId);
        const hierarchy =
            document?.formatId === 'yamaha.a-series'
                ? await aSeriesHierarchy(
                      sessionId,
                      source ? imageLocation(source.source).displayName : document.name,
                      (parent, offset, limit) => this.contentChildren(sessionId, parent, offset, limit),
                  )
                : { tree: [], initialVolume: null };
        this.assertCurrent(sessionId, session);
        return {
            sessionId,
            activeSourceMemberId: session.activeSourceMemberId,
            revision: summary.revision,
            sources: summary.sources,
            documents: summary.documents,
            documentId: session.documentId,
            discovery: summary.discovery,
            companionSources: document?.companionSources.map(imageLocation) ?? [],
            floppySet: document?.floppySet ?? null,
            validation: validationSummary(document),
            objects: [],
            objectTotalCount: 0,
            format: document?.format,
            ...documentCapabilities(document),
            ...hierarchy,
        };
    }

    private assertCurrent(sessionId: number, session: SessionState): void {
        if (this.get(sessionId) !== session) throw new Error('Native document or workspace revision changed');
    }
}
