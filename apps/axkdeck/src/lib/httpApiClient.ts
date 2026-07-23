import type { components } from './generated/axklibApiV1';
import type { ClientUploadSource } from './clientUploadSource';

export interface AxklibApiConnection {
    baseUrl: string;
    bearerToken: string;
}

export type OperationCapability = components['schemas']['OperationCapability'];
export type ApiCapabilities = components['schemas']['Capabilities'];
export type ApiLimits = components['schemas']['ApiLimits'];
export type ApiJobSnapshot = components['schemas']['Job'];
export type ApiJobEvent = components['schemas']['JobEvent'];

import type { DirectoryListing, DirectoryRef, FileRef, SandboxRoot, UploadKind, UploadRef } from './storageLocations';

export interface EntryMetadata extends FileRef {
    kind: 'FILE' | 'DIRECTORY';
    size: number | null;
    writable: boolean;
}

export interface UploadSnapshot {
    uploadId: string;
    filename: string;
    kind: UploadKind;
    mediaType: string;
    declaredSize: number;
    receivedSize: number;
    state: 'RECEIVING' | 'READY';
    expiresInSeconds: number;
}

export interface DownloadArchiveSnapshot {
    archiveId: string;
    filename: string;
    sizeBytes: number;
    entryCount: number;
    expiresInSeconds: number;
    contentPath: string;
}

export interface WorkspaceInfo {
    id: string;
    displayName: string;
    path: string;
    writable: boolean;
    effectiveWritable: boolean;
    status: 'AVAILABLE' | 'READ_ONLY' | 'MISSING' | 'NOT_DIRECTORY' | 'PERMISSION_DENIED';
    issue: string | null;
}

export interface WorkspaceSnapshot {
    state: 'READY' | 'NO_AVAILABLE_WORKSPACE' | 'CONFIGURATION_ERROR';
    revision: number;
    workspaces: WorkspaceInfo[];
    configurationIssue: string | null;
}

export interface HostDirectoryRoot {
    name: string;
    path: string;
}
export interface HostDirectoryListing {
    path: string;
    parentPath: string | null;
    entries: HostDirectoryRoot[];
    nextCursor: string | null;
}

interface ApiEnvelope<T> {
    data: T;
    meta?: { requestId?: string };
}

interface ApiErrorEnvelope {
    error?: {
        code?: string;
        message?: string;
        context?: unknown;
        requestId?: string;
        retryable?: boolean;
    };
}

export class AxklibApiError extends Error {
    readonly code: string;
    readonly status: number;
    readonly requestId?: string;
    readonly context?: unknown;
    readonly retryable: boolean;

    constructor(
        code: string,
        message: string,
        status: number,
        requestId?: string,
        context?: unknown,
        retryable = false,
    ) {
        super(message);
        this.name = 'AxklibApiError';
        this.code = code;
        this.status = status;
        this.requestId = requestId;
        this.context = context;
        this.retryable = retryable;
    }
}

export type JobEventListener = (event: ApiJobEvent) => void;

export interface EventConnection {
    opened: Promise<void>;
    close(): void;
}

function isJobProgress(value: unknown): boolean {
    if (typeof value !== 'object' || value === null || Array.isArray(value)) return false;
    const progress = value as Record<string, unknown>;
    return (
        Object.keys(progress).every((key) => ['phase', 'completed', 'total', 'message'].includes(key)) &&
        typeof progress.phase === 'string' &&
        Number.isSafeInteger(progress.completed) &&
        Number(progress.completed) >= 0 &&
        (progress.total === null || (Number.isSafeInteger(progress.total) && Number(progress.total) >= 0)) &&
        typeof progress.message === 'string'
    );
}

