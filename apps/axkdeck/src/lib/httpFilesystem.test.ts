import { afterEach, describe, expect, it, vi } from 'vitest';
import { AxklibHttpApiClient } from './httpApiClient';
import { HttpImageSessions } from './httpImageSessions';
import { HttpJobController } from './httpJobController';
import { serverFileLocation, clientUploadLocation } from './storageLocations';

afterEach(() => vi.restoreAllMocks());

function setup() {
    const client = new AxklibHttpApiClient({ baseUrl: 'http://localhost/api/v1', bearerToken: 'test' });
    const sessions = new HttpImageSessions(client, new HttpJobController(client));
    vi.spyOn(sessions, 'get').mockReturnValue({
        remoteId: 'remote-image',
        revision: 9,
        source: serverFileLocation({ rootId: 'images', relativePath: 'test.hds' }),
        contentCursors: new Map(),
        contentItems: new Map(),
        objectCursors: new Map(),
        relationshipCursors: new Map(),
    });
    return { client, sessions };
}

describe('Files HTTP binding', () => {
    it('binds SU700 inspection and execution to the reviewed session and a stable write identity', async () => {
        const { client, sessions } = setup();
        const invoke = vi.spyOn(client, 'invoke').mockResolvedValue({
            jobId: 'su700',
            operationId: 'images.su700.import',
            state: 'QUEUED',
            latestSequence: 0,
            progress: null,
            result: null,
            error: null,
        });
        const source = serverFileLocation({ rootId: 'host', relativePath: 'floppy.img' });
        const destination = { sessionId: 1, expectedRevision: 6, rootEntryId: 'root', volumeName: 'New' };
        await sessions.startSu700Import({ source, destination, includedExtras: [] });
        expect(invoke).toHaveBeenLastCalledWith(
            'images.su700.import.inspect',
            {
                source: { fileRef: source.reference },
                destination: { imageId: 'remote-image', expectedRevision: 6, rootEntryId: 'root', volumeName: 'New' },
                includedExtras: [],
            },
            {},
        );
        const expectedSource = { revision: 'original', sha256: 'a'.repeat(64), sizeBytes: 100 };
        await sessions.startSu700Import({
            source,
            destination,
            includedExtras: [],
            expectedSource,
            idempotencyKey: 'retained-key',
        });
        expect(invoke).toHaveBeenLastCalledWith('images.su700.import', expect.objectContaining({ expectedSource }), {
            idempotencyKey: 'retained-key',
        });
    });
    it('inspects raw exports using the reviewed selection and revision', async () => {
        const { client, sessions } = setup();
        const inspection = {
            rootDirectory: null,
            imageId: 'remote-image',
            revision: 6,
            entries: [],
            notices: [{ entryId: 'metadata', sourcePath: '/sfserrlog', message: 'Filesystem metadata omitted' }],
            totalBytes: 0,
        };
        const invoke = vi.spyOn(client, 'invoke').mockResolvedValue(inspection);
        expect(await sessions.inspectFilesystemExport(1, 6, ['folder', 'empty'])).toEqual(inspection);
        expect(invoke).toHaveBeenCalledWith('images.filesystem.export.inspect', {
            imageId: 'remote-image',
            expectedRevision: 6,
            entryIds: ['folder', 'empty'],
            layout: 'SELECTED_ENTRIES',
        });
    });

    it.each([
        { kind: 'WORKSPACE', output: { rootId: 'exports', relativePath: 'Raw files' } },
        { kind: 'DOWNLOAD', directoryName: 'Raw files' },
    ] as const)('starts a raw export job to $kind', async (destination) => {
        const { client, sessions } = setup();
        const invoke = vi.spyOn(client, 'invoke').mockResolvedValue({
            jobId: 'export-files',
            operationId: 'images.filesystem.export',
            state: 'QUEUED',
            latestSequence: 0,
            progress: null,
            result: null,
            error: null,
        });
        expect(await sessions.startFilesystemExport(1, 6, ['folder'], destination)).toMatchObject({
            kind: 'images.filesystem.export',
            status: 'queued',
        });
        expect(invoke).toHaveBeenCalledWith(
            'images.filesystem.export',
            {
                imageId: 'remote-image',
                expectedRevision: 6,
                entryIds: ['folder'],
                destination,
                layout: 'SELECTED_ENTRIES',
            },
            { idempotencyKey: expect.any(String) },
        );
    });

    it('rejects empty export selections and unexpected execution responses', async () => {
        const { client, sessions } = setup();
        const invoke = vi.spyOn(client, 'invoke').mockResolvedValue({});
        const destination = { kind: 'DOWNLOAD', directoryName: 'Files' } as const;
        await expect(sessions.inspectFilesystemExport(1, 6, [])).rejects.toThrow('At least one');
        await expect(sessions.startFilesystemExport(1, 6, [], destination)).rejects.toThrow('At least one');
        expect(invoke).not.toHaveBeenCalled();
        await expect(sessions.startFilesystemExport(1, 6, ['file'], destination)).rejects.toThrow(
            'did not return a job',
        );
    });

    it('rejects an unexpected job response from export inspection', async () => {
        const { client, sessions } = setup();
        vi.spyOn(client, 'invoke').mockResolvedValue({
            jobId: 'unexpected',
            operationId: 'images.filesystem.export.inspect',
            state: 'QUEUED',
            latestSequence: 0,
            progress: null,
            result: null,
            error: null,
        });
        await expect(sessions.inspectFilesystemExport(1, 6, ['file'])).rejects.toThrow('unexpectedly returned a job');
    });

    it('inspects imports in the reviewed destination and revision without uploading or writing', async () => {
        const { client, sessions } = setup();
        const invoke = vi.spyOn(client, 'invoke').mockResolvedValue({
            jobId: 'import-review',
            operationId: 'images.filesystem.import.inspect',
            state: 'QUEUED',
            latestSequence: 0,
            progress: null,
            result: null,
            error: null,
        });
        const entries = [{ relativePath: ['file'], directory: false, sizeBytes: 12, conflict: 'REPLACE' as const }];
        const result = await sessions.startFilesystemImportInspection(1, 6, 'reviewed-parent', entries);
        expect(result).toMatchObject({ kind: 'images.filesystem.import.inspect', status: 'queued' });
        expect(invoke).toHaveBeenCalledWith('images.filesystem.import.inspect', {
            imageId: 'remote-image',
            expectedRevision: 6,
            parentEntryId: 'reviewed-parent',
            entries,
        });
        await expect(sessions.startFilesystemImportInspection(1, 6, 'parent', [])).rejects.toThrow(
            'between 1 and 10000',
        );
        invoke.mockResolvedValue({});
        await expect(sessions.startFilesystemImportInspection(1, 6, 'parent', entries)).rejects.toThrow(
            'did not return a job',
        );
    });

    it('inspects host and uploaded files as one cancellable job without requiring an image', async () => {
        const { client, sessions } = setup();
        const invoke = vi.spyOn(client, 'invoke').mockResolvedValue({
            jobId: 'inspection',
            operationId: 'filesystem.inputs.inspect',
            state: 'QUEUED',
            latestSequence: 0,
            progress: null,
            result: null,
            error: null,
        });
        const result = await sessions.startFilesystemInputInspection([
            serverFileLocation({ rootId: 'host', relativePath: 'source' }),
            clientUploadLocation({ uploadId: 'upload' }, 'FILE', 'uploaded'),
        ]);
        expect(result).toMatchObject({ kind: 'filesystem.inputs.inspect', status: 'queued' });
        expect(invoke).toHaveBeenCalledWith('filesystem.inputs.inspect', {
            inputs: [{ fileRef: { rootId: 'host', relativePath: 'source' } }, { uploadRef: { uploadId: 'upload' } }],
        });
        await expect(sessions.startFilesystemInputInspection([])).rejects.toThrow('between 1 and 10000');
        invoke.mockResolvedValue({});
        await expect(
            sessions.startFilesystemInputInspection([serverFileLocation({ rootId: 'host', relativePath: 'source' })]),
        ).rejects.toThrow('did not return a job');
    });

    it('submits the reviewed revision and maps raw host and uploaded inputs in one job', async () => {
        const { client, sessions } = setup();
        const invoke = vi.spyOn(client, 'invoke').mockResolvedValue({
            jobId: 'job-files',
            operationId: 'images.filesystem.edit',
            state: 'QUEUED',
            latestSequence: 0,
            progress: null,
            result: null,
            error: null,
        });
        const edits = [
            { kind: 'CREATE_DIRECTORY', parentEntryId: 'root', relativePath: ['New'] },
            {
                kind: 'PUT_FILE',
                parentEntryId: 'root',
                relativePath: ['New', 'file'],
                source: serverFileLocation({ rootId: 'host', relativePath: 'source' }),
                expectedSource: { revision: 'reviewed-host', sizeBytes: 3, sha256: 'a'.repeat(64) },
                conflict: 'SKIP',
            },
            {
                kind: 'PUT_FILE',
                parentEntryId: 'root',
                relativePath: ['uploaded'],
                source: clientUploadLocation({ uploadId: 'upload' }, 'FILE', 'uploaded'),
                expectedSource: { revision: 'upload:upload', sizeBytes: 0, sha256: 'b'.repeat(64) },
                conflict: 'REPLACE',
            },
            { kind: 'DELETE', entryId: 'old', recursive: true },
        ] satisfies import('./filesystem').FilesystemEdit[];
        const result = await sessions.startFilesystemEdits(1, 6, edits);
        expect(result).toMatchObject({ kind: 'images.filesystem.edit', status: 'queued' });
        expect(invoke).toHaveBeenCalledWith(
            'images.filesystem.edit',
            {
                imageId: 'remote-image',
                expectedRevision: 6,
                acknowledgeDeviceRelationships: true,
                edits: [
                    edits[0],
                    { ...edits[1], source: { fileRef: { rootId: 'host', relativePath: 'source' } } },
                    { ...edits[2], source: { uploadRef: { uploadId: 'upload' } } },
                    edits[3],
                ],
            },
            { idempotencyKey: expect.any(String) },
        );
    });

    it('rejects an empty edit set and a non-job execution response', async () => {
        const { client, sessions } = setup();
        const invoke = vi.spyOn(client, 'invoke').mockResolvedValue({});
        await expect(sessions.startFilesystemEdits(1, 6, [])).rejects.toThrow('At least one');
        expect(invoke).not.toHaveBeenCalled();
        await expect(
            sessions.startFilesystemEdits(1, 6, [{ kind: 'DELETE', entryId: 'old', recursive: false }]),
        ).rejects.toThrow('did not return a job');
    });

    it('retains per-root capabilities without inferring them from the device view', async () => {
        const { client, sessions } = setup();
        const capabilities = [
            {
                rootId: 'root',
                createDirectory: true,
                putFile: true,
                deleteEntry: true,
                maximumNameBytes: 23,
                namePattern: '^[ -~]{1,23}$',
                nameHint: 'Printable ASCII',
                supportedImports: [],
            },
        ];
        vi.spyOn(client, 'request').mockResolvedValue({
            revision: 9,
            available: true,
            filesystemName: 'Yamaha SFS',
            deviceView: null,
            items: [],
            totalCount: 0,
            rootCapabilities: capabilities,
        });
        const result = await sessions.filesystem(1);
        expect(result.rootCapabilities).toEqual(capabilities);
        expect(result.deviceView).toBeNull();
    });
});
