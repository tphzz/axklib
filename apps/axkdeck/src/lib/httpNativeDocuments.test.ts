import { afterEach, describe, expect, it, vi } from 'vitest';
import { AxklibHttpApiClient } from './httpApiClient';
import { HttpImageSessions } from './httpImageSessions';
import { HttpJobController } from './httpJobController';
import { documentSession, refreshedDocumentSession } from './httpNativeDocuments';
import { nativeWorkspaceFixture } from '../test/nativeWorkspaceFixture';
import { serverFileLocation } from './storageLocations';

afterEach(() => vi.restoreAllMocks());

function summary() {
    const native = nativeWorkspaceFixture();
    return {
        ...native,
        imageId: 'workspace-1',
        revision: 1,
        sources: [
            ...native.sources,
            {
                ...native.sources[0],
                id: 'source-2',
                source: { kind: 'FILE' as const, file: { rootId: 'root', relativePath: 'second.hds' } },
            },
        ],
        documents: [...native.documents, { ...native.documents[0], id: 'second', sourceMemberIds: ['source-2'] }],
    };
}

describe('native document HTTP sessions', () => {
    it('requires explicit selection when multiple documents exist and rejects unknown selections', () => {
        expect(documentSession(summary()).documentId).toBeNull();
        expect(() => documentSession(summary(), 'missing')).toThrow('no longer available');
        const selected = documentSession(summary(), 'second');
        expect(refreshedDocumentSession({ ...summary(), revision: 2, documents: [] }, selected).documentId).toBeNull();
    });

    it('labels the selected member and refreshes to Files if a commit removes its document', async () => {
        const client = new AxklibHttpApiClient({ baseUrl: 'http://localhost/api/v1', bearerToken: 'test' });
        const sessions = new HttpImageSessions(client, new HttpJobController(client));
        const initial = summary();
        vi.spyOn(client, 'request').mockImplementation(async (_method, path) => {
            if (path === '/images')
                return {
                    jobId: 'open',
                    operationId: 'images.open',
                    state: 'COMPLETED',
                    latestSequence: 1,
                    progress: null,
                    result: initial,
                    error: null,
                } as never;
            if (path.includes('/content')) return { items: [], totalCount: 0, nextCursor: null } as never;
            if (path === '/images/workspace-1')
                return { ...initial, revision: 2, documents: [initial.documents[0]] } as never;
            throw new Error(`Unexpected path: ${path}`);
        });
        const opened = await sessions.open(
            initial.sources.map((source) =>
                serverFileLocation(
                    source.source.kind === 'FILE' ? source.source.file! : { rootId: '', relativePath: '' },
                ),
            ),
        );
        const selected = await sessions.selectDocument(opened.sessionId, 'second');
        expect(selected.tree[0].name).toBe('second.hds');
        const refreshed = await sessions.refresh(opened.sessionId);
        expect(refreshed.documentId).toBeNull();
        expect(refreshed.tree).toEqual([]);
        expect(refreshed.documents).toHaveLength(1);
    });

    it.each(['filesystem', 'systemProgramContexts'] as const)(
        'rejects stale %s replies after a document switch',
        async (resource) => {
            const client = new AxklibHttpApiClient({ baseUrl: 'http://localhost/api/v1', bearerToken: 'test' });
            const sessions = new HttpImageSessions(client, new HttpJobController(client));
            let current = documentSession(summary(), 'second');
            vi.spyOn(sessions, 'get').mockImplementation(() => current);
            let finish!: (value: never) => void;
            vi.spyOn(client, 'request').mockReturnValue(
                new Promise((resolve) => {
                    finish = resolve;
                }),
            );
            const request = resource === 'filesystem' ? sessions.filesystem(1) : sessions.systemProgramContexts(1, 0);
            const rejected = expect(request).rejects.toThrow('changed');
            current = documentSession(summary(), null);
            finish({ items: [] } as never);
            await rejected;
        },
    );
});
