import { afterEach, describe, expect, it, vi } from 'vitest';

import { AxklibApiError, AxklibHttpApiClient, type OperationCapability } from './httpApiClient';

function operation(overrides: Partial<OperationCapability>): OperationCapability {
    return {
        id: 'report.info',
        method: 'POST',
        route: '/api/v1/reports/info',
        mode: 'JOB',
        operationClass: 'READ',
        requiresIdempotency: false,
        cliParity: true,
        variant: null,
        requestSchema: 'InfoRequest',
        resultSchema: 'InfoResult',
        implemented: true,
        ...overrides,
    };
}

function jsonResponse(body: unknown, status = 200): Response {
    return new Response(JSON.stringify(body), {
        status,
        headers: { 'Content-Type': 'application/json' },
    });
}

afterEach(() => vi.restoreAllMocks());

describe('AxklibHttpApiClient', () => {
    it('discovers registry operations and dispatches a single-route operation without duplicating its id', async () => {
        const fetchMock = vi
            .spyOn(globalThis, 'fetch')
            .mockResolvedValueOnce(
                jsonResponse({
                    data: { apiVersion: 'v1', operations: [operation({})], limits: {} },
                }),
            )
            .mockResolvedValueOnce(jsonResponse({ data: { jobId: 'job-1', state: 'QUEUED' } }, 202));
        const client = new AxklibHttpApiClient({
            baseUrl: 'http://127.0.0.1:7331/api/v1/',
            bearerToken: 'secret-token',
        });

        await client.invoke('report.info', { sources: [] });

        expect(fetchMock).toHaveBeenNthCalledWith(
            2,
            'http://127.0.0.1:7331/api/v1/reports/info',
            expect.objectContaining({
                body: JSON.stringify({ sources: [] }),
                headers: expect.objectContaining({ Authorization: 'Bearer secret-token' }),
            }),
        );
    });

    it('omits request bodies for registry GET operations', async () => {
        const fetchMock = vi
            .spyOn(globalThis, 'fetch')
            .mockResolvedValueOnce(
                jsonResponse({
                    data: {
                        apiVersion: 'v1',
                        operations: [
                            operation({
                                id: 'create.hds.profiles',
                                method: 'GET',
                                route: '/api/v1/hard-disk-creation-profiles',
                                mode: 'REQUEST',
                                requestSchema: 'EmptyRequest',
                                resultSchema: 'HardDiskCreationProfiles',
                            }),
                        ],
                        limits: {},
                    },
                }),
            )
            .mockResolvedValueOnce(jsonResponse({ data: { schemaVersion: '1.0', profiles: [] } }));
        const client = new AxklibHttpApiClient({ baseUrl: 'http://localhost/api/v1', bearerToken: 'token' });

        await client.invoke('create.hds.profiles');

        expect(fetchMock).toHaveBeenNthCalledWith(
            2,
            'http://localhost/api/v1/hard-disk-creation-profiles',
            expect.objectContaining({ method: 'GET', body: undefined }),
        );
        await expect(client.invoke('create.hds.profiles', { unexpected: true })).rejects.toMatchObject({
            code: 'invalid_operation_input',
        });
    });

    it('adds operationId only when several registry operations share one Crow route', async () => {
        const operations = [
            operation({ id: 'extract.wav', route: '/api/v1/extractions', variant: 'WAV' }),
            operation({ id: 'extract.sfz', route: '/api/v1/extractions', variant: 'SFZ' }),
        ];
        const fetchMock = vi
            .spyOn(globalThis, 'fetch')
            .mockResolvedValueOnce(jsonResponse({ data: { apiVersion: 'v1', operations, limits: {} } }))
            .mockResolvedValueOnce(jsonResponse({ data: { jobId: 'job-2', state: 'QUEUED' } }, 202));
        const client = new AxklibHttpApiClient({ baseUrl: 'http://localhost/api/v1', bearerToken: 'token' });

        await client.invoke('extract.sfz', { sources: [] }, { idempotencyKey: 'request-7' });

        expect(fetchMock).toHaveBeenNthCalledWith(
            2,
            'http://localhost/api/v1/extractions',
            expect.objectContaining({
                body: JSON.stringify({ sources: [], operationId: 'extract.sfz' }),
                headers: expect.objectContaining({ 'Idempotency-Key': 'request-7' }),
            }),
        );
    });

    it('maps stable server errors without exposing transport-specific response handling', async () => {
        vi.spyOn(globalThis, 'fetch').mockResolvedValueOnce(
            jsonResponse(
                {
                    error: {
                        code: 'path_outside_root',
                        message: 'path is outside the configured root',
                        context: { relativePath: 'outside/image.hds' },
                        requestId: 'request-9',
                        retryable: false,
                    },
                },
                422,
            ),
        );
        const client = new AxklibHttpApiClient({ baseUrl: 'http://localhost/api/v1', bearerToken: 'token' });

        const error = await client.request('POST', '/files/metadata', {}).catch((reason: unknown) => reason);

        expect(error).toBeInstanceOf(AxklibApiError);
        expect(error).toMatchObject({
            code: 'path_outside_root',
            status: 422,
            requestId: 'request-9',
            context: { relativePath: 'outside/image.hds' },
            retryable: false,
        });
    });

    it('browses server-local roots without exposing absolute server paths', async () => {
        const fetchMock = vi
            .spyOn(globalThis, 'fetch')
            .mockResolvedValueOnce(
                jsonResponse({ data: { roots: [{ id: 'workspace', displayName: 'Workspace', writable: true }] } }),
            )
            .mockResolvedValueOnce(
                jsonResponse({
                    data: {
                        directory: { rootId: 'workspace', relativePath: '' },
                        entries: [{ name: 'disks', relativePath: 'disks', kind: 'directory', size: null }],
                        truncated: false,
                        nextCursor: null,
                    },
                }),
            );
        const client = new AxklibHttpApiClient({ baseUrl: 'http://localhost/api/v1', bearerToken: 'token' });

        await expect(client.roots()).resolves.toEqual([{ id: 'workspace', displayName: 'Workspace', writable: true }]);
        await expect(client.listDirectory({ rootId: 'workspace', relativePath: '' })).resolves.toMatchObject({
            entries: [{ relativePath: 'disks', kind: 'directory' }],
        });
        expect(fetchMock).toHaveBeenNthCalledWith(
            2,
            'http://localhost/api/v1/files/list',
            expect.objectContaining({
                body: JSON.stringify({
                    directory: { rootId: 'workspace', relativePath: '' },
                    limit: 200,
                    cursor: null,
                }),
            }),
        );
    });

    it('uses typed sandbox references for directory and entry mutations', async () => {
        const metadata = {
            rootId: 'workspace',
            relativePath: 'Imports',
            kind: 'directory',
            size: null,
            writable: true,
        };
        const fetchMock = vi
            .spyOn(globalThis, 'fetch')
            .mockResolvedValueOnce(jsonResponse({ data: metadata }, 201))
            .mockResolvedValueOnce(jsonResponse({ data: { ...metadata, relativePath: 'Renamed' } }))
            .mockResolvedValueOnce(jsonResponse({ data: { deleted: true } }));
        const client = new AxklibHttpApiClient({ baseUrl: 'http://localhost/api/v1', bearerToken: 'token' });

        await client.createDirectory({ rootId: 'workspace', relativePath: '' }, 'Imports');
        await client.renameEntry({ rootId: 'workspace', relativePath: 'Imports' }, 'Renamed');
        await client.deleteEntry({ rootId: 'workspace', relativePath: 'Renamed' });

        expect(fetchMock).toHaveBeenNthCalledWith(
            1,
            'http://localhost/api/v1/filesystem/directories',
            expect.objectContaining({
                method: 'POST',
                body: JSON.stringify({
                    parent: { rootId: 'workspace', relativePath: '' },
                    name: 'Imports',
                }),
            }),
        );
        expect(fetchMock).toHaveBeenNthCalledWith(
            2,
            'http://localhost/api/v1/filesystem/entries',
            expect.objectContaining({
                method: 'PATCH',
                body: JSON.stringify({
                    entry: { rootId: 'workspace', relativePath: 'Imports' },
                    name: 'Renamed',
                }),
            }),
        );
        expect(fetchMock).toHaveBeenNthCalledWith(
            3,
            'http://localhost/api/v1/filesystem/entries?rootId=workspace&relativePath=Renamed',
            expect.objectContaining({ method: 'DELETE', body: undefined }),
        );
    });

    it('manages persisted workspaces with revision-controlled requests', async () => {
        const fetchMock = vi
            .spyOn(globalThis, 'fetch')
            .mockResolvedValueOnce(
                jsonResponse({
                    data: {
                        state: 'NO_AVAILABLE_WORKSPACE',
                        revision: 0,
                        workspaces: [],
                        configurationIssue: null,
                    },
                }),
            )
            .mockResolvedValueOnce(
                jsonResponse(
                    {
                        data: {
                            id: 'workspace-a',
                            displayName: 'Samples',
                            path: '/srv/samples',
                            writable: true,
                            effectiveWritable: true,
                            status: 'AVAILABLE',
                            issue: null,
                        },
                    },
                    201,
                ),
            )
            .mockResolvedValueOnce(new Response(null, { status: 204 }));
        const client = new AxklibHttpApiClient({ baseUrl: 'http://localhost/api/v1', bearerToken: 'token' });

        await expect(client.workspaces()).resolves.toMatchObject({ state: 'NO_AVAILABLE_WORKSPACE', revision: 0 });
        await client.createWorkspace({ displayName: 'Samples', path: '/srv/samples', writable: true, revision: 0 });
        await client.removeWorkspace('workspace-a', 1);

        expect(fetchMock).toHaveBeenNthCalledWith(
            2,
            'http://localhost/api/v1/workspaces',
            expect.objectContaining({
                method: 'POST',
                body: JSON.stringify({ displayName: 'Samples', path: '/srv/samples', writable: true, revision: 0 }),
            }),
        );
        expect(fetchMock).toHaveBeenNthCalledWith(
            3,
            'http://localhost/api/v1/workspaces/workspace-a',
            expect.objectContaining({
                method: 'DELETE',
                body: JSON.stringify({ revision: 1 }),
            }),
        );
    });

    it('uploads admitted client files in bounded chunks and completes the temporary UploadRef', async () => {
        const fetchMock = vi
            .spyOn(globalThis, 'fetch')
            .mockResolvedValueOnce(
                jsonResponse(
                    {
                        data: {
                            uploadId: 'upload-1',
                            filename: 'sample.wav',
                            kind: 'audio',
                            mediaType: 'audio/wav',
                            declaredSize: 5,
                            receivedSize: 0,
                            state: 'RECEIVING',
                            expiresInSeconds: 600,
                        },
                    },
                    201,
                ),
            )
            .mockResolvedValueOnce(
                jsonResponse({
                    data: {
                        uploadId: 'upload-1',
                        filename: 'sample.wav',
                        kind: 'audio',
                        mediaType: 'audio/wav',
                        declaredSize: 5,
                        receivedSize: 3,
                        state: 'RECEIVING',
                        expiresInSeconds: 600,
                    },
                }),
            )
            .mockResolvedValueOnce(
                jsonResponse({
                    data: {
                        uploadId: 'upload-1',
                        filename: 'sample.wav',
                        kind: 'audio',
                        mediaType: 'audio/wav',
                        declaredSize: 5,
                        receivedSize: 5,
                        state: 'RECEIVING',
                        expiresInSeconds: 600,
                    },
                }),
            )
            .mockResolvedValueOnce(
                jsonResponse({
                    data: {
                        uploadId: 'upload-1',
                        filename: 'sample.wav',
                        kind: 'audio',
                        mediaType: 'audio/wav',
                        declaredSize: 5,
                        receivedSize: 5,
                        state: 'READY',
                        expiresInSeconds: 600,
                    },
                }),
            );
        const progress = vi.fn();
        const client = new AxklibHttpApiClient({ baseUrl: 'http://localhost/api/v1', bearerToken: 'token' });

        const result = await client.uploadBlob(
            new Blob(['12345'], { type: 'audio/wav' }),
            {
                filename: 'sample.wav',
                kind: 'audio',
            },
            { chunkBytes: 3, onProgress: progress },
        );

        expect(result).toMatchObject({ uploadId: 'upload-1', state: 'READY' });
        expect(progress.mock.calls).toEqual([
            [3, 5],
            [5, 5],
        ]);
        expect(fetchMock).toHaveBeenNthCalledWith(
            2,
            'http://localhost/api/v1/uploads/upload-1',
            expect.objectContaining({
                body: expect.any(Blob),
                headers: expect.objectContaining({ 'Upload-Offset': '0', 'Content-Type': 'application/octet-stream' }),
            }),
        );
    });

    it('opens an explicit authenticated download as a streamable response', async () => {
        const response = new Response('bytes', { status: 206, headers: { 'Content-Range': 'bytes 2-6/10' } });
        const fetchMock = vi.spyOn(globalThis, 'fetch').mockResolvedValueOnce(response);
        const client = new AxklibHttpApiClient({ baseUrl: 'http://localhost/api/v1', bearerToken: 'token' });

        const opened = await client.openDownload(
            { rootId: 'workspace', relativePath: 'exports/out.wav' },
            {
                start: 2,
                end: 6,
            },
        );

        expect(opened.body).not.toBeNull();
        expect(fetchMock).toHaveBeenCalledWith(
            'http://localhost/api/v1/files/content?rootId=workspace&relativePath=exports%2Fout.wav',
            expect.objectContaining({
                headers: expect.objectContaining({ Authorization: 'Bearer token', Range: 'bytes=2-6' }),
            }),
        );
    });

    it('creates, streams, and explicitly deletes a bounded directory archive', async () => {
        const snapshot = {
            archiveId: 'archive-1',
            filename: 'sfz-export.tar',
            sizeBytes: 2048,
            entryCount: 2,
            expiresInSeconds: 300,
            contentPath: '/api/v1/download-archives/archive-1/content',
        };
        const fetchMock = vi
            .spyOn(globalThis, 'fetch')
            .mockResolvedValueOnce(jsonResponse({ data: snapshot }, 201))
            .mockResolvedValueOnce(
                new Response('tar-bytes', { status: 200, headers: { 'Content-Type': 'application/x-tar' } }),
            )
            .mockResolvedValueOnce(new Response(null, { status: 204 }));
        const client = new AxklibHttpApiClient({ baseUrl: 'http://localhost/api/v1', bearerToken: 'token' });

        const created = await client.createDirectoryArchive({ rootId: 'workspace', relativePath: 'exports/sfz' });
        const opened = await client.openDirectoryArchive(created);
        await client.deleteDirectoryArchive(created);

        expect(await opened.text()).toBe('tar-bytes');
        expect(fetchMock).toHaveBeenNthCalledWith(
            1,
            'http://localhost/api/v1/files/archive',
            expect.objectContaining({
                body: JSON.stringify({ directory: { rootId: 'workspace', relativePath: 'exports/sfz' } }),
            }),
        );
        expect(fetchMock).toHaveBeenNthCalledWith(
            2,
            'http://localhost/api/v1/download-archives/archive-1/content',
            expect.objectContaining({ headers: expect.objectContaining({ Authorization: 'Bearer token' }) }),
        );
        expect(fetchMock).toHaveBeenNthCalledWith(
            3,
            'http://localhost/api/v1/download-archives/archive-1/content',
            expect.objectContaining({ method: 'DELETE' }),
        );
    });
});