function isJobEvent(value: unknown): value is ApiJobEvent {
    if (typeof value !== 'object' || value === null || Array.isArray(value)) return false;
    const event = value as Record<string, unknown>;
    return (
        Object.keys(event).every((key) =>
            [
                'schemaVersion',
                'eventId',
                'sequence',
                'jobId',
                'operationId',
                'type',
                'timestampUnixMs',
                'state',
                'progress',
                'jobUrl',
            ].includes(key),
        ) &&
        event.schemaVersion === '1' &&
        typeof event.eventId === 'string' &&
        typeof event.jobId === 'string' &&
        typeof event.jobUrl === 'string' &&
        typeof event.operationId === 'string' &&
        typeof event.type === 'string' &&
        Number.isSafeInteger(event.sequence) &&
        Number(event.sequence) > 0 &&
        Number.isSafeInteger(event.timestampUnixMs) &&
        Number(event.timestampUnixMs) >= 0 &&
        ['QUEUED', 'RUNNING', 'COMPLETED', 'FAILED', 'CANCELLED'].includes(String(event.state)) &&
        (event.progress === null || isJobProgress(event.progress))
    );
}

export class AxklibHttpApiClient {
    private readonly baseUrl: string;
    private readonly bearerToken: string;
    private capabilities?: ApiCapabilities;
    private operations = new Map<string, OperationCapability>();
    private routeCounts = new Map<string, number>();

    constructor(connection: AxklibApiConnection) {
        this.baseUrl = connection.baseUrl.replace(/\/+$/, '');
        this.bearerToken = connection.bearerToken;
    }

    async discover(): Promise<ApiCapabilities> {
        const capabilities = await this.request<ApiCapabilities>('GET', '/system/capabilities');
        if (capabilities.apiVersion !== 'v1') {
            throw new AxklibApiError(
                'unsupported_api_version',
                `axklib-server API ${capabilities.apiVersion} is not supported`,
                0,
            );
        }

        const operations = new Map<string, OperationCapability>();
        const routeCounts = new Map<string, number>();
        for (const operation of capabilities.operations) {
            if (operations.has(operation.id)) {
                throw new AxklibApiError('invalid_capabilities', `duplicate operation ${operation.id}`, 0);
            }
            operations.set(operation.id, operation);
            const routeKey = `${operation.method} ${operation.route}`;
            routeCounts.set(routeKey, (routeCounts.get(routeKey) ?? 0) + 1);
        }
        this.capabilities = capabilities;
        this.operations = operations;
        this.routeCounts = routeCounts;
        return capabilities;
    }

    async serverLimits(): Promise<ApiCapabilities['limits']> {
        if (!this.capabilities) await this.discover();
        return this.capabilities!.limits;
    }

    async serverCapabilities(): Promise<ApiCapabilities> {
        if (!this.capabilities) await this.discover();
        return this.capabilities!;
    }

    async invoke<Result>(
        operationId: string,
        input: Record<string, unknown> = {},
        options: { idempotencyKey?: string } = {},
    ): Promise<Result | ApiJobSnapshot> {
        if (!this.capabilities) await this.discover();
        const operation = this.operations.get(operationId);
        if (!operation || !operation.implemented) {
            throw new AxklibApiError('operation_unavailable', `operation ${operationId} is unavailable`, 0);
        }

        const headers: Record<string, string> = {};
        if (options.idempotencyKey) headers['Idempotency-Key'] = options.idempotencyKey;

        const routeKey = `${operation.method} ${operation.route}`;
        if (operation.method === 'GET') {
            if (Object.keys(input).length > 0) {
                throw new AxklibApiError(
                    'invalid_operation_input',
                    `GET operation ${operationId} does not accept request fields`,
                    0,
                );
            }
            if (this.routeCounts.get(routeKey) !== 1) {
                throw new AxklibApiError(
                    'invalid_capabilities',
                    `GET operation ${operationId} does not have a unique route`,
                    0,
                );
            }
            return this.request<Result | ApiJobSnapshot>(
                operation.method,
                this.operationPath(operation.route),
                undefined,
                headers,
            );
        }

        const requestInput = this.routeCounts.get(routeKey) === 1 ? input : { ...input, operationId };
        return this.request<Result | ApiJobSnapshot>(
            operation.method,
            this.operationPath(operation.route),
            requestInput,
            headers,
        );
    }

    async roots(): Promise<SandboxRoot[]> {
        const result = await this.request<{ roots: SandboxRoot[] }>('GET', '/roots');
        return result.roots;
    }

    workspaces(): Promise<WorkspaceSnapshot> {
        return this.request('GET', '/workspaces');
    }

