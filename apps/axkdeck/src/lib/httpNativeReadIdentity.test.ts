import { afterEach, describe, expect, it, vi } from 'vitest';
import { AxklibHttpApiClient } from './httpApiClient';
import { HttpImageSessions } from './httpImageSessions';
import { HttpJobController } from './httpJobController';
import { nativeWorkspaceFixture } from '../test/nativeWorkspaceFixture';
import { imageLocation } from './httpImageSources';
import { DocumentAudio } from './audio/documentAudio.svelte';
import type { ApiImageSummary } from './httpTransportModels';
import type { JobState, PreviewEnvelope } from './transport';

afterEach(() => vi.restoreAllMocks());

function deferred<T>() {
    let resolve!: (value: T) => void;
    const promise = new Promise<T>((done) => (resolve = done));
    return { promise, resolve };
}

async function workspace() {
    const native = nativeWorkspaceFixture();
    let summary: ApiImageSummary = {
        ...native,
        imageId: 'shared-image',
        revision: 1,
        sources: [
            native.sources[0],
            {
                ...native.sources[0],
                id: 'source-2',
                source: { kind: 'FILE', file: { rootId: 'root', relativePath: 'disk2.img' } },
            },
        ],
        documents: [
            {
                ...native.documents[0],
                id: 'shared-document',
                formatId: 'yamaha.tx16w',
                profile: 'yamaha-native',
                sourceMemberIds: ['source-1', 'source-2'],
            },
            { ...native.documents[0], id: 'other-document', sourceMemberIds: ['source-2'] },
        ],
    };
    const client = new AxklibHttpApiClient({ baseUrl: 'http://localhost/api/v1', bearerToken: 'test' });
    const jobs = new HttpJobController(client);
    const sessions = new HttpImageSessions(client, jobs);
    vi.spyOn(client, 'request').mockImplementation(async (method, path) => {
        if (path === '/images')
            return {
                jobId: 'open',
                operationId: 'images.open',
                state: 'COMPLETED',
                latestSequence: 1,
                result: summary,
            } as never;
        if (path === '/images/shared-image') return (method === 'DELETE' ? {} : summary) as never;
        if (path.includes('/content')) return { items: [], totalCount: 0, nextCursor: null } as never;
        throw new Error(`Unexpected request: ${path}`);
    });
    const opened = await sessions.open(summary.sources.map((source) => imageLocation(source.source)));
    return { client, jobs, sessions, id: opened.sessionId, revise: () => (summary = { ...summary, revision: 2 }) };
}

function preview(): PreviewEnvelope {
    return {
        imageId: 'shared-image',
        documentId: 'shared-document',
        revision: 1,
        objectId: 'wave-1',
        warnings: [],
        lanes: [
            {
                role: 'MONO',
                sourceObjectId: 'wave-1',
                sampleRate: 33333,
                sampleWidthBytes: 2,
                nativeBitDepth: 12,
                sampleRateApproximate: false,
                storedFrameCount: 4,
                playbackStartFrame: 0,
                playbackLengthFrames: 4,
                loopStartFrame: 0,
                loopLengthFrames: 0,
                bins: [{ minimum: 0, maximum: 1 }],
            },
        ],
    };
}

