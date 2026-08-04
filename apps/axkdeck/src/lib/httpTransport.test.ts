import { afterEach, describe, expect, it, vi } from 'vitest';

import { HttpImageTransport } from './httpTransport';
import {
    axkObjectDirectoryLocation,
    clientUploadLocation,
    serverDirectoryLocation,
    serverFileLocation,
} from './storageLocations';
import type { FileLocation } from './storageLocations';

const serverFile = (relativePath: string) => serverFileLocation({ rootId: 'workspace', relativePath });

function json(data: unknown, status = 200): Response {
    return new Response(JSON.stringify({ data, meta: { requestId: 'request-test' } }), {
        status,
        headers: { 'Content-Type': 'application/json' },
    });
}

class OpeningWebSocket {
    static readonly instances: OpeningWebSocket[] = [];
    readonly url: string;
    readonly protocol: string;
    private readonly listeners = new Map<string, EventListener[]>();

    constructor(url: string | URL, protocol: string | string[]) {
        this.url = String(url);
        this.protocol = Array.isArray(protocol) ? (protocol[0] ?? '') : protocol;
        OpeningWebSocket.instances.push(this);
        setTimeout(() => this.dispatch('open', new Event('open')), 0);
    }

    addEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
        const callback = typeof listener === 'function' ? listener : listener.handleEvent.bind(listener);
        this.listeners.set(type, [...(this.listeners.get(type) ?? []), callback]);
    }

    close(): void {
        this.dispatch('close', new CloseEvent('close'));
    }

    private dispatch(type: string, event: Event): void {
        for (const listener of this.listeners.get(type) ?? []) listener(event);
    }
}

class FlappingWebSocket {
    static instances = 0;
    private readonly listeners = new Map<string, EventListener[]>();

    constructor() {
        FlappingWebSocket.instances += 1;
        setTimeout(() => this.dispatch('open', new Event('open')), 0);
        setTimeout(() => this.dispatch('close', new CloseEvent('close')), 1);
    }

    addEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
        const callback = typeof listener === 'function' ? listener : listener.handleEvent.bind(listener);
        this.listeners.set(type, [...(this.listeners.get(type) ?? []), callback]);
    }

    close(): void {}

    private dispatch(type: string, event: Event): void {
        for (const listener of this.listeners.get(type) ?? []) listener(event);
    }
}

class RejectingWebSocket {
    static instances = 0;
    private readonly listeners = new Map<string, EventListener[]>();

    constructor() {
        RejectingWebSocket.instances += 1;
        setTimeout(() => this.dispatch('error', new Event('error')), 0);
    }

    addEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
        const callback = typeof listener === 'function' ? listener : listener.handleEvent.bind(listener);
        this.listeners.set(type, [...(this.listeners.get(type) ?? []), callback]);
    }

    close(): void {}

    private dispatch(type: string, event: Event): void {
        for (const listener of this.listeners.get(type) ?? []) listener(event);
    }
}