    createWorkspace(input: {
        displayName: string;
        path: string;
        writable: boolean;
        revision: number;
    }): Promise<WorkspaceInfo> {
        return this.request('POST', '/workspaces', input);
    }

    updateWorkspace(
        id: string,
        input: { displayName?: string; path?: string; writable?: boolean; revision: number },
    ): Promise<WorkspaceInfo> {
        return this.request('PATCH', `/workspaces/${encodeURIComponent(id)}`, input);
    }

    async removeWorkspace(id: string, revision: number): Promise<void> {
        await this.request('DELETE', `/workspaces/${encodeURIComponent(id)}`, { revision });
    }

    resetWorkspaceStore(): Promise<{ archivedPath: string | null; revision: number }> {
        return this.request('POST', '/workspaces/recovery/reset', {});
    }

    async hostDirectoryRoots(): Promise<HostDirectoryRoot[]> {
        const result = await this.request<{ roots: HostDirectoryRoot[] }>('GET', '/host-directories/roots');
        return result.roots;
    }

    hostDirectoryListing(path: string, cursor: string | null = null): Promise<HostDirectoryListing> {
        return this.request('POST', '/host-directories/list', { path, cursor, limit: 200 });
    }

    listDirectory(
        directory: DirectoryRef,
        options: { limit?: number; cursor?: string } = {},
    ): Promise<DirectoryListing> {
        return this.request('POST', '/files/list', {
            directory,
            limit: options.limit ?? 200,
            cursor: options.cursor ?? null,
        });
    }

    metadata(reference: FileRef): Promise<EntryMetadata> {
        return this.request('POST', '/files/metadata', reference);
    }

    createDirectory(parent: DirectoryRef, name: string): Promise<EntryMetadata> {
        return this.request('POST', '/filesystem/directories', { parent, name });
    }

    renameEntry(entry: FileRef, name: string): Promise<EntryMetadata> {
        return this.request('PATCH', '/filesystem/entries', { entry, name });
    }

    async deleteEntry(entry: FileRef): Promise<void> {
        const query = new URLSearchParams({ rootId: entry.rootId, relativePath: entry.relativePath });
        await this.request('DELETE', `/filesystem/entries?${query}`);
    }

    createUpload(input: {
        filename: string;
        kind: UploadKind;
        mediaType: string;
        size: number;
        sha256?: string;
    }): Promise<UploadSnapshot> {
        return this.request('POST', '/uploads', input);
    }

    uploadStatus(reference: UploadRef): Promise<UploadSnapshot> {
        return this.request('GET', `/uploads/${encodeURIComponent(reference.uploadId)}`);
    }

    completeUpload(reference: UploadRef): Promise<UploadSnapshot> {
        return this.request('POST', `/uploads/${encodeURIComponent(reference.uploadId)}/complete`, {});
    }

    async deleteUpload(reference: UploadRef): Promise<void> {
        const response = await this.fetchResponse('DELETE', `/uploads/${encodeURIComponent(reference.uploadId)}`);
        if (!response.ok) await this.throwResponseError(response);
    }

    async uploadBlob(
        blob: Blob,
        input: { filename: string; kind: UploadKind; mediaType?: string; sha256?: string },
        options: { chunkBytes?: number; onProgress?: (sent: number, total: number) => void; signal?: AbortSignal } = {},
    ): Promise<UploadSnapshot> {
        const source: ClientUploadSource = {
            name: input.filename,
            type: input.mediaType ?? blob.type,
            size: blob.size,
            readChunk: async (start, end) => blob.slice(start, end),
        };
        return this.uploadSource(source, input, options);
    }

