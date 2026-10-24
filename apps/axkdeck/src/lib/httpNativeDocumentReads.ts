import type { AxklibHttpApiClient } from './httpApiClient';
import type { ApiContentItem, ApiObjectItem, ApiPage, ApiRelationshipItem, SessionState } from './httpTransportModels';
import type { NativeObjectDetail, PreviewEnvelope } from './transport';
import { documentPath } from './httpNativeDocuments';
import { sameAudioIdentity } from './audio/audioIdentity';
import { normalizeWaveformPreview } from './waveformPreview';
import { readContext, readTrace } from './readDiagnostics';

export class HttpNativeDocumentReads {
    constructor(
        private readonly client: AxklibHttpApiClient,
        private readonly session: (id: number) => SessionState,
    ) {}

    private async page<Item>(sessionId: number, resource: string): Promise<Item[]> {
        const session = this.session(sessionId);
        const path = documentPath(session);
        const items: Item[] = [];
        let cursor: string | null = null;
        for (let page = 0; page < 256; page++) {
            const query = new URLSearchParams({ limit: '256' });
            if (cursor) query.set('cursor', cursor);
            const result = await this.client.request<ApiPage<Item>>('GET', `${path}/${resource}?${query}`);
            if (this.session(sessionId) !== session) throw new Error('Native document changed during loading');
            items.push(...result.items);
            cursor = result.nextCursor;
            if (!cursor) return items;
        }
        throw new Error('Native document pagination exceeds its safety limit');
    }

    content(sessionId: number): Promise<ApiContentItem[]> {
        return this.page(sessionId, 'content');
    }
    objects(sessionId: number): Promise<ApiObjectItem[]> {
        return this.page(sessionId, 'objects');
    }
    relationships(sessionId: number): Promise<ApiRelationshipItem[]> {
        return this.page(sessionId, 'relationships');
    }

    async detail(sessionId: number, objectId: string): Promise<NativeObjectDetail> {
        const session = this.session(sessionId);
        const detail = await this.client.request<NativeObjectDetail>(
            'GET',
            `${documentPath(session)}/objects/${encodeURIComponent(objectId)}`,
        );
        if (this.session(sessionId) !== session) throw new Error('Native document changed during loading');
        return detail;
    }

    async preview(sessionId: number, objectId: string, bins: number, signal?: AbortSignal) {
        const session = this.session(sessionId);
        const context = readContext(sessionId, objectId);
        const identity = { imageId: session.remoteId, documentId: session.documentId!, revision: session.revision };
        const query = new URLSearchParams({ objectId, bins: String(bins), expectedRevision: String(session.revision) });
        const result = await this.client.request<PreviewEnvelope>(
            'GET',
            `${documentPath(session)}/preview?${query}`,
            undefined,
            {},
            signal,
        );
        if (this.session(sessionId) !== session || !sameAudioIdentity(result, identity) || result.objectId !== objectId)
            throw new Error('Native document changed during loading');
        const trace = readTrace('preview-normalize', { sessionId, objectId, bins }, context);
        const preview = normalizeWaveformPreview(result);
        trace?.finish('success');
        return preview;
    }
}
