import type { AxklibHttpApiClient } from './httpApiClient';
import type { components } from './generated/axklibApiV1';
import { HttpJobController } from './httpJobController';
import {
    type ApiContentItem,
    type ApiImageSummary,
    type ApiObjectItem,
    type ApiPage,
    type ApiRelationshipItem,
    type SessionState,
    mapContentItem,
    mapObject,
    mapRelationship,
    validationSummary,
} from './httpTransportModels';
import { collectPages } from './pagination';
import type { ImageLocation } from './storageLocations';
import type {
    CompanionSelection,
    ContentPage,
    ObjectPage,
    ObjectPageFilter,
    OpenedImage,
    RelationshipPage,
    RelationshipPageFilter,
    JobState,
    PlacementRepairInspection,
    PlacementRepairScope,
    VolumeDeletionInspection,
} from './transport';
import { randomIdempotencyKey } from './httpTransportWire';
import type { DiskTreeItem } from './types';

const ALTERATION_MANIFEST_SCHEMA_VERSION = '1.0';

export class HttpImageSessions {
    private readonly sessions = new Map<number, SessionState>();
    private nextSessionId = 1;

    constructor(
        private readonly client: AxklibHttpApiClient,
        private readonly jobs: HttpJobController,
    ) {}

    async open(location: ImageLocation): Promise<OpenedImage> {
        if (location.kind !== 'server-file' && location.kind !== 'axk-object-directory') {
            throw new Error('Opening images requires a server sandbox file selection or AXK object directory');
        }
        const wireSource = imageSourceReference(location);
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
        this.replaceRevision(session, summary.revision);
        return this.openedImage(sessionId, summary);
    }

    async attachCompanions(sessionId: number, selection: CompanionSelection): Promise<OpenedImage> {
        const session = this.get(sessionId);
        const wireSelection: components['schemas']['CompanionSelection'] =
            selection.kind === 'sources'
                ? { kind: 'SOURCES', sources: selection.sources.map(imageSourceReference) }
                : { kind: 'IMMEDIATE_SIBLINGS' };
        const summary = await this.client.request<ApiImageSummary>(
            'POST',
            `/images/${encodeURIComponent(session.remoteId)}/companions`,
            {
                expectedRevision: session.revision,
                selection: wireSelection,
            },
        );
        this.replaceRevision(session, summary.revision);
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
            `/images/${encodeURIComponent(session.remoteId)}/relationships?${query}`,
        );
        if (page.nextCursor) cursors.set(offset + page.items.length, page.nextCursor);
        return { relationships: page.items.map(mapRelationship), totalCount: page.totalCount };
    }

    async close(sessionId: number): Promise<void> {
        const session = this.sessions.get(sessionId);
        if (!session) return;
        await this.client.request('DELETE', `/images/${encodeURIComponent(session.remoteId)}`);
        this.sessions.delete(sessionId);
    }

    async inspectVolumeDeletion(
        sessionId: number,
        partitionIndex: number,
        volumeName: string,
    ): Promise<VolumeDeletionInspection> {
        const session = this.get(sessionId);
        const result = await this.client.invoke<VolumeDeletionInspection>('images.volume_deletion.inspect', {
            imageId: session.remoteId,
            expectedRevision: session.revision,
            partitionIndex,
            volumeName,
        });
        if (this.jobs.isJob(result)) throw new Error('images.volume_deletion.inspect unexpectedly returned a job');
        return result;
    }

    async inspectPlacement(
        sessionId: number,
        scope: PlacementRepairScope,
        recoveryVolumeName?: string,
    ): Promise<PlacementRepairInspection> {
        const session = this.get(sessionId);
        const result = await this.client.invoke<PlacementRepairInspection>('images.placement.inspect', {
            imageId: session.remoteId,
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
        const result = await this.client.invoke<never>(
            'images.placement.repair',
            {
                imageId: session.remoteId,
                expectedRevision: session.revision,
                scope,
                ...(recoveryVolumeName ? { recoveryVolumeName } : {}),
            },
            { idempotencyKey: randomIdempotencyKey() },
        );
        if (!this.jobs.isJob(result)) throw new Error('images.placement.repair did not return a job');
        return this.jobs.map(result);
    }

    async startMutation(sessionId: number, operation: Record<string, unknown>): Promise<JobState> {
        const session = this.get(sessionId);
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
        if (!this.jobs.isJob(job)) throw new Error('images.alter did not return a job');
        return this.jobs.map(job);
    }

    get(sessionId: number): SessionState {
        const session = this.sessions.get(sessionId);
        if (!session) throw new Error('Image session is closed or unknown');
        return session;
    }

    private async openedImage(sessionId: number, summary: ApiImageSummary): Promise<OpenedImage> {
        const session = this.get(sessionId);
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
            companionSources: summary.companionSources.map(imageLocation),
            floppySet: summary.floppySet,
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
            volumePackageExportAvailable: (summary.availableOperations ?? []).includes('images.volume_package_export'),
            volumeFloppyExportAvailable: (summary.availableOperations ?? []).includes('images.volume_floppy_export'),
            audioExportAvailable: (summary.availableOperations ?? []).includes('images.audio_export'),
            sequenceExportAvailable: (summary.availableOperations ?? []).includes('images.sequence_export'),
            mediaConversionAvailable: (summary.availableOperations ?? []).includes('images.media_conversion'),
            tree: [disk],
        };
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
        const items = await collectPages((offset, limit) => this.contentChildren(sessionId, parentId, offset, limit), {
            key: (item) => item.id,
        });
        return { items, totalCount: items.length };
    }

    private replaceRevision(session: SessionState, revision: number): void {
        session.revision = revision;
        session.contentCursors.clear();
        session.contentItems.clear();
        session.objectCursors.clear();
        session.relationshipCursors.clear();
    }
}

function imageSourceReference(location: ImageLocation): components['schemas']['ImageSourceRef'] {
    return location.kind === 'server-file'
        ? { kind: 'FILE', file: location.reference }
        : { kind: 'AXK_OBJECT_DIRECTORY', directory: location.reference };
}

function imageLocation(source: components['schemas']['ImageSourceRef']): ImageLocation {
    if (source.kind === 'FILE') {
        return {
            kind: 'server-file',
            reference: source.file,
            displayName: source.file.relativePath || source.file.rootId,
        };
    }
    return {
        kind: 'axk-object-directory',
        reference: source.directory,
        displayName: source.directory.relativePath || source.directory.rootId,
    };
}