    async uploadSource(
        source: ClientUploadSource,
        input: { filename: string; kind: UploadKind; mediaType?: string; sha256?: string },
        options: { chunkBytes?: number; onProgress?: (sent: number, total: number) => void; signal?: AbortSignal } = {},
    ): Promise<UploadSnapshot> {
        options.signal?.throwIfAborted();
        const limits = options.chunkBytes === undefined ? await this.serverLimits() : undefined;
        if (limits && source.size > limits.maximumUploadBytes) {
            throw new AxklibApiError('upload_too_large', 'File exceeds the server upload limit', 413);
        }
        const created = await this.createUpload({
            filename: input.filename,
            kind: input.kind,
            mediaType: input.mediaType ?? (source.type || 'application/octet-stream'),
            size: source.size,
            sha256: input.sha256,
        });
        const reference = { uploadId: created.uploadId };
        const chunkBytes = options.chunkBytes ?? limits!.maximumUploadChunkBytes;
        if (!Number.isSafeInteger(chunkBytes) || chunkBytes <= 0) {
            await this.deleteUpload(reference).catch(() => undefined);
            throw new AxklibApiError('invalid_upload_chunk_size', 'upload chunk size must be a positive integer', 0);
        }
        try {
            let offset = created.receivedSize;
            while (offset < source.size) {
                options.signal?.throwIfAborted();
                const chunk = await source.readChunk(offset, Math.min(source.size, offset + chunkBytes));
                if (chunk.size === 0 || chunk.size > source.size - offset) {
                    throw new AxklibApiError('invalid_upload_source', 'upload source returned an invalid chunk', 0);
                }
                const response = await this.fetchResponse(
                    'PUT',
                    `/uploads/${encodeURIComponent(reference.uploadId)}`,
                    chunk,
                    { 'Content-Type': 'application/octet-stream', 'Upload-Offset': String(offset) },
                    options.signal,
                );
                const snapshot = await this.readEnvelope<UploadSnapshot>(response);
                if (snapshot.receivedSize <= offset || snapshot.receivedSize > source.size) {
                    throw new AxklibApiError(
                        'invalid_upload_offset',
                        'server returned an invalid upload offset',
                        response.status,
                    );
                }
                offset = snapshot.receivedSize;
                options.onProgress?.(offset, source.size);
            }
            return await this.completeUpload(reference);
        } catch (error) {
            await this.deleteUpload(reference).catch(() => undefined);
            throw error;
        }
    }

    materializeUpload(reference: UploadRef, destination: FileRef, overwrite = false): Promise<{ file: FileRef }> {
        return this.request('POST', `/uploads/${encodeURIComponent(reference.uploadId)}/materialize`, {
            destination,
            overwrite,
        });
    }

    async openDownload(reference: FileRef, range?: { start: number; end?: number }): Promise<Response> {
        const query = new URLSearchParams({ rootId: reference.rootId, relativePath: reference.relativePath });
        const headers: Record<string, string> = {};
        if (range) headers.Range = `bytes=${range.start}-${range.end ?? ''}`;
        const response = await this.fetchResponse('GET', `/files/content?${query}`, undefined, headers);
        if (!response.ok) await this.throwResponseError(response);
        return response;
    }

    async openAuditionAudio(auditionId: string, start: number, end: number, signal?: AbortSignal): Promise<Response> {
        const response = await this.fetchResponse(
            'GET',
            `/auditions/${encodeURIComponent(auditionId)}/audio`,
            undefined,
            { Range: `bytes=${start}-${end}` },
            signal,
        );
        if (!response.ok) await this.throwResponseError(response);
        return response;
    }

    async deleteAudition(auditionId: string): Promise<void> {
        const response = await this.fetchResponse('DELETE', `/auditions/${encodeURIComponent(auditionId)}`);
        if (!response.ok) await this.throwResponseError(response);
    }

    createDirectoryArchive(directory: DirectoryRef): Promise<DownloadArchiveSnapshot> {
        return this.request('POST', '/files/archive', { directory });
    }

    async openDirectoryArchive(snapshot: DownloadArchiveSnapshot): Promise<Response> {
        const response = await this.fetchResponse('GET', this.operationPath(snapshot.contentPath));
        if (!response.ok) await this.throwResponseError(response);
        return response;
    }

    async deleteDirectoryArchive(snapshot: DownloadArchiveSnapshot): Promise<void> {
        const response = await this.fetchResponse('DELETE', this.operationPath(snapshot.contentPath));
        if (!response.ok) await this.throwResponseError(response);
    }

    replayJobEvents(jobId: string, afterSequence: number): Promise<{ events: ApiJobEvent[] }> {
        const query = new URLSearchParams({ afterSequence: String(afterSequence) });
        return this.request('GET', `/jobs/${encodeURIComponent(jobId)}/events?${query}`);
    }