describe('HttpImageTransport', () => {
    afterEach(() => {
        vi.useRealTimers();
        vi.unstubAllGlobals();
    });

    it('opens and lazily pages one Crow image session without reconstructing the full tree', async () => {
        const requests: string[] = [];
        vi.stubGlobal(
            'fetch',
            vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
                const url = new URL(String(input));
                requests.push(`${init?.method} ${url.pathname}${url.search}`);
                expect((init?.headers as Record<string, string>).Authorization).toBe('Bearer secret');
                if (url.pathname.endsWith('/images') && init?.method === 'POST') {
                    expect(JSON.parse(String(init.body))).toEqual({
                        source: {
                            kind: 'FILE',
                            file: { rootId: 'workspace', relativePath: 'images/test.hds' },
                        },
                    });
                    return json(
                        {
                            imageId: 'image-remote',
                            source: {
                                kind: 'FILE',
                                file: { rootId: 'workspace', relativePath: 'images/test.hds' },
                            },
                            companionSources: [],
                            floppySet: null,
                            format: 'sfs',
                            rootCount: 1,
                            objectCount: 2,
                            relationshipCount: 1,
                            validation: { valid: true, infoCount: 0, warningCount: 0, errorCount: 0 },
                        },
                        201,
                    );
                }
                if (url.pathname.endsWith('/images/image-remote/content') && !url.searchParams.has('parentId')) {
                    return json({
                        items: [
                            {
                                id: 'partition-1',
                                parentId: null,
                                kind: 'partition',
                                name: 'PARTITION 1',
                                displayName: 'partition 0: PARTITION 1',
                                partitionIndex: 0,
                                childCount: 1,
                                scopeRole: 'CONTAINED',
                            },
                        ],
                        totalCount: 1,
                        nextCursor: null,
                    });
                }
                if (url.pathname.endsWith('/images/image-remote/content')) {
                    expect(url.searchParams.get('parentId')).toBe('partition-1');
                    return json({
                        items: [
                            {
                                id: 'volume-1',
                                parentId: 'partition-1',
                                kind: 'volume',
                                name: 'drumloops',
                                displayName: 'drumloops',
                                partitionIndex: 0,
                                childCount: 0,
                                scopeRole: 'CONTAINED',
                            },
                        ],
                        totalCount: 1,
                        nextCursor: null,
                    });
                }
                if (url.pathname.endsWith('/images/image-remote/objects') && !url.searchParams.has('cursor')) {
                    expect(url.searchParams.get('type')).toBe('SMPL');
                    expect(url.searchParams.get('scopeId')).toBe('volume-1');
                    return json({
                        items: [
                            {
                                id: 'object-1',
                                type: 'SMPL',
                                name: 'Tone',
                                partitionIndex: 0,
                                partitionName: 'Partition 0',
                                volumeName: 'Volume',
                                categoryName: 'SMPL',
                                sizeBytes: 88_064,
                                waveform: {
                                    sampleRate: 44100,
                                    sampleWidthBytes: 2,
                                    rootKey: 60,
                                    frameCount: 100,
                                },
                            },
                        ],
                        totalCount: 2,
                        nextCursor: 'objects-next',
                    });
                }
                if (url.pathname.endsWith('/images/image-remote/objects')) {
                    expect(url.searchParams.get('cursor')).toBe('objects-next');
                    return json({ items: [], totalCount: 2, nextCursor: null });
                }
                if (url.pathname.endsWith('/images/image-remote') && init?.method === 'DELETE') {
                    return json({ closed: true });
                }
                throw new Error(`unexpected request ${init?.method} ${url}`);
            }),
        );

        const transport = new HttpImageTransport({
            baseUrl: 'http://127.0.0.1:4000/api/v1',
            bearerToken: 'secret',
        });
        const opened = await transport.openImage(serverFile('images/test.hds'));
        expect(opened).toMatchObject({ sessionId: 1, objectTotalCount: 0, initialVolume: { id: 'volume-1' } });
        expect(opened.tree[0]?.children?.[0]).toMatchObject({
            id: 'partition-1',
            name: 'PARTITION 1',
            childCount: 1,
            partitionIndex: 0,
            scopeRole: 'CONTAINED',
        });
        expect(opened.tree[0]?.children?.[0]?.children?.[0]).toMatchObject({
            id: 'volume-1',
            name: 'drumloops',
            volumeName: 'drumloops',
            scopeRole: 'CONTAINED',
        });
        expect(opened.objects).toEqual([]);
        await expect(
            transport.objectPage(1, 0, 64, { objectType: 'SMPL', scopeId: 'volume-1' }),
        ).resolves.toMatchObject({
            objects: [
                {
                    key: 'object-1',
                    sampleRate: 44100,
                    rootKey: 60,
                    storedSizeBytes: 88_064,
                },
            ],
            totalCount: 2,
        });
        await expect(transport.contentChildren(1, 'partition-1', 0, 64)).resolves.toMatchObject({
            items: [{ id: 'volume-1', partitionIndex: 0 }],
            totalCount: 1,
        });
        await transport.closeImage(1);
        expect(requests).toHaveLength(6);
    });

    it('opens an AXK object directory through the explicit directory source contract', async () => {
        vi.stubGlobal(
            'fetch',
            vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
                const url = new URL(String(input));
                if (url.pathname.endsWith('/images') && init?.method === 'POST') {
                    expect(JSON.parse(String(init.body))).toEqual({
                        source: {
                            kind: 'AXK_OBJECT_DIRECTORY',
                            directory: { rootId: 'workspace', relativePath: 'unpacked/volume' },
                        },
                    });
                    return json(
                        {
                            imageId: 'directory-image',
                            revision: 1,
                            source: {
                                kind: 'AXK_OBJECT_DIRECTORY',
                                directory: { rootId: 'workspace', relativePath: 'unpacked/volume' },
                            },
                            companionSources: [],
                            floppySet: null,
                            format: 'axk-object-directory',
                            availableOperations: ['images.content', 'images.objects', 'images.package.export'],
                            rootCount: 0,
                            objectCount: 0,
                            relationshipCount: 0,
                            validation: { valid: true, infoCount: 0, warningCount: 0, errorCount: 0 },
                        },
                        201,
                    );
                }
                if (url.pathname.endsWith('/images/directory-image/content')) {
                    return json({ items: [], totalCount: 0, nextCursor: null });
                }
                throw new Error(`unexpected request ${init?.method} ${url}`);
            }),
        );

        const transport = new HttpImageTransport({ baseUrl: 'http://localhost/api/v1', bearerToken: 'secret' });
        const opened = await transport.openImage(
            axkObjectDirectoryLocation({ rootId: 'workspace', relativePath: 'unpacked/volume' }),
        );
        expect(opened).toMatchObject({
            sessionId: 1,
            companionSources: [],
            floppySet: null,
            packageImportAvailable: false,
            packageExportAvailable: true,
        });
    });

    it('attaches selected companion directories to the same local image session', async () => {
        const requests: { path: string; body: unknown }[] = [];
        vi.stubGlobal(
            'fetch',
            vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
                const url = new URL(String(input));
                if (url.pathname.endsWith('/images') && init?.method === 'POST') {
                    return json(
                        {
                            imageId: 'directory-image',
                            revision: 1,
                            source: {
                                kind: 'AXK_OBJECT_DIRECTORY',
                                directory: { rootId: 'workspace', relativePath: 'set/DISK2' },
                            },
                            companionSources: [],
                            floppySet: null,
                            format: 'axk-object-directory',
                            availableOperations: ['images.content'],
                            rootCount: 0,
                            objectCount: 1,
                            relationshipCount: 0,
                            validation: { valid: true, infoCount: 0, warningCount: 0, errorCount: 0 },
                        },
                        201,
                    );
                }
                if (url.pathname.endsWith('/images/directory-image/companions')) {
                    requests.push({ path: url.pathname, body: JSON.parse(String(init?.body)) });
                    return json({
                        imageId: 'directory-image',
                        revision: 2,
                        source: {
                            kind: 'AXK_OBJECT_DIRECTORY',
                            directory: { rootId: 'workspace', relativePath: 'set/DISK2' },
                        },
                        companionSources: [
                            {
                                kind: 'AXK_OBJECT_DIRECTORY',
                                directory: { rootId: 'workspace', relativePath: 'set/DISK1' },
                            },
                        ],
                        floppySet: null,
                        format: 'axk-object-directory',
                        availableOperations: ['images.content'],
                        rootCount: 0,
                        objectCount: 1,
                        relationshipCount: 0,
                        validation: { valid: true, infoCount: 0, warningCount: 0, errorCount: 0 },
                    });
                }
                if (url.pathname.endsWith('/images/directory-image/content')) {
                    return json({ items: [], totalCount: 0, nextCursor: null });
                }
                throw new Error(`unexpected request ${init?.method} ${url}`);
            }),
        );

        const transport = new HttpImageTransport({ baseUrl: 'http://localhost/api/v1', bearerToken: 'secret' });
        const opened = await transport.openImage(
            axkObjectDirectoryLocation({ rootId: 'workspace', relativePath: 'set/DISK2' }),
        );
        const attached = await transport.attachCompanions(opened.sessionId, {
            kind: 'sources',
            sources: [axkObjectDirectoryLocation({ rootId: 'workspace', relativePath: 'set/DISK1' })],
        });

        expect(attached).toMatchObject({
            sessionId: opened.sessionId,
            companionSources: [
                {
                    kind: 'axk-object-directory',
                    reference: { rootId: 'workspace', relativePath: 'set/DISK1' },
                    displayName: 'set/DISK1',
                },
            ],
            floppySet: null,
        });
        expect(requests).toEqual([
            {
                path: '/api/v1/images/directory-image/companions',
                body: {
                    expectedRevision: 1,
                    selection: {
                        kind: 'SOURCES',
                        sources: [
                            {
                                kind: 'AXK_OBJECT_DIRECTORY',
                                directory: { rootId: 'workspace', relativePath: 'set/DISK1' },
                            },
                        ],
                    },
                },
            },
        ]);
    });

    it('assembles complete audition audio with the server-advertised range limit', async () => {
        const requestedRanges: string[] = [];
        vi.stubGlobal(
            'fetch',
            vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
                const url = new URL(String(input));
                if (url.pathname.endsWith('/system/capabilities')) {
                    return json({
                        apiVersion: 'v1',
                        operations: [],
                        limits: { maximumDownloadRangeBytes: 4 },
                    });
                }
                if (url.pathname.endsWith('/auditions/audition-1/content')) {
                    const range = (init?.headers as Record<string, string>).Range;
                    requestedRanges.push(range);
                    const match = /^bytes=(\d+)-(\d+)$/.exec(range);
                    if (!match) throw new Error(`invalid range ${range}`);
                    const start = Number(match[1]);
                    const end = Number(match[2]);
                    return new Response(
                        Uint8Array.from({ length: end - start + 1 }, (_, index) => start + index),
                        {
                            status: 206,
                            headers: { 'Content-Range': `bytes ${start}-${end}/10` },
                        },
                    );
                }
                throw new Error(`unexpected request ${init?.method} ${url}`);
            }),
        );

        const transport = new HttpImageTransport({
            baseUrl: 'http://127.0.0.1:4000/api/v1',
            bearerToken: 'secret',
        });

        const audio = await transport.readAuditionContent('audition-1', 10);

        expect(Array.from(new Uint8Array(audio))).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
        expect(requestedRanges).toEqual(['bytes=0-3', 'bytes=4-7', 'bytes=8-9']);
    });

    it('assembles file downloads from ranges bound to one server revision', async () => {
        const requests: Array<{ method: string; range?: string; revision?: string }> = [];
        vi.stubGlobal(
            'fetch',
            vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
                const url = new URL(String(input));
                if (url.pathname.endsWith('/system/capabilities')) {
                    return json({
                        apiVersion: 'v1',
                        operations: [],
                        limits: { maximumDownloadRangeBytes: 4 },
                    });
                }
                if (url.pathname.endsWith('/files/content')) {
                    const headers = init?.headers as Record<string, string>;
                    requests.push({
                        method: String(init?.method),
                        range: headers.Range,
                        revision: headers['If-Match'],
                    });
                    if (init?.method === 'HEAD') {
                        return new Response(null, {
                            status: 200,
                            headers: { 'Content-Length': '10', ETag: '"revision-1"' },
                        });
                    }
                    const match = /^bytes=(\d+)-(\d+)$/.exec(headers.Range);
                    if (!match) throw new Error(`invalid range ${headers.Range}`);
                    const start = Number(match[1]);
                    const end = Number(match[2]);
                    return new Response(
                        Uint8Array.from({ length: end - start + 1 }, (_, index) => start + index),
                        { status: 206, headers: { ETag: '"revision-1"' } },
                    );
                }
                throw new Error(`unexpected request ${init?.method} ${url}`);
            }),
        );

        const transport = new HttpImageTransport({
            baseUrl: 'http://127.0.0.1:4000/api/v1',
            bearerToken: 'secret',
        });
        const result = await transport.downloadFile(serverFile('exports/tone.wav'));

        expect(result.filename).toBe('tone.wav');
        expect(Array.from(new Uint8Array(await result.blob.arrayBuffer()))).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
        expect(requests).toEqual([
            { method: 'HEAD', range: undefined, revision: undefined },
            { method: 'GET', range: 'bytes=0-3', revision: '"revision-1"' },
            { method: 'GET', range: 'bytes=4-7', revision: '"revision-1"' },
            { method: 'GET', range: 'bytes=8-9', revision: '"revision-1"' },
        ]);
    });

    it('rejects truncated audition ranges', async () => {
        vi.stubGlobal(
            'fetch',
            vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
                const url = new URL(String(input));
                if (url.pathname.endsWith('/system/capabilities')) {
                    return json({
                        apiVersion: 'v1',
                        operations: [],
                        limits: { maximumDownloadRangeBytes: 8 },
                    });
                }
                expect(init?.signal).toBeInstanceOf(AbortSignal);
                return new Response(Uint8Array.of(1, 2, 3), {
                    status: 206,
                    headers: { 'Content-Range': 'bytes 0-3/4' },
                });
            }),
        );

        const transport = new HttpImageTransport({
            baseUrl: 'http://127.0.0.1:4000/api/v1',
            bearerToken: 'secret',
        });

        await expect(transport.readAuditionContent('audition-1', 4)).rejects.toThrow('returned 3 bytes; expected 4');
    });

    it('forwards cancellation while an audition request is active', async () => {
        const controller = new AbortController();
        let requestSignal: AbortSignal | undefined;
        let notifyRequestStarted!: () => void;
        const requestStarted = new Promise<void>((resolve) => {
            notifyRequestStarted = resolve;
        });
        vi.stubGlobal(
            'fetch',
            vi.fn((input: RequestInfo | URL, init?: RequestInit) => {
                const url = new URL(String(input));
                if (url.pathname.endsWith('/system/capabilities')) {
                    return Promise.resolve(
                        json({
                            apiVersion: 'v1',
                            operations: [],
                            limits: { maximumDownloadRangeBytes: 8 },
                        }),
                    );
                }
                const effectiveSignal = init?.signal;
                if (!(effectiveSignal instanceof AbortSignal)) throw new Error('missing request cancellation signal');
                requestSignal = effectiveSignal;
                notifyRequestStarted();
                return new Promise<Response>((_resolve, reject) => {
                    effectiveSignal.addEventListener('abort', () => reject(effectiveSignal.reason), { once: true });
                });
            }),
        );

        const transport = new HttpImageTransport({
            baseUrl: 'http://127.0.0.1:4000/api/v1',
            bearerToken: 'secret',
        });
        const request = transport.readAuditionContent('audition-1', 4, controller.signal);
        await requestStarted;
        controller.abort(new DOMException('cancelled', 'AbortError'));

        await expect(request).rejects.toMatchObject({ name: 'AbortError' });
        expect(requestSignal?.aborted).toBe(true);
    });

    it('discovers create operations generically and applies the exact reserved plan token', async () => {
        OpeningWebSocket.instances.length = 0;
        vi.stubGlobal('WebSocket', OpeningWebSocket as unknown as typeof WebSocket);
        vi.stubGlobal(
            'fetch',
            vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
                const url = new URL(String(input));
                if (url.pathname.endsWith('/system/capabilities')) {
                    return json({
                        apiVersion: 'v1',
                        operations: [
                            {
                                id: 'create.plan',
                                method: 'POST',
                                route: '/api/v1/image-build-plans',
                                mode: 'request',
                                operationClass: 'read',
                                requiresIdempotency: false,
                                variant: null,
                                requestSchema: 'ImageBuildPlanRequest',
                                resultSchema: 'ImageBuildPlan',
                                implemented: true,
                            },
                            {
                                id: 'create.hds',
                                method: 'POST',
                                route: '/api/v1/image-builds',
                                mode: 'job',
                                operationClass: 'write',
                                requiresIdempotency: true,
                                variant: 'HDS',
                                requestSchema: 'ImageBuildRequest',
                                resultSchema: 'ImageBuildResult',
                                implemented: true,
                            },
                        ],
                    });
                }
                if (url.pathname.endsWith('/image-build-plans')) {
                    expect(JSON.parse(String(init?.body))).toMatchObject({
                        kind: 'HDS',
                        manifest: { fileRef: { rootId: 'workspace', relativePath: 'authoring/build.json' } },
                        output: { rootId: 'workspace', relativePath: 'images/new.hds' },
                    });
                    return json({
                        planToken: 'plan-exact',
                        kind: 'HDS',
                        summary: { sizeBytes: 1073741824, partitionCount: 1 },
                    });
                }
                if (url.pathname.endsWith('/image-builds')) {
                    expect((init?.headers as Record<string, string>)['Idempotency-Key']).toBeTruthy();
                    expect(JSON.parse(String(init?.body))).toEqual({ planToken: 'plan-exact' });
                    return json(
                        {
                            jobId: 'job-remote',
                            operationId: 'create.hds',
                            state: 'QUEUED',
                            latestSequence: 1,
                            progress: null,
                            result: null,
                            error: null,
                        },
                        202,
                    );
                }
                if (url.pathname.endsWith('/event-tickets')) {
                    return json(
                        {
                            ticket: 'ticket-one',
                            websocketUrl: '/api/v1/events',
                            subprotocol: 'axklib.events.v1',
                        },
                        201,
                    );
                }
                if (url.pathname.endsWith('/jobs/job-remote/events')) {
                    expect(url.searchParams.get('afterSequence')).toBe('0');
                    return json({
                        events: [
                            {
                                schemaVersion: '1',
                                eventId: 'event-1',
                                sequence: 1,
                                jobId: 'job-remote',
                                operationId: 'create.hds',
                                type: 'state',
                                timestampUnixMs: 1,
                                state: 'QUEUED',
                                progress: null,
                                jobUrl: '/api/v1/jobs/job-remote',
                            },
                        ],
                    });
                }
                if (url.pathname.endsWith('/jobs/job-remote')) {
                    return json({
                        jobId: 'job-remote',
                        operationId: 'create.hds',
                        state: 'COMPLETED',
                        latestSequence: 3,
                        progress: { phase: 'publish', completed: 1, total: 1, message: 'Published image' },
                        result: { output: { rootId: 'workspace', relativePath: 'images/new.hds' } },
                        error: null,
                    });
                }
                throw new Error(`unexpected request ${init?.method} ${url}`);
            }),
        );

        const transport = new HttpImageTransport({
            baseUrl: 'http://localhost/api/v1',
            bearerToken: 'secret',
        });
        const manifest = serverFile('authoring/build.json');
        const output = serverFile('images/new.hds');
        const plan = await transport.planCreate(manifest, output, false);
        expect(plan).toMatchObject({ planToken: 'plan-exact', partitionCount: 1, sizeBytes: 1073741824 });
        const job = await transport.startCreate(manifest, output, false);
        expect(job).toMatchObject({ jobId: 1, kind: 'create.hds', status: 'queued' });
        const updates: string[] = [];
        const completed = await transport.waitForJob(job.jobId, (update) => updates.push(update.status));
        expect(completed).toMatchObject({ status: 'completed', progress: { label: 'Published image' } });
        expect(updates).toEqual(['queued', 'completed']);
        expect(OpeningWebSocket.instances).toHaveLength(0);
    });

    it('uses typed hard-disk profiles and planning without constructing writer geometry', async () => {
        vi.stubGlobal(
            'fetch',
            vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
                const url = new URL(String(input));
                if (url.pathname.endsWith('/system/capabilities')) {
                    return json({
                        apiVersion: 'v1',
                        operations: [
                            {
                                id: 'create.hds.profiles',
                                method: 'GET',
                                route: '/api/v1/hard-disk-creation-profiles',
                                mode: 'request',
                                operationClass: 'read',
                                requiresIdempotency: false,
                                variant: null,
                                requestSchema: 'EmptyRequest',
                                resultSchema: 'HardDiskCreationProfiles',
                                implemented: true,
                            },
                            {
                                id: 'create.hds.plan',
                                method: 'POST',
                                route: '/api/v1/hard-disk-build-plans',
                                mode: 'request',
                                operationClass: 'read',
                                requiresIdempotency: false,
                                variant: null,
                                requestSchema: 'HardDiskCreationPlanRequest',
                                resultSchema: 'ImageBuildPlan',
                                implemented: true,
                            },
                            {
                                id: 'create.hds',
                                method: 'POST',
                                route: '/api/v1/image-builds',
                                mode: 'job',
                                operationClass: 'write',
                                requiresIdempotency: true,
                                variant: 'HDS',
                                requestSchema: 'ImageBuildRequest',
                                resultSchema: 'ImageBuildResult',
                                implemented: true,
                            },
                        ],
                    });
                }
                if (url.pathname.endsWith('/hard-disk-creation-profiles')) {
                    expect(init?.method).toBe('GET');
                    expect(init?.body).toBeUndefined();
                    return json({
                        schemaVersion: '1.0',
                        profiles: [
                            {
                                profileId: 'CD_R_700',
                                sizeBytes: 737280000,
                                defaultPartitionCount: 1,
                                partitionOptions: [
                                    { partitionCount: 1, partitionSizeBytes: 737278464, unusedTailBytes: 0 },
                                ],
                            },
                        ],
                    });
                }
                if (url.pathname.endsWith('/hard-disk-build-plans')) {
                    expect(JSON.parse(String(init?.body))).toEqual({
                        profileId: 'CD_R_700',
                        partitionCount: 1,
                        output: { rootId: 'workspace', relativePath: 'images/new.hds' },
                        overwrite: false,
                    });
                    return json({
                        planToken: 'quick-plan',
                        kind: 'HDS',
                        summary: { sizeBytes: 737280000, partitionCount: 1, objectCount: 0 },
                    });
                }
                if (url.pathname.endsWith('/image-builds')) {
                    expect(JSON.parse(String(init?.body))).toEqual({ planToken: 'quick-plan' });
                    return json(
                        {
                            jobId: 'quick-job',
                            operationId: 'create.hds',
                            state: 'QUEUED',
                            latestSequence: 0,
                            progress: null,
                            result: null,
                            error: null,
                        },
                        202,
                    );
                }
                throw new Error(`unexpected request ${init?.method} ${url}`);
            }),
        );

        const transport = new HttpImageTransport({ baseUrl: 'http://localhost/api/v1', bearerToken: 'secret' });
        const profiles = await transport.hardDiskCreationProfiles();
        expect(profiles[0]).toMatchObject({ profileId: 'CD_R_700', defaultPartitionCount: 1 });
        const plan = await transport.planHardDiskCreation('CD_R_700', 1, serverFile('images/new.hds'));
        expect(plan).toMatchObject({ planToken: 'quick-plan', sizeBytes: 737280000 });
        const job = await transport.startHardDiskCreation(plan.planToken!);
        expect(job).toMatchObject({ kind: 'create.hds', status: 'queued' });
    });

    it('starts typed volume changes through the retained image session', async () => {
        const alterationBodies: unknown[] = [];
        vi.stubGlobal(
            'fetch',
            vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
                const url = new URL(String(input));
                if (url.pathname.endsWith('/system/capabilities')) {
                    return json({
                        apiVersion: 'v1',
                        limits: {},
                        operations: [
                            {
                                id: 'images.alter',
                                method: 'POST',
                                route: '/api/v1/image-session-alterations',
                                mode: 'job',
                                operationClass: 'write',
                                requiresIdempotency: true,
                                variant: null,
                                requestSchema: 'ImageSessionAlterationRequest',
                                resultSchema: 'ImageSessionAlterationResult',
                                implemented: true,
                            },
                        ],
                    });
                }
                if (url.pathname.endsWith('/images') && init?.method === 'POST') {
                    return json({
                        imageId: 'image-retained',
                        revision: 1,
                        source: {
                            kind: 'FILE',
                            file: { rootId: 'workspace', relativePath: 'images/base.hds' },
                        },
                        companionSources: [],
                        floppySet: null,
                        format: 'sfs',
                        rootCount: 0,
                        objectCount: 0,
                        relationshipCount: 0,
                        availableOperations: [
                            'images.alter.volumes',
                            'images.alter.partitions',
                            'images.alter.objects',
                        ],
                        validation: { valid: true, infoCount: 0, warningCount: 0, errorCount: 0 },
                    });
                }
                if (url.pathname.endsWith('/images/image-retained') && init?.method !== 'POST') {
                    return json({
                        imageId: 'image-retained',
                        revision: 2,
                        source: {
                            kind: 'FILE',
                            file: { rootId: 'workspace', relativePath: 'images/base.hds' },
                        },
                        companionSources: [],
                        floppySet: null,
                        format: 'sfs',
                        rootCount: 0,
                        objectCount: 0,
                        relationshipCount: 0,
                        availableOperations: [
                            'images.alter.volumes',
                            'images.alter.partitions',
                            'images.alter.objects',
                        ],
                        validation: { valid: true, infoCount: 0, warningCount: 0, errorCount: 0 },
                    });
                }
                if (url.pathname.endsWith('/content')) {
                    return json({ items: [], totalCount: 0, nextCursor: null });
                }
                if (url.pathname.endsWith('/image-session-alterations')) {
                    alterationBodies.push(JSON.parse(String(init?.body)));
                    return json(
                        {
                            jobId: `job-${alterationBodies.length}`,
                            operationId: 'images.alter',
                            state: 'QUEUED',
                            latestSequence: 0,
                            progress: null,
                            result: null,
                            error: null,
                        },
                        202,
                    );
                }
                throw new Error(`unexpected request ${init?.method ?? 'GET'} ${url}`);
            }),
        );

        const transport = new HttpImageTransport({
            baseUrl: 'http://localhost/api/v1',
            bearerToken: 'secret',
        });
        const source = serverFile('images/base.hds');
        const opened = await transport.openImage(source);
        await transport.startVolumeMutation(opened.sessionId, {
            kind: 'add',
            partitionIndex: 2,
            volumeName: 'New Volume',
        });
        const refreshed = await transport.refreshImage(opened.sessionId);
        expect(refreshed.sessionId).toBe(opened.sessionId);
        await transport.startVolumeMutation(opened.sessionId, {
            kind: 'rename',
            partitionIndex: 2,
            volumeName: 'Old Volume',
            newVolumeName: 'Renamed Volume',
        });
        await transport.startVolumeMutation(opened.sessionId, {
            kind: 'delete',
            partitionIndex: 2,
            volumeName: 'Renamed Volume',
        });
        await transport.startPartitionMutation(opened.sessionId, {
            kind: 'rename',
            partitionIndex: 2,
            partitionName: 'PARTITION 3',
            newPartitionName: 'Samples',
        });
        await transport.startObjectRename(opened.sessionId, {
            kind: 'program',
            partitionIndex: 2,
            volumeName: 'Volume',
            programNumber: 1,
            newProgramName: 'Piano',
        });
        await transport.startObjectRename(opened.sessionId, {
            kind: 'sample-bank',
            partitionIndex: 2,
            volumeName: 'Volume',
            sampleBankName: 'Old Bank',
            newSampleBankName: 'New Bank',
        });
        await transport.startObjectRename(opened.sessionId, {
            kind: 'sample',
            partitionIndex: 2,
            volumeName: 'Volume',
            sampleName: 'Old Sample',
            newSampleName: 'New Sample',
        });
        await transport.startObjectRename(opened.sessionId, {
            kind: 'wave-data',
            partitionIndex: 2,
            volumeName: 'Volume',
            waveformName: 'Old Wave',
            newWaveformName: 'New Wave',
        });

        expect(alterationBodies).toEqual([
            {
                imageId: 'image-retained',
                expectedRevision: 1,
                manifest: {
                    inline: {
                        schema_version: '1.0',
                        operations: [
                            {
                                id: 'volume-add',
                                type: 'insert_volume',
                                partition_index: 2,
                                volume: { name: 'New Volume', waveforms: [], samples: [] },
                            },
                        ],
                    },
                },
                inputBindings: [],
            },
            {
                imageId: 'image-retained',
                expectedRevision: 2,
                manifest: {
                    inline: {
                        schema_version: '1.0',
                        operations: [
                            {
                                id: 'volume-rename',
                                type: 'rename_volume',
                                partition_index: 2,
                                volume_name: 'Old Volume',
                                new_volume_name: 'Renamed Volume',
                            },
                        ],
                    },
                },
                inputBindings: [],
            },
            {
                imageId: 'image-retained',
                expectedRevision: 2,
                manifest: {
                    inline: {
                        schema_version: '1.0',
                        operations: [
                            {
                                id: 'volume-delete',
                                type: 'delete_volume',
                                partition_index: 2,
                                volume_name: 'Renamed Volume',
                            },
                        ],
                    },
                },
                inputBindings: [],
            },
            {
                imageId: 'image-retained',
                expectedRevision: 2,
                manifest: {
                    inline: {
                        schema_version: '1.0',
                        operations: [
                            {
                                id: 'partition-rename',
                                type: 'rename_partition',
                                partition_index: 2,
                                partition_name: 'PARTITION 3',
                                new_partition_name: 'Samples',
                            },
                        ],
                    },
                },
                inputBindings: [],
            },
            {
                imageId: 'image-retained',
                expectedRevision: 2,
                manifest: {
                    inline: {
                        schema_version: '1.0',
                        operations: [
                            {
                                id: 'rename-program',
                                type: 'rename_program',
                                partition_index: 2,
                                volume_name: 'Volume',
                                program_number: 1,
                                new_program_name: 'Piano',
                            },
                        ],
                    },
                },
                inputBindings: [],
            },
            {
                imageId: 'image-retained',
                expectedRevision: 2,
                manifest: {
                    inline: {
                        schema_version: '1.0',
                        operations: [
                            {
                                id: 'rename-sample-bank',
                                type: 'rename_sbac',
                                partition_index: 2,
                                volume_name: 'Volume',
                                sample_bank_name: 'Old Bank',
                                new_sample_bank_name: 'New Bank',
                            },
                        ],
                    },
                },
                inputBindings: [],
            },
            {
                imageId: 'image-retained',
                expectedRevision: 2,
                manifest: {
                    inline: {
                        schema_version: '1.0',
                        operations: [
                            {
                                id: 'rename-sample',
                                type: 'rename_sbnk',
                                partition_index: 2,
                                volume_name: 'Volume',
                                sample_name: 'Old Sample',
                                new_sample_name: 'New Sample',
                            },
                        ],
                    },
                },
                inputBindings: [],
            },
            {
                imageId: 'image-retained',
                expectedRevision: 2,
                manifest: {
                    inline: {
                        schema_version: '1.0',
                        operations: [
                            {
                                id: 'rename-wave-data',
                                type: 'rename_waveform',
                                partition_index: 2,
                                volume_name: 'Volume',
                                waveform_name: 'Old Wave',
                                new_waveform_name: 'New Wave',
                            },
                        ],
                    },
                },
                inputBindings: [],
            },
        ]);
    });

    it('inspects and starts revision-bound object deletion through typed operations', async () => {
        const bodies = new Map<string, unknown>();
        vi.stubGlobal(
            'fetch',
            vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
                const url = new URL(String(input));
                if (url.pathname.endsWith('/system/capabilities')) {
                    return json({
                        apiVersion: 'v1',
                        limits: {},
                        operations: [
                            {
                                id: 'images.deletion.inspect',
                                method: 'POST',
                                route: '/api/v1/image-object-deletion-inspections',
                                mode: 'read',
                                operationClass: 'read',
                                requiresIdempotency: false,
                                variant: null,
                                requestSchema: 'ImageObjectDeletionRequest',
                                resultSchema: 'ImageObjectDeletionInspection',
                                implemented: true,
                            },
                            {
                                id: 'images.delete',
                                method: 'POST',
                                route: '/api/v1/image-object-deletions',
                                mode: 'job',
                                operationClass: 'write',
                                requiresIdempotency: true,
                                variant: null,
                                requestSchema: 'ImageObjectDeletionRequest',
                                resultSchema: 'ImageObjectDeletionResult',
                                implemented: true,
                            },
                        ],
                    });
                }
                if (url.pathname.endsWith('/images') && init?.method === 'POST') {
                    return json({
                        imageId: 'image-delete',
                        revision: 7,
                        source: {
                            kind: 'FILE',
                            file: { rootId: 'workspace', relativePath: 'images/base.hds' },
                        },
                        companionSources: [],
                        floppySet: null,
                        format: 'sfs',
                        rootCount: 0,
                        objectCount: 3,
                        relationshipCount: 2,
                        availableOperations: ['images.alter.objects'],
                        validation: { valid: true, infoCount: 0, warningCount: 0, errorCount: 0 },
                    });
                }
                if (url.pathname.endsWith('/content')) {
                    return json({ items: [], totalCount: 0, nextCursor: null });
                }
                if (url.pathname.endsWith('/image-object-deletion-inspections')) {
                    bodies.set('inspect', JSON.parse(String(init?.body)));
                    return json({
                        canApply: true,
                        imageId: 'image-delete',
                        revision: 7,
                        targetObjectIds: ['object-sample'],
                        selectedObjectIds: ['object-sample', 'object-wave'],
                        impacts: [
                            {
                                objectId: 'object-sample',
                                objectType: 'SBNK',
                                objectName: 'Tone',
                                partitionIndex: 0,
                                partitionName: 'Partition 0',
                                volumeName: 'Volume',
                                role: 'TARGET',
                                status: 'REQUIRED',
                                selected: true,
                                storedSizeBytes: 512,
                                freedClusters: 1,
                                prerequisiteObjectIds: [],
                                reason: 'Requested deletion target',
                            },
                            {
                                objectId: 'object-wave',
                                objectType: 'SMPL',
                                objectName: 'Tone L',
                                partitionIndex: 0,
                                partitionName: 'Partition 0',
                                volumeName: 'Volume',
                                role: 'DEPENDENCY',
                                status: 'OPTIONAL',
                                selected: true,
                                storedSizeBytes: 4096,
                                freedClusters: 4,
                                prerequisiteObjectIds: ['object-sample'],
                                reason: 'Referenced only by selected Samples',
                            },
                        ],
                        references: [],
                        blockers: [],
                        warnings: [],
                        estimatedFreedBytes: 4608,
                        estimatedFreedClusters: 5,
                    });
                }
                if (url.pathname.endsWith('/image-object-deletions')) {
                    bodies.set('delete', JSON.parse(String(init?.body)));
                    return json(
                        {
                            jobId: 'job-delete',
                            operationId: 'images.delete',
                            state: 'QUEUED',
                            latestSequence: 0,
                            progress: null,
                            result: null,
                            error: null,
                        },
                        202,
                    );
                }
                throw new Error(`unexpected request ${init?.method ?? 'GET'} ${url}`);
            }),
        );

        const transport = new HttpImageTransport({
            baseUrl: 'http://localhost/api/v1',
            bearerToken: 'secret',
        });
        const opened = await transport.openImage(serverFile('images/base.hds'));
        expect(opened.objectDeletionAvailable).toBe(true);

        const inspection = await transport.inspectObjectDeletion(opened.sessionId, ['object-sample'], ['object-wave']);
        expect(inspection).toMatchObject({
            canApply: true,
            selectedObjectIds: ['object-sample', 'object-wave'],
            estimatedFreedClusters: 5,
        });
        const job = await transport.startObjectDeletion(opened.sessionId, ['object-sample'], ['object-wave']);
        expect(job).toMatchObject({ kind: 'images.delete', status: 'queued' });

        const expected = {
            imageId: 'image-delete',
            expectedRevision: 7,
            targetObjectIds: ['object-sample'],
            cleanupObjectIds: ['object-wave'],
        };
        expect(bodies.get('inspect')).toEqual(expected);
        expect(bodies.get('delete')).toEqual(expected);
    });

    it('discovers revision-bound unreferenced Wave Data within one volume scope', async () => {
        let inspectionBody: unknown;
        vi.stubGlobal(
            'fetch',
            vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
                const url = new URL(String(input));
                if (url.pathname.endsWith('/system/capabilities')) {
                    return json({
                        apiVersion: 'v1',
                        limits: {},
                        operations: [
                            {
                                id: 'images.deletion.orphans.inspect',
                                method: 'POST',
                                route: '/api/v1/image-wave-data-orphan-inspections',
                                mode: 'read',
                                operationClass: 'read',
                                requiresIdempotency: false,
                                variant: null,
                                requestSchema: 'ImageWaveDataOrphanInspectionRequest',
                                resultSchema: 'ImageWaveDataOrphanInspection',
                                implemented: true,
                            },
                        ],
                    });
                }
                if (url.pathname.endsWith('/images') && init?.method === 'POST') {
                    return json({
                        imageId: 'image-cleanup',
                        revision: 11,
                        source: {
                            kind: 'FILE',
                            file: { rootId: 'workspace', relativePath: 'images/base.hds' },
                        },
                        companionSources: [],
                        floppySet: null,
                        format: 'sfs',
                        rootCount: 0,
                        objectCount: 1,
                        relationshipCount: 0,
                        availableOperations: ['images.deletion.orphans.inspect'],
                        validation: { valid: true, infoCount: 0, warningCount: 0, errorCount: 0 },
                    });
                }
                if (url.pathname.endsWith('/content')) {
                    return json({ items: [], totalCount: 0, nextCursor: null });
                }
                if (url.pathname.endsWith('/image-wave-data-orphan-inspections')) {
                    inspectionBody = JSON.parse(String(init?.body));
                    return json({
                        imageId: 'image-cleanup',
                        revision: 11,
                        contentScopeId: 'volume-7',
                        candidates: [
                            {
                                objectId: 'object-wave',
                                objectName: 'Unused Wave',
                                objectType: 'SMPL',
                                partitionIndex: 0,
                                partitionName: 'Partition 0',
                                volumeName: 'Volume',
                                storedSizeBytes: 4096,
                                recoverableBytes: 4096,
                                recoverableClusters: 4,
                            },
                        ],
                        totalCandidateCount: 1,
                    });
                }
                throw new Error(`unexpected request ${init?.method ?? 'GET'} ${url}`);
            }),
        );

        const transport = new HttpImageTransport({
            baseUrl: 'http://localhost/api/v1',
            bearerToken: 'secret',
        });
        const opened = await transport.openImage(serverFile('images/base.hds'));
        expect(opened.waveDataCleanupAvailable).toBe(true);

        const inspection = await transport.inspectWaveDataOrphans(opened.sessionId, 'volume-7');
        expect(inspection).toMatchObject({
            contentScopeId: 'volume-7',
            totalCandidateCount: 1,
            candidates: [{ objectId: 'object-wave', objectName: 'Unused Wave' }],
        });
        expect(inspectionBody).toEqual({
            imageId: 'image-cleanup',
            expectedRevision: 11,
            contentScopeId: 'volume-7',
        });
    });

    it('imports uploaded and workspace audio through one atomic alteration job', async () => {
        let alterationBody: unknown;
        vi.stubGlobal(
            'fetch',
            vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
                const url = new URL(String(input));
                if (url.pathname.endsWith('/system/capabilities')) {
                    return json({
                        apiVersion: 'v1',
                        limits: {},
                        operations: [
                            {
                                id: 'images.alter',
                                method: 'POST',
                                route: '/api/v1/image-session-alterations',
                                mode: 'job',
                                operationClass: 'write',
                                requiresIdempotency: true,
                                variant: null,
                                requestSchema: 'ImageSessionAlterationRequest',
                                resultSchema: 'ImageSessionAlterationResult',
                                implemented: true,
                            },
                        ],
                    });
                }
                if (url.pathname.endsWith('/images') && init?.method === 'POST') {
                    return json({
                        imageId: 'image-audio',
                        revision: 1,
                        source: {
                            kind: 'FILE',
                            file: { rootId: 'workspace', relativePath: 'images/base.hds' },
                        },
                        companionSources: [],
                        floppySet: null,
                        format: 'sfs',
                        rootCount: 0,
                        objectCount: 0,
                        relationshipCount: 0,
                        availableOperations: ['images.alter.volumes'],
                        validation: { valid: true, infoCount: 0, warningCount: 0, errorCount: 0 },
                    });
                }
                if (url.pathname.endsWith('/content')) {
                    return json({ items: [], totalCount: 0, nextCursor: null });
                }
                if (url.pathname.endsWith('/image-session-alterations')) {
                    alterationBody = JSON.parse(String(init?.body));
                    return json(
                        {
                            jobId: 'job-audio-import',
                            operationId: 'images.alter',
                            state: 'QUEUED',
                            latestSequence: 0,
                            progress: null,
                            result: null,
                            error: null,
                        },
                        202,
                    );
                }
                throw new Error(`unexpected request ${init?.method ?? 'GET'} ${url}`);
            }),
        );

        const transport = new HttpImageTransport({
            baseUrl: 'http://localhost/api/v1',
            bearerToken: 'secret',
        });
        const mono = clientUploadLocation({ uploadId: 'upload-mono' }, 'AUDIO', 'mono.wav');
        const stereo = serverFile('audio/stereo.flac');
        const opened = await transport.openImage(serverFile('images/base.hds'));
        const job = await transport.startAudioImport(opened.sessionId, { partitionIndex: 3, volumeName: 'Imported' }, [
            {
                source: mono,
                sampleName: 'Mono',
                waveformNames: ['Mono Wave'],
                rootKey: 60,
                fineTuneCents: 0,
                keyLow: 0,
                keyHigh: 127,
                velocityLow: 0,
                velocityHigh: 127,
                loopMode: 4,
                loopStartFrame: 0,
                loopLengthFrames: 0,
                targetSampleRate: 44_100,
            },
            {
                source: stereo,
                sampleName: 'Stereo',
                waveformNames: ['Stereo-L', 'Stereo-R'],
                rootKey: 69,
                fineTuneCents: -5,
                keyLow: 12,
                keyHigh: 108,
                velocityLow: 4,
                velocityHigh: 120,
                loopMode: 1,
                loopStartFrame: 100,
                loopLengthFrames: 1_000,
                targetSampleRate: 22_050,
            },
        ]);

        expect(job).toMatchObject({ jobId: 1, kind: 'images.alter', status: 'queued' });
        expect(alterationBody).toEqual({
            imageId: 'image-audio',
            expectedRevision: 1,
            manifest: {
                inline: {
                    schema_version: '1.0',
                    operations: [
                        {
                            id: 'wave-0',
                            type: 'insert_waveform',
                            partition_index: 3,
                            volume_name: 'Imported',
                            audio: {
                                path: 'audio/import-0',
                                waveform_names: ['Mono Wave'],
                                root_key: 60,
                                fine_tune_cents: 0,
                                loop_mode: 4,
                                loop_start_frame: 0,
                                loop_length_frames: 0,
                                target_sample_rate: 44_100,
                            },
                        },
                        {
                            id: 'sample-0',
                            type: 'insert_sbnk',
                            partition_index: 3,
                            volume_name: 'Imported',
                            sample: {
                                name: 'Mono',
                                waveform_name: 'Mono Wave',
                                root_key: 60,
                                fine_tune_cents: 0,
                                key_low: 0,
                                key_high: 127,
                                velocity_low: 0,
                                velocity_high: 127,
                                loop_mode: 4,
                                loop_start_frame: 0,
                                loop_length_frames: 0,
                                level: 100,
                            },
                        },
                        {
                            id: 'wave-1',
                            type: 'insert_waveform',
                            partition_index: 3,
                            volume_name: 'Imported',
                            audio: {
                                path: 'audio/import-1',
                                waveform_names: ['Stereo-L', 'Stereo-R'],
                                root_key: 69,
                                fine_tune_cents: -5,
                                loop_mode: 1,
                                loop_start_frame: 100,
                                loop_length_frames: 1_000,
                                target_sample_rate: 22_050,
                            },
                        },
                        {
                            id: 'sample-1',
                            type: 'insert_sbnk',
                            partition_index: 3,
                            volume_name: 'Imported',
                            sample: {
                                name: 'Stereo',
                                waveform_name: 'Stereo-L',
                                right_waveform_name: 'Stereo-R',
                                root_key: 69,
                                fine_tune_cents: -5,
                                key_low: 12,
                                key_high: 108,
                                velocity_low: 4,
                                velocity_high: 120,
                                loop_mode: 1,
                                loop_start_frame: 100,
                                loop_length_frames: 1_000,
                                level: 100,
                            },
                        },
                    ],
                },
            },
            inputBindings: [
                { manifestPath: 'audio/import-0', input: { uploadRef: { uploadId: 'upload-mono' } } },
                {
                    manifestPath: 'audio/import-1',
                    input: { fileRef: { rootId: 'workspace', relativePath: 'audio/stereo.flac' } },
                },
            ],
        });
    });

    it('imports uploaded and workspace MIDI through one atomic alteration job', async () => {
        let alterationBody: unknown;
        vi.stubGlobal(
            'fetch',
            vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
                const url = new URL(String(input));
                if (url.pathname.endsWith('/system/capabilities')) {
                    return json({
                        apiVersion: 'v1',
                        limits: {},
                        operations: [
                            {
                                id: 'images.alter',
                                method: 'POST',
                                route: '/api/v1/image-session-alterations',
                                mode: 'job',
                                operationClass: 'write',
                                requiresIdempotency: true,
                                variant: null,
                                requestSchema: 'ImageSessionAlterationRequest',
                                resultSchema: 'ImageSessionAlterationResult',
                                implemented: true,
                            },
                        ],
                    });
                }
                if (url.pathname.endsWith('/images') && init?.method === 'POST') {
                    return json({
                        imageId: 'image-midi',
                        revision: 4,
                        source: {
                            kind: 'FILE',
                            file: { rootId: 'workspace', relativePath: 'images/base.hds' },
                        },
                        companionSources: [],
                        floppySet: null,
                        format: 'sfs',
                        rootCount: 0,
                        objectCount: 0,
                        relationshipCount: 0,
                        availableOperations: ['images.alter.objects'],
                        validation: { valid: true, infoCount: 0, warningCount: 0, errorCount: 0 },
                    });
                }
                if (url.pathname.endsWith('/content')) {
                    return json({ items: [], totalCount: 0, nextCursor: null });
                }
                if (url.pathname.endsWith('/image-session-alterations')) {
                    alterationBody = JSON.parse(String(init?.body));
                    return json(
                        {
                            jobId: 'job-midi-import',
                            operationId: 'images.alter',
                            state: 'QUEUED',
                            latestSequence: 0,
                            progress: null,
                            result: null,
                            error: null,
                        },
                        202,
                    );
                }
                throw new Error(`unexpected request ${init?.method ?? 'GET'} ${url}`);
            }),
        );

        const transport = new HttpImageTransport({
            baseUrl: 'http://localhost/api/v1',
            bearerToken: 'secret',
        });
        const uploaded = clientUploadLocation({ uploadId: 'upload-midi' }, 'MIDI', 'Intro.mid');
        const workspace = serverFile('midi/Ending.midi');
        const opened = await transport.openImage(serverFile('images/base.hds'));
        await transport.startSequenceImport(
            opened.sessionId,
            { partitionIndex: 2, volumeName: 'Songs' },
            [
                { source: uploaded, sequenceName: 'Intro' },
                { source: workspace, sequenceName: 'Ending' },
            ],
            'exclude',
        );

        expect(alterationBody).toEqual({
            imageId: 'image-midi',
            expectedRevision: 4,
            manifest: {
                inline: {
                    schema_version: '1.0',
                    operations: [
                        {
                            id: 'sequence-0',
                            type: 'insert_sequence',
                            partition_index: 2,
                            volume_name: 'Songs',
                            sequence: {
                                name: 'Intro',
                                midi_path: 'sequence/import-0.mid',
                                system_exclusive_policy: 'exclude',
                            },
                        },
                        {
                            id: 'sequence-1',
                            type: 'insert_sequence',
                            partition_index: 2,
                            volume_name: 'Songs',
                            sequence: {
                                name: 'Ending',
                                midi_path: 'sequence/import-1.mid',
                                system_exclusive_policy: 'exclude',
                            },
                        },
                    ],
                },
            },
            inputBindings: [
                { manifestPath: 'sequence/import-0.mid', input: { uploadRef: { uploadId: 'upload-midi' } } },
                {
                    manifestPath: 'sequence/import-1.mid',
                    input: { fileRef: { rootId: 'workspace', relativePath: 'midi/Ending.midi' } },
                },
            ],
        });
    });

    it('rejects desktop paths instead of relabeling them as sandbox paths', async () => {
        const transport = new HttpImageTransport({ baseUrl: 'http://localhost/api/v1', bearerToken: 'secret' });
        const desktopPath = { kind: 'native-file', path: 'C:\\Users\\example\\disk.hds' } as unknown as FileLocation;
        await expect(transport.openImage(desktopPath)).rejects.toThrow('server sandbox file selection');
    });

    it('binds a temporary manifest upload directly without materializing a sandbox copy', async () => {
        vi.stubGlobal(
            'fetch',
            vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
                const url = new URL(String(input));
                if (url.pathname.endsWith('/system/capabilities')) {
                    return json({
                        apiVersion: 'v1',
                        operations: [
                            {
                                id: 'create.plan',
                                method: 'POST',
                                route: '/api/v1/image-build-plans',
                                mode: 'request',
                                operationClass: 'read',
                                requiresIdempotency: false,
                                variant: null,
                                requestSchema: 'ImageBuildPlanRequest',
                                resultSchema: 'ImageBuildPlan',
                                implemented: true,
                            },
                        ],
                    });
                }
                if (url.pathname.endsWith('/image-build-plans')) {
                    expect(JSON.parse(String(init?.body))).toMatchObject({
                        manifest: { uploadRef: { uploadId: 'upload-manifest' } },
                        inputBindings: [
                            {
                                manifestPath: 'audio/tone.wav',
                                input: { uploadRef: { uploadId: 'upload-audio' } },
                            },
                        ],
                        output: { rootId: 'workspace', relativePath: 'images/from-upload.hds' },
                    });
                    return json({
                        planToken: 'plan-upload',
                        kind: 'HDS',
                        summary: { sizeBytes: 1024, partitionCount: 1 },
                    });
                }
                throw new Error(`unexpected request ${init?.method} ${url}`);
            }),
        );

        const transport = new HttpImageTransport({ baseUrl: 'http://localhost/api/v1', bearerToken: 'secret' });
        const manifest = clientUploadLocation({ uploadId: 'upload-manifest' }, 'MANIFEST', 'build.json');
        const audio = clientUploadLocation({ uploadId: 'upload-audio' }, 'AUDIO', 'tone.wav');
        await expect(
            transport.planCreate(manifest, serverFile('images/from-upload.hds'), false, [
                {
                    logicalPath: 'audio/tone.wav',
                    source: audio,
                },
            ]),
        ).resolves.toMatchObject({ planToken: 'plan-upload' });
    });

    it('downloads server files and deletes temporary directory archives after reading them', async () => {
        const requests: string[] = [];
        vi.stubGlobal(
            'fetch',
            vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
                const url = new URL(String(input));
                requests.push(`${init?.method} ${url.pathname}${url.search}`);
                if (url.pathname.endsWith('/files/content')) {
                    expect(url.searchParams.get('rootId')).toBe('workspace');
                    expect(url.searchParams.get('relativePath')).toBe('exports/tone.wav');
                    if (init?.method === 'HEAD') {
                        return new Response(null, {
                            headers: { 'Content-Length': '10', ETag: '"tone-revision"' },
                        });
                    }
                    expect((init?.headers as Record<string, string>)['If-Match']).toBe('"tone-revision"');
                    return new Response('wave-bytes', {
                        status: 206,
                        headers: {
                            'Content-Disposition': 'attachment; filename="tone.wav"',
                            ETag: '"tone-revision"',
                        },
                    });
                }
                if (url.pathname.endsWith('/system/capabilities')) {
                    return json({
                        apiVersion: 'v1',
                        operations: [
                            {
                                id: 'files.archive',
                                method: 'POST',
                                route: '/api/v1/files/archive',
                                mode: 'job',
                                operationClass: 'read',
                                requiresIdempotency: true,
                                variant: null,
                                requestSchema: 'DirectoryArchiveRequest',
                                resultSchema: 'DirectoryArchiveResult',
                                implemented: true,
                            },
                        ],
                        limits: { maximumDownloadRangeBytes: 10 },
                    });
                }
                if (url.pathname.endsWith('/files/archive') && init?.method === 'POST') {
                    expect(JSON.parse(String(init.body))).toEqual({
                        directory: { rootId: 'workspace', relativePath: 'exports/set' },
                    });
                    return json(
                        {
                            jobId: 'job-archive',
                            operationId: 'files.archive',
                            state: 'QUEUED',
                            latestSequence: 1,
                            progress: null,
                            result: null,
                            error: null,
                        },
                        202,
                    );
                }
                if (url.pathname.endsWith('/jobs/job-archive/events')) {
                    return json({ events: [] });
                }
                if (url.pathname.endsWith('/jobs/job-archive')) {
                    return json({
                        jobId: 'job-archive',
                        operationId: 'files.archive',
                        state: 'COMPLETED',
                        latestSequence: 2,
                        progress: null,
                        result: {
                            archiveId: 'archive-one',
                            filename: 'set.tar',
                            sizeBytes: 9,
                            entryCount: 2,
                            expiresInSeconds: 60,
                            contentPath: '/api/v1/download-archives/archive-one/content',
                        },
                        error: null,
                    });
                }
                if (url.pathname.endsWith('/download-archives/archive-one/content') && init?.method === 'GET') {
                    return new Response('tar-bytes');
                }
                if (url.pathname.endsWith('/download-archives/archive-one/content') && init?.method === 'DELETE') {
                    return json({ deleted: true });
                }
                throw new Error(`unexpected request ${init?.method} ${url}`);
            }),
        );

        const transport = new HttpImageTransport({ baseUrl: 'http://localhost/api/v1', bearerToken: 'secret' });
        const file = await transport.downloadFile(serverFile('exports/tone.wav'));
        expect(file.filename).toBe('tone.wav');
        expect(await file.blob.text()).toBe('wave-bytes');

        const directory = await transport.downloadDirectory(
            serverDirectoryLocation({
                rootId: 'workspace',
                relativePath: 'exports/set',
            }),
        );
        expect(directory.filename).toBe('set.tar');
        expect(await directory.blob.text()).toBe('tar-bytes');
        expect(requests.at(-1)).toBe('DELETE /api/v1/download-archives/archive-one/content');
    });

    it('verifies and imports a temporary package upload without materializing it', async () => {
        vi.stubGlobal(
            'fetch',
            vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
                const url = new URL(String(input));
                if (url.pathname.endsWith('/system/capabilities')) {
                    return json({
                        apiVersion: 'v1',
                        limits: {},
                        operations: [
                            {
                                id: 'package.verify',
                                method: 'POST',
                                route: '/api/v1/package-verifications',
                                mode: 'request',
                                operationClass: 'read',
                                requiresIdempotency: false,
                                variant: null,
                                requestSchema: 'PackageReadRequest',
                                resultSchema: 'PackageVerification',
                                implemented: true,
                            },
                            {
                                id: 'package.plan_import',
                                method: 'POST',
                                route: '/api/v1/package-import-plans',
                                mode: 'request',
                                operationClass: 'read',
                                requiresIdempotency: false,
                                variant: null,
                                requestSchema: 'PackageImportPlanRequest',
                                resultSchema: 'PackageImportPlan',
                                implemented: true,
                            },
                            {
                                id: 'package.import',
                                method: 'POST',
                                route: '/api/v1/package-imports',
                                mode: 'job',
                                operationClass: 'write',
                                requiresIdempotency: true,
                                variant: null,
                                requestSchema: 'PackageImportRequest',
                                resultSchema: 'PackageImportResult',
                                implemented: true,
                            },
                        ],
                    });
                }
                if (url.pathname.endsWith('/package-verifications')) {
                    expect(JSON.parse(String(init?.body))).toEqual({
                        package: { uploadRef: { uploadId: 'upload-package' } },
                    });
                    return json({
                        schemaVersion: '1.0',
                        packageId: 'package-one',
                        packageKind: 'volume',
                        requiredExtension: '.axkvol',
                        sourceMediaKind: 'sfs',
                        valid: true,
                        payloadsVerified: true,
                        roots: [],
                        objects: [],
                        relationshipCount: 0,
                        issues: [],
                    });
                }
                if (url.pathname.endsWith('/package-import-plans')) {
                    expect(JSON.parse(String(init?.body))).toEqual({
                        target: { rootId: 'workspace', relativePath: 'images/base.hds' },
                        output: { rootId: 'workspace', relativePath: 'images/imported.hds' },
                        packages: [{ uploadRef: { uploadId: 'upload-package' } }],
                        destinations: [
                            { packageIndex: 0, rootIndex: 0, partitionIndex: 0, volumeName: 'IMPORTED', create: true },
                        ],
                        renames: [],
                        overwrite: false,
                    });
                    return json({
                        schemaVersion: '1.0',
                        planToken: 'package-plan-one',
                        expiresInSeconds: 600,
                        planId: 'plan-one',
                        targetKind: 'sfs',
                        targetSnapshotId: 'before',
                        valid: true,
                        warnings: [],
                        conflicts: [],
                        actions: [],
                        programAssignmentAdjustments: [],
                        allocation: [],
                    });
                }
                if (url.pathname.endsWith('/package-imports')) {
                    expect(JSON.parse(String(init?.body))).toEqual({ planToken: 'package-plan-one' });
                    expect((init?.headers as Record<string, string>)['Idempotency-Key']).toBeTruthy();
                    return json(
                        {
                            jobId: 'package-job',
                            operationId: 'package.import',
                            state: 'QUEUED',
                            latestSequence: 1,
                            progress: null,
                            result: null,
                            error: null,
                        },
                        202,
                    );
                }
                throw new Error(`unexpected request ${init?.method} ${url}`);
            }),
        );

        const transport = new HttpImageTransport({ baseUrl: 'http://localhost/api/v1', bearerToken: 'secret' });
        const uploaded = clientUploadLocation({ uploadId: 'upload-package' }, 'PACKAGE', 'volume.axkvol');
        await expect(transport.inspectPackage(uploaded, true)).resolves.toMatchObject({
            packageId: 'package-one',
            valid: true,
            payloadsVerified: true,
        });
        const plan = await transport.planPackageImport(
            serverFile('images/base.hds'),
            serverFile('images/imported.hds'),
            [uploaded],
            [{ packageIndex: 0, rootIndex: 0, partitionIndex: 0, volumeName: 'IMPORTED', create: true }],
            false,
        );
        expect(plan).toMatchObject({ planToken: 'package-plan-one', valid: true });
        await expect(transport.startPackageImport(plan.planToken)).resolves.toMatchObject({
            kind: 'package.import',
            status: 'queued',
        });
    });

    it('plans, releases, imports, and exports packages against an open image revision', async () => {
        const requests: Array<{ path: string; body: unknown }> = [];
        vi.stubGlobal(
            'fetch',
            vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
                const url = new URL(String(input));
                const body = init?.body ? JSON.parse(String(init.body)) : undefined;
                requests.push({ path: `${init?.method ?? 'GET'} ${url.pathname}`, body });
                if (url.pathname.endsWith('/system/capabilities')) {
                    return json({
                        apiVersion: 'v1',
                        limits: {},
                        operations: [
                            {
                                id: 'images.package_import.plan',
                                method: 'POST',
                                route: '/api/v1/image-session-package-import-plans',
                                mode: 'REQUEST',
                                operationClass: 'READ',
                                requiresIdempotency: false,
                                variant: null,
                                requestSchema: 'ImageSessionPackageImportPlanRequest',
                                resultSchema: 'ImageSessionPackageImportPlan',
                                implemented: true,
                            },
                            {
                                id: 'images.package_import.release',
                                method: 'POST',
                                route: '/api/v1/image-session-package-import-plan-releases',
                                mode: 'REQUEST',
                                operationClass: 'WRITE',
                                requiresIdempotency: false,
                                variant: null,
                                requestSchema: 'PlanTokenRequest',
                                resultSchema: 'PlanReleaseResult',
                                implemented: true,
                            },
                            {
                                id: 'images.package_import',
                                method: 'POST',
                                route: '/api/v1/image-session-package-imports',
                                mode: 'JOB',
                                operationClass: 'WRITE',
                                requiresIdempotency: true,
                                variant: null,
                                requestSchema: 'PackageImportRequest',
                                resultSchema: 'ImageSessionPackageImportResult',
                                implemented: true,
                            },
                            {
                                id: 'images.package_export',
                                method: 'POST',
                                route: '/api/v1/image-session-package-exports',
                                mode: 'JOB',
                                operationClass: 'READ',
                                requiresIdempotency: true,
                                variant: null,
                                requestSchema: 'ImageSessionPackageExportRequest',
                                resultSchema: 'ImageSessionPackageExportResult',
                                implemented: true,
                            },
                            {
                                id: 'images.audio_export.inspect',
                                method: 'POST',
                                route: '/api/v1/image-session-audio-export-inspections',
                                mode: 'REQUEST',
                                operationClass: 'READ',
                                requiresIdempotency: false,
                                variant: null,
                                requestSchema: 'ImageSessionAudioExportInspectionRequest',
                                resultSchema: 'ImageSessionAudioExportInspection',
                                implemented: true,
                            },
                            {
                                id: 'images.audio_export',
                                method: 'POST',
                                route: '/api/v1/image-session-audio-exports',
                                mode: 'JOB',
                                operationClass: 'READ',
                                requiresIdempotency: true,
                                variant: null,
                                requestSchema: 'ImageSessionAudioExportRequest',
                                resultSchema: 'ImageSessionAudioExportResult',
                                implemented: true,
                            },
                        ],
                    });
                }
                if (url.pathname.endsWith('/images') && init?.method === 'POST') {
                    return json(
                        {
                            imageId: 'image-package',
                            revision: 7,
                            source: {
                                kind: 'FILE',
                                file: { rootId: 'workspace', relativePath: 'images/base.hds' },
                            },
                            companionSources: [],
                            floppySet: null,
                            format: 'sfs',
                            rootCount: 1,
                            objectCount: 2,
                            relationshipCount: 1,
                            availableOperations: [
                                'images.package.import',
                                'images.package.export',
                                'images.audio_export',
                            ],
                            validation: { valid: true, infoCount: 0, warningCount: 0, errorCount: 0 },
                        },
                        201,
                    );
                }
                if (url.pathname.endsWith('/images/image-package/content')) {
                    return json({ items: [], totalCount: 0, nextCursor: null });
                }
                if (url.pathname.endsWith('/image-session-package-import-plans')) {
                    return json({
                        schemaVersion: '1.0',
                        imageId: 'image-package',
                        revision: 7,
                        planToken: 'session-plan',
                        expiresInSeconds: 600,
                        planId: 'plan-id',
                        targetKind: 'SFS',
                        targetSnapshotId: 'snapshot',
                        valid: true,
                        warnings: [],
                        conflicts: [],
                        actions: [],
                        programAssignmentAdjustments: [],
                        allocation: [],
                    });
                }
                if (url.pathname.endsWith('/image-session-package-import-plan-releases')) {
                    return json({ released: true });
                }
                if (url.pathname.endsWith('/image-session-audio-export-inspections')) {
                    return json({
                        imageId: 'image-package',
                        revision: 7,
                        rootCount: 1,
                        programCount: 0,
                        sampleBankCount: 1,
                        sampleCount: 2,
                        waveDataCount: 2,
                        sfzFileCount: 1,
                        sfzEligible: true,
                        defaultDirectoryName: 'DRUMS',
                        issues: [],
                    });
                }
                if (
                    url.pathname.endsWith('/image-session-package-imports') ||
                    url.pathname.endsWith('/image-session-package-exports') ||
                    url.pathname.endsWith('/image-session-audio-exports')
                ) {
                    const audioExport = url.pathname.endsWith('/image-session-audio-exports');
                    return json(
                        {
                            jobId: audioExport
                                ? 'audio-export-job'
                                : url.pathname.endsWith('exports')
                                  ? 'export-job'
                                  : 'import-job',
                            operationId: audioExport
                                ? 'images.audio_export'
                                : url.pathname.endsWith('exports')
                                  ? 'images.package_export'
                                  : 'images.package_import',
                            state: 'QUEUED',
                            latestSequence: 1,
                            progress: null,
                            result: null,
                            error: null,
                        },
                        202,
                    );
                }
                if (url.pathname.endsWith('/download-archives/archive-package/content')) {
                    return new Response(null, { status: 204 });
                }
                throw new Error(`unexpected request ${init?.method} ${url}`);
            }),
        );

        const transport = new HttpImageTransport({ baseUrl: 'http://localhost/api/v1', bearerToken: 'secret' });
        const opened = await transport.openImage(serverFile('images/base.hds'));
        expect(opened).toMatchObject({
            packageImportAvailable: true,
            packageExportAvailable: true,
            audioExportAvailable: true,
        });
        const source = serverFile('packages/drums.axkvol');
        const plan = await transport.planImagePackageImport(opened.sessionId, source, 0, 'DRUMS', [
            { nodeId: 'node-1', destinationName: 'DRUM KIT' },
        ]);
        await transport.planImagePackageImport(
            opened.sessionId,
            source,
            0,
            'DRUMS',
            [{ nodeId: 'node-1', destinationName: 'DRUM KIT 2' }],
            plan.planToken,
        );
        await transport.releaseImagePackageImportPlan(plan.planToken);
        await expect(transport.startImagePackageImport(plan.planToken)).resolves.toMatchObject({
            kind: 'images.package_import',
            status: 'queued',
        });
        await expect(
            transport.startImagePackageExport(
                opened.sessionId,
                [
                    { kind: 'SBNK', objectId: 'object-sample' },
                    { kind: 'SMPL', objectId: 'object-wave-data' },
                ],
                {
                    kind: 'DOWNLOAD',
                    filename: 'DRUMS.axkpkg',
                },
            ),
        ).resolves.toMatchObject({ kind: 'images.package_export', status: 'queued' });
        await expect(
            transport.inspectImageAudioExport(opened.sessionId, [{ kind: 'SBAC', objectId: 'object-bank' }]),
        ).resolves.toMatchObject({ sfzEligible: true, defaultDirectoryName: 'DRUMS' });
        await expect(
            transport.startImageAudioExport(opened.sessionId, [{ kind: 'SBAC', objectId: 'object-bank' }], 'SFZ', {
                kind: 'DOWNLOAD',
                directoryName: 'DRUMS',
            }),
        ).resolves.toMatchObject({ kind: 'images.audio_export', status: 'queued' });
        const retained = {
            archiveId: 'archive-package',
            filename: 'DRUMS.axkvol',
            sizeBytes: 13,
            expiresInSeconds: 60,
            contentPath: '/api/v1/download-archives/archive-package/content',
        };
        await transport.deleteRetainedPackage(retained);

        const planRequests = requests.filter((request) => request.path.endsWith('image-session-package-import-plans'));
        expect(planRequests[0]?.body).toEqual({
            imageId: 'image-package',
            expectedRevision: 7,
            package: { fileRef: { rootId: 'workspace', relativePath: 'packages/drums.axkvol' } },
            partitionIndex: 0,
            volumeName: 'DRUMS',
            renames: [{ nodeId: 'node-1', destinationName: 'DRUM KIT' }],
        });
        expect(planRequests[1]?.body).toEqual({
            imageId: 'image-package',
            expectedRevision: 7,
            package: { fileRef: { rootId: 'workspace', relativePath: 'packages/drums.axkvol' } },
            replacePlanToken: 'session-plan',
            partitionIndex: 0,
            volumeName: 'DRUMS',
            renames: [{ nodeId: 'node-1', destinationName: 'DRUM KIT 2' }],
        });
        expect(requests.find((request) => request.path.endsWith('image-session-package-exports'))?.body).toEqual({
            imageId: 'image-package',
            expectedRevision: 7,
            roots: [
                { kind: 'SBNK', objectId: 'object-sample' },
                { kind: 'SMPL', objectId: 'object-wave-data' },
            ],
            destination: { kind: 'DOWNLOAD', filename: 'DRUMS.axkpkg' },
        });
        expect(
            requests.find((request) => request.path.endsWith('image-session-audio-export-inspections'))?.body,
        ).toEqual({
            imageId: 'image-package',
            expectedRevision: 7,
            roots: [{ kind: 'SBAC', objectId: 'object-bank' }],
        });
        expect(requests.find((request) => request.path.endsWith('image-session-audio-exports'))?.body).toEqual({
            imageId: 'image-package',
            expectedRevision: 7,
            roots: [{ kind: 'SBAC', objectId: 'object-bank' }],
            format: 'SFZ',
            destination: { kind: 'DOWNLOAD', directoryName: 'DRUMS' },
        });
        expect(requests.at(-1)?.path).toBe('DELETE /api/v1/download-archives/archive-package/content');
    });

    it('runs remote alteration and extraction jobs without deleting persistent outputs', async () => {
        const requests: string[] = [];
        vi.stubGlobal(
            'fetch',
            vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
                const url = new URL(String(input));
                requests.push(`${init?.method ?? 'GET'} ${url.pathname}`);
                expect(url.protocol).toBe('https:');
                if (url.pathname.endsWith('/images') && init?.method === 'POST') {
                    return json(
                        {
                            imageId: 'image-remote',
                            source: {
                                kind: 'FILE',
                                file: { rootId: 'workspace', relativePath: 'images/base.hds' },
                            },
                            companionSources: [],
                            floppySet: null,
                            format: 'sfs',
                            rootCount: 1,
                            objectCount: 1,
                            relationshipCount: 0,
                            validation: { valid: true, infoCount: 0, warningCount: 0, errorCount: 0 },
                        },
                        201,
                    );
                }
                if (url.pathname.endsWith('/images/image-remote/content')) {
                    return json({ items: [], totalCount: 0, nextCursor: null });
                }
                if (url.pathname.endsWith('/images/image-remote/objects')) {
                    return json({ items: [], totalCount: 0, nextCursor: null });
                }
                if (url.pathname.endsWith('/system/capabilities')) {
                    return json({
                        apiVersion: 'v1',
                        limits: {},
                        operations: [
                            {
                                id: 'alter.inspect',
                                method: 'POST',
                                route: '/api/v1/image-alteration-inspections',
                                mode: 'request',
                                operationClass: 'read',
                                requiresIdempotency: false,
                                variant: null,
                                requestSchema: 'ImageAlterationInspectionRequest',
                                resultSchema: 'ImageAlterationInspection',
                                implemented: true,
                            },
                            {
                                id: 'alter.hds',
                                method: 'POST',
                                route: '/api/v1/image-alterations',
                                mode: 'job',
                                operationClass: 'write',
                                requiresIdempotency: true,
                                variant: null,
                                requestSchema: 'ImageAlterationRequest',
                                resultSchema: 'ImageAlterationResult',
                                implemented: true,
                            },
                            {
                                id: 'extract.wav',
                                method: 'POST',
                                route: '/api/v1/extractions',
                                mode: 'job',
                                operationClass: 'write',
                                requiresIdempotency: true,
                                variant: 'WAV',
                                requestSchema: 'ExtractionRequest',
                                resultSchema: 'ExtractionResult',
                                implemented: true,
                            },
                            {
                                id: 'extract.sfz',
                                method: 'POST',
                                route: '/api/v1/extractions',
                                mode: 'job',
                                operationClass: 'write',
                                requiresIdempotency: true,
                                variant: 'SFZ',
                                requestSchema: 'ExtractionRequest',
                                resultSchema: 'ExtractionResult',
                                implemented: true,
                            },
                        ],
                    });
                }
                if (url.pathname.endsWith('/image-alteration-inspections')) {
                    expect(JSON.parse(String(init?.body))).toEqual({
                        source: { rootId: 'workspace', relativePath: 'images/base.hds' },
                        manifest: { fileRef: { rootId: 'workspace', relativePath: 'authoring/alter.json' } },
                        inputBindings: [
                            {
                                manifestPath: 'audio/tone.wav',
                                input: { uploadRef: { uploadId: 'audio-upload' } },
                            },
                        ],
                    });
                    return json({
                        kind: 'ALTERATION',
                        summary: { operationCount: 1, appliesChanges: true },
                    });
                }
                if (url.pathname.endsWith('/image-alterations')) {
                    expect(JSON.parse(String(init?.body))).toEqual({
                        source: { rootId: 'workspace', relativePath: 'images/base.hds' },
                        manifest: { fileRef: { rootId: 'workspace', relativePath: 'authoring/alter.json' } },
                        inputBindings: [
                            {
                                manifestPath: 'audio/tone.wav',
                                input: { uploadRef: { uploadId: 'audio-upload' } },
                            },
                        ],
                        output: { rootId: 'workspace', relativePath: 'images/altered.hds' },
                        overwrite: false,
                    });
                    return json(
                        {
                            jobId: 'alter-job',
                            operationId: 'alter.hds',
                            state: 'QUEUED',
                            latestSequence: 0,
                            progress: null,
                            result: null,
                            error: null,
                        },
                        202,
                    );
                }
                if (url.pathname.endsWith('/extractions')) {
                    expect(JSON.parse(String(init?.body))).toEqual({
                        sources: [{ rootId: 'workspace', relativePath: 'images/base.hds' }],
                        destination: { rootId: 'workspace', relativePath: 'exports/sfz' },
                        scope: 'FILE',
                        selectors: [],
                        overwrite: false,
                        strict: true,
                        operationId: 'extract.sfz',
                    });
                    return json(
                        {
                            jobId: 'extract-job',
                            operationId: 'extract.sfz',
                            state: 'QUEUED',
                            latestSequence: 0,
                            progress: null,
                            result: null,
                            error: null,
                        },
                        202,
                    );
                }
                if (url.pathname.endsWith('/jobs/alter-job') && init?.method === 'GET') {
                    return json({
                        jobId: 'alter-job',
                        operationId: 'alter.hds',
                        state: 'COMPLETED',
                        latestSequence: 6,
                        progress: { phase: 'PUBLISHING', completed: 1, total: 1, message: 'Published image' },
                        result: { output: { rootId: 'workspace', relativePath: 'images/altered.hds' } },
                        error: null,
                    });
                }
                if (url.pathname.endsWith('/jobs/extract-job') && init?.method === 'DELETE') {
                    return new Response(null, { status: 204 });
                }
                throw new Error(`unexpected request ${init?.method ?? 'GET'} ${url}`);
            }),
        );

        const transport = new HttpImageTransport({
            baseUrl: 'https://sampler.example.test/api/v1',
            bearerToken: 'remote-secret',
        });
        const opened = await transport.openImage(serverFile('images/base.hds'));
        const audio = clientUploadLocation({ uploadId: 'audio-upload' }, 'AUDIO', 'tone.wav');
        await expect(
            transport.planAlter(
                serverFile('images/base.hds'),
                serverFile('authoring/alter.json'),
                serverFile('images/altered.hds'),
                false,
                [{ logicalPath: 'audio/tone.wav', source: audio }],
            ),
        ).resolves.toMatchObject({ operationCount: 1 });
        const alteration = await transport.startAlter(
            serverFile('images/base.hds'),
            serverFile('authoring/alter.json'),
            serverFile('images/altered.hds'),
            [{ logicalPath: 'audio/tone.wav', source: audio }],
        );
        const extraction = await transport.startExport(
            opened.sessionId,
            serverDirectoryLocation({ rootId: 'workspace', relativePath: 'exports/sfz' }),
            false,
            true,
        );
        expect(extraction).toMatchObject({ kind: 'extract.sfz', status: 'queued' });
        await expect(transport.jobStatus(alteration.jobId)).resolves.toMatchObject({
            status: 'completed',
            result: { output: { rootId: 'workspace', relativePath: 'images/altered.hds' } },
        });
        await transport.cancelJob(alteration.jobId);
        await transport.cancelJob();

        expect(requests).not.toContain('DELETE /api/v1/jobs/alter-job');
        expect(requests).toContain('DELETE /api/v1/jobs/extract-job');
        expect(requests.some((request) => request.includes('/files/content'))).toBe(false);
        expect(requests.some((request) => request.includes('images/altered.hds'))).toBe(false);
    });

    it('exhausts its retry budget when event sockets repeatedly open and immediately close', async () => {
        vi.useFakeTimers();
        FlappingWebSocket.instances = 0;
        vi.stubGlobal('WebSocket', FlappingWebSocket as unknown as typeof WebSocket);
        vi.stubGlobal(
            'fetch',
            vi.fn(async (input: RequestInfo | URL) => {
                const url = new URL(String(input));
                if (url.pathname.endsWith('/system/capabilities')) {
                    return json({
                        apiVersion: 'v1',
                        limits: {},
                        operations: [
                            {
                                id: 'create.plan',
                                method: 'POST',
                                route: '/api/v1/image-build-plans',
                                mode: 'request',
                                operationClass: 'read',
                                requiresIdempotency: false,
                                variant: null,
                                requestSchema: 'ImageBuildPlanRequest',
                                resultSchema: 'ImageBuildPlan',
                                implemented: true,
                            },
                            {
                                id: 'create.hds',
                                method: 'POST',
                                route: '/api/v1/image-builds',
                                mode: 'job',
                                operationClass: 'write',
                                requiresIdempotency: true,
                                variant: 'HDS',
                                requestSchema: 'ImageBuildRequest',
                                resultSchema: 'ImageBuildResult',
                                implemented: true,
                            },
                        ],
                    });
                }
                if (url.pathname.endsWith('/image-build-plans')) {
                    return json({
                        planToken: 'plan-flap',
                        kind: 'HDS',
                        summary: { sizeBytes: 1024, partitionCount: 1 },
                    });
                }
                if (url.pathname.endsWith('/image-builds')) {
                    return json(
                        {
                            jobId: 'job-flap',
                            operationId: 'create.hds',
                            state: 'QUEUED',
                            latestSequence: 0,
                            progress: null,
                            result: null,
                            error: null,
                        },
                        202,
                    );
                }
                if (url.pathname.endsWith('/event-tickets')) {
                    return json(
                        {
                            ticket: `ticket-${FlappingWebSocket.instances}`,
                            websocketUrl: '/api/v1/events',
                            subprotocol: 'axklib.events.v1',
                        },
                        201,
                    );
                }
                if (url.pathname.endsWith('/jobs/job-flap/events')) return json({ events: [] });
                if (url.pathname.endsWith('/jobs/job-flap')) {
                    return json({
                        jobId: 'job-flap',
                        operationId: 'create.hds',
                        state: 'QUEUED',
                        latestSequence: 0,
                        progress: null,
                        result: null,
                        error: null,
                    });
                }
                throw new Error(`unexpected request ${url}`);
            }),
        );

        const transport = new HttpImageTransport({ baseUrl: 'http://localhost/api/v1', bearerToken: 'secret' });
        const manifest = serverFile('authoring/build.json');
        const output = serverFile('images/new.hds');
        await transport.planCreate(manifest, output, false);
        const job = await transport.startCreate(manifest, output, false);
        const waiting = transport.waitForJob(job.jobId, () => undefined);
        const rejected = expect(waiting).rejects.toThrow('Lost the axklib-server event connection');

        await vi.runAllTimersAsync();

        await rejected;
        expect(FlappingWebSocket.instances).toBe(7);
    });

    it('completes from REST reconciliation when the event socket upgrade is rejected', async () => {
        vi.useFakeTimers();
        RejectingWebSocket.instances = 0;
        vi.stubGlobal('WebSocket', RejectingWebSocket as unknown as typeof WebSocket);
        let snapshots = 0;
        vi.stubGlobal(
            'fetch',
            vi.fn(async (input: RequestInfo | URL) => {
                const url = new URL(String(input));
                if (url.pathname.endsWith('/system/capabilities')) {
                    return json({
                        apiVersion: 'v1',
                        limits: {},
                        operations: [
                            {
                                id: 'create.plan',
                                method: 'POST',
                                route: '/api/v1/image-build-plans',
                                mode: 'request',
                                operationClass: 'read',
                                requiresIdempotency: false,
                                variant: null,
                                requestSchema: 'ImageBuildPlanRequest',
                                resultSchema: 'ImageBuildPlan',
                                implemented: true,
                            },
                            {
                                id: 'create.hds',
                                method: 'POST',
                                route: '/api/v1/image-builds',
                                mode: 'job',
                                operationClass: 'write',
                                requiresIdempotency: true,
                                variant: 'HDS',
                                requestSchema: 'ImageBuildRequest',
                                resultSchema: 'ImageBuildResult',
                                implemented: true,
                            },
                        ],
                    });
                }
                if (url.pathname.endsWith('/image-build-plans')) {
                    return json({ planToken: 'plan-reconcile', kind: 'HDS', summary: { sizeBytes: 1 } });
                }
                if (url.pathname.endsWith('/image-builds')) {
                    return json(
                        {
                            jobId: 'job-reconcile',
                            operationId: 'create.hds',
                            state: 'QUEUED',
                            latestSequence: 0,
                            progress: null,
                            result: null,
                            error: null,
                        },
                        202,
                    );
                }
                if (url.pathname.endsWith('/event-tickets')) {
                    return json(
                        {
                            ticket: 'ticket-reconcile',
                            websocketUrl: '/api/v1/events',
                            subprotocol: 'axklib.events.v1',
                        },
                        201,
                    );
                }
                if (url.pathname.endsWith('/jobs/job-reconcile/events')) return json({ events: [] });
                if (url.pathname.endsWith('/jobs/job-reconcile')) {
                    snapshots += 1;
                    const completed = snapshots > 1;
                    return json({
                        jobId: 'job-reconcile',
                        operationId: 'create.hds',
                        state: completed ? 'COMPLETED' : 'QUEUED',
                        latestSequence: 0,
                        progress: null,
                        result: completed ? { output: 'ready' } : null,
                        error: null,
                    });
                }
                throw new Error(`unexpected request ${url}`);
            }),
        );

        const transport = new HttpImageTransport({ baseUrl: 'http://localhost/api/v1', bearerToken: 'secret' });
        const manifest = serverFile('authoring/build.json');
        const output = serverFile('images/new.hds');
        await transport.planCreate(manifest, output, false);
        const job = await transport.startCreate(manifest, output, false);
        const waiting = transport.waitForJob(job.jobId, () => undefined);

        await vi.runAllTimersAsync();

        await expect(waiting).resolves.toMatchObject({ status: 'completed', result: { output: 'ready' } });
        expect(RejectingWebSocket.instances).toBe(1);
    });
});