describe('native read identity across source selection', () => {
    it.each(['content', 'objects', 'relationships', 'detail'] as const)(
        'keeps deferred native %s replies when only the companion member changes',
        async (resource) => {
            const { sessions, client, id } = await workspace();
            const reply = deferred<never>();
            vi.mocked(client.request).mockReturnValueOnce(reply.promise);
            const pending =
                resource === 'detail' ? sessions.native.detail(id, 'wave-1') : sessions.native[resource](id);
            await sessions.selectDocument(id, 'shared-document', 'source-2');
            const result = { items: [], totalCount: 0, nextCursor: null };
            reply.resolve(result as never);
            await expect(pending).resolves.toEqual(resource === 'detail' ? result : []);
        },
    );

    it.each([
        'contentChildren',
        'objectPage',
        'relationshipPage',
        'systemProgramContexts',
        'validationIssues',
    ] as const)('keeps deferred A-series %s reads for the unchanged document', async (resource) => {
        const { sessions, client, id } = await workspace();
        const reply = deferred<never>();
        vi.mocked(client.request).mockReturnValueOnce(reply.promise);
        const pending =
            resource === 'contentChildren'
                ? sessions.contentChildren(id, '', 0, 20)
                : resource === 'objectPage'
                  ? sessions.objectPage(id, 0, 20)
                  : resource === 'relationshipPage'
                    ? sessions.relationshipPage(id, 0, 20)
                    : resource === 'systemProgramContexts'
                      ? sessions.systemProgramContexts(id, 0)
                      : sessions.validationIssues(id);
        await sessions.selectDocument(id, 'shared-document', 'source-2');
        reply.resolve({ items: [], totalCount: 0, nextCursor: null } as never);
        await expect(pending).resolves.toBeDefined();
    });

    it.each(['document', 'revision', 'closed', 'away-and-back'] as const)(
        'rejects deferred replies after %s changes',
        async (change) => {
            const { sessions, client, id, revise } = await workspace();
            const reply = deferred<never>();
            vi.mocked(client.request).mockReturnValueOnce(reply.promise);
            const pending = sessions.native.objects(id);
            const rejected = expect(pending).rejects.toThrow();
            if (change === 'closed') await sessions.close(id);
            else if (change === 'revision') {
                revise();
                await sessions.refresh(id);
            } else {
                await sessions.selectDocument(id, 'other-document', 'source-2');
                if (change === 'away-and-back') await sessions.selectDocument(id, 'shared-document', 'source-1');
            }
            reply.resolve({ items: [], totalCount: 0, nextCursor: null } as never);
            await rejected;
        },
    );

    it('publishes the pending Wave preview instead of retaining a permanent unavailable error', async () => {
        const { sessions, client, jobs, id } = await workspace();
        const reply = deferred<JobState>();
        vi.spyOn(client, 'invoke').mockResolvedValue({
            jobId: 'preview',
            operationId: 'images.preview',
            state: 'QUEUED',
        });
        vi.spyOn(jobs, 'wait').mockReturnValue(reply.promise);
        const transport = {
            audioIdentity: () => ({ imageId: 'shared-image', documentId: 'shared-document', revision: 1 }),
            preview: sessions.native.preview.bind(sessions.native),
        };
        const audio = new DocumentAudio(transport as never, () => id);
        try {
            audio.request('wave-1');
            await vi.waitFor(() => expect(jobs.wait).toHaveBeenCalledOnce());
            await sessions.selectDocument(id, 'shared-document', 'source-2');
            reply.resolve({ jobId: 2, kind: 'images.preview', status: 'completed', result: preview() });
            await vi.waitFor(() => expect(audio.previews['wave-1']).toBeDefined());
            expect(audio.errors['wave-1']).toBeUndefined();
            expect(audio.previews['wave-1'].documentId).toBe('shared-document');
        } finally {
            audio.dispose();
        }
    });

    it('preserves document cursor caches but rejects a stale Files reply', async () => {
        const { sessions, client, id } = await workspace();
        const previous = sessions.get(id);
        previous.objectCursors.set('scope', new Map([[20, 'cursor']]));
        const reply = deferred<never>();
        vi.mocked(client.request).mockReturnValueOnce(reply.promise);
        const pending = sessions.filesystem(id);
        const rejected = expect(pending).rejects.toThrow('changed');
        await sessions.selectDocument(id, 'shared-document', 'source-2');
        reply.resolve({ items: [] } as never);
        await rejected;
        expect(sessions.get(id).objectCursors).toBe(previous.objectCursors);
    });

    it('does not resurrect old replies when a failed document selection rolls back', async () => {
        const { sessions, client, id } = await workspace();
        const previous = sessions.get(id);
        const reply = deferred<never>();
        vi.mocked(client.request).mockReturnValueOnce(reply.promise);
        const pending = sessions.native.detail(id, 'wave-1');
        const rejected = expect(pending).rejects.toThrow('changed');
        vi.mocked(client.request).mockRejectedValueOnce(new Error('Content unavailable'));
        await expect(sessions.selectDocument(id, 'other-document', 'source-2')).rejects.toThrow('Content unavailable');
        expect(sessions.get(id).documentId).toBe(previous.documentId);
        reply.resolve({} as never);
        await rejected;
        expect(sessions.get(id).nativeReadGeneration).toBeGreaterThan(previous.nativeReadGeneration);
    });

    it('continues a pending cursor page after a companion switch and resets it on a true transition', async () => {
        const { sessions, client, id } = await workspace();
        const reply = deferred<never>();
        vi.mocked(client.request).mockReturnValueOnce(reply.promise);
        const pending = sessions.objectPage(id, 0, 1);
        await sessions.selectDocument(id, 'shared-document', 'source-2');
        reply.resolve({
            items: [{ id: 'wave-1', type: 'WAVE', name: 'Tone' }],
            totalCount: 2,
            nextCursor: 'next-page',
        } as never);
        await pending;
        vi.mocked(client.request).mockResolvedValueOnce({ items: [], totalCount: 1, nextCursor: null } as never);
        await sessions.objectPage(id, 1, 1);
        expect(client.request).toHaveBeenLastCalledWith('GET', expect.stringContaining('cursor=next-page'));
        await sessions.selectDocument(id, null, 'source-2');
        expect(sessions.get(id).objectCursors.size).toBe(0);
    });
});