    request<Result>(
        method: 'GET' | 'POST' | 'PUT' | 'PATCH' | 'DELETE',
        path: string,
        body?: unknown,
        headers: Record<string, string> = {},
    ): Promise<Result> {
        return this.fetchJson(method, path, body, headers);
    }

    async connectEvents(
        listener: JobEventListener,
        onDisconnect?: (event: CloseEvent) => void,
    ): Promise<EventConnection> {
        const ticket = await this.request<{
            ticket: string;
            websocketUrl: string;
            subprotocol: string;
        }>('POST', '/event-tickets', {});
        const websocketUrl = new URL(`${this.baseUrl}${this.operationPath(ticket.websocketUrl)}`);
        websocketUrl.protocol = websocketUrl.protocol === 'https:' ? 'wss:' : 'ws:';
        websocketUrl.searchParams.set('ticket', ticket.ticket);
        const socket = new WebSocket(websocketUrl, ticket.subprotocol);
        const opened = new Promise<void>((resolve, reject) => {
            socket.addEventListener('open', () => resolve(), { once: true });
            socket.addEventListener(
                'error',
                () =>
                    reject(
                        new AxklibApiError(
                            'websocket_connection_failed',
                            'Could not connect to axklib-server job events',
                            0,
                        ),
                    ),
                { once: true },
            );
        });
        socket.addEventListener('message', (event) => {
            try {
                if (typeof event.data !== 'string') throw new Error('binary job event');
                const value: unknown = JSON.parse(event.data);
                if (!isJobEvent(value)) throw new Error('invalid job event');
                listener(value);
            } catch {
                socket.close(1002, 'Invalid job event');
            }
        });
        if (onDisconnect) socket.addEventListener('close', onDisconnect);
        return { opened, close: () => socket.close() };
    }

    private async fetchJson<Result>(
        method: 'GET' | 'POST' | 'PUT' | 'PATCH' | 'DELETE',
        path: string,
        body?: unknown,
        headers: Record<string, string> = {},
    ): Promise<Result> {
        const response = await this.fetchResponse(method, path, body === undefined ? undefined : JSON.stringify(body), {
            ...(body === undefined ? {} : { 'Content-Type': 'application/json' }),
            ...headers,
        });
        return this.readEnvelope<Result>(response);
    }

    private fetchResponse(
        method: 'GET' | 'POST' | 'PUT' | 'PATCH' | 'DELETE',
        path: string,
        body?: BodyInit,
        headers: Record<string, string> = {},
        signal?: AbortSignal,
    ): Promise<Response> {
        return fetch(`${this.baseUrl}${path}`, {
            method,
            headers: {
                Authorization: `Bearer ${this.bearerToken}`,
                ...headers,
            },
            body,
            signal,
        });
    }

    private async readEnvelope<Result>(response: Response): Promise<Result> {
        const body = await response.text();
        if (response.ok && body.length === 0) return undefined as Result;
        let document: ApiEnvelope<Result> & ApiErrorEnvelope;
        try {
            document = JSON.parse(body) as ApiEnvelope<Result> & ApiErrorEnvelope;
        } catch {
            throw new AxklibApiError(
                'invalid_response',
                `axklib-server returned an invalid JSON response with HTTP ${response.status}`,
                response.status,
            );
        }
        if (!response.ok) this.throwDocumentError(document, response.status);
        return document.data;
    }

    private async throwResponseError(response: Response): Promise<never> {
        let document: ApiErrorEnvelope = {};
        try {
            document = (await response.json()) as ApiErrorEnvelope;
        } catch {
            // A proxy may replace the JSON body. Preserve the status without exposing it as a parsing failure.
        }
        return this.throwDocumentError(document, response.status);
    }

    private throwDocumentError(document: ApiErrorEnvelope, status: number): never {
        throw new AxklibApiError(
            document.error?.code ?? 'http_error',
            document.error?.message ?? `axklib-server returned HTTP ${status}`,
            status,
            document.error?.requestId,
            document.error?.context,
            document.error?.retryable ?? false,
        );
    }

    private operationPath(route: string): string {
        return route.startsWith('/api/v1/') ? route.slice('/api/v1'.length) : route;
    }
}
