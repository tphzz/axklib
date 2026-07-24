import { beforeEach, describe, expect, it, vi } from 'vitest';

import { HttpImageTransport } from './httpTransport';
import { InMemoryImageTransport } from './testing/inMemoryTransport';
import { serverFileLocation } from './storageLocations';
import type { FileLocation } from './storageLocations';
import type { ImageTransport, OpenedImage, SamplerObject } from './transport';

const object: SamplerObject = {
    key: 'sample-1',
    objectType: 'SMPL',
    name: 'Tone',
    partitionIndex: 0,
    partitionName: 'Partition 0',
    volumeName: 'Volume',
    categoryName: 'SMPL',
    sfsId: 9,
    storedSizeBytes: 128,
    sampleRate: 44_100,
    rootKey: 60,
    frameCount: 4,
    sampleWidthBytes: 2,
};

const opened: Omit<OpenedImage, 'sessionId'> = {
    initialVolume: null,
    volumeMutationsAvailable: false,
    partitionMutationsAvailable: false,
    tree: [
        {
            id: 'disk',
            name: 'fixture.hds',
            kind: 'disk',
            childCount: 1,
            children: [{ id: 'volume-1', name: 'Volume', kind: 'volume', childCount: 1 }],
        },
    ],
    validation: {
        valid: true,
        issueCount: 0,
        errorCount: 0,
        warningCount: 0,
        objectCount: 1,
        relationshipCount: 1,
    },
    objects: [object],
    objectTotalCount: 1,
};

async function exerciseReadContract(transport: ImageTransport, source: FileLocation): Promise<void> {
    const image = await transport.openImage(source);
    expect(image.validation).toMatchObject({ valid: true, objectCount: 1 });
    const content = await transport.contentChildren(image.sessionId, '', 0, 64);
    expect(content).toMatchObject({ totalCount: 1 });
    expect(content.items[0]).toMatchObject({ name: 'Volume', kind: 'volume' });

    const objects = await transport.objectPage(image.sessionId, 0, 64);
    expect(objects).toMatchObject({ totalCount: 1 });
    expect(objects.objects[0]).toMatchObject({ objectType: 'SMPL', name: 'Tone' });

    const relationships = await transport.relationshipPage(image.sessionId, 0, 64, { scopeId: 'volume-1' });
    expect(relationships).toMatchObject({ totalCount: 1 });
    expect(relationships.relationships[0]).toMatchObject({
        sourceObjectId: 'sample-1',
        targetObjectId: 'sample-1',
        relationshipType: 'TEST',
    });

    const preview = await transport.preview(image.sessionId, objects.objects[0]!.key, 16);
    expect(preview).toEqual({
        frameCount: 4,
        lanes: [
            {
                role: 'MONO',
                sourceObjectId: 'wave-1',
                frameCount: 4,
                bins: [{ minimum: -1, maximum: 1 }],
            },
        ],
    });
    await transport.closeImage(image.sessionId);
}

function json(body: unknown, status = 200): Response {
    return new Response(JSON.stringify({ data: body }), {
        status,
        headers: { 'content-type': 'application/json' },
    });
}

beforeEach(() => {
    vi.unstubAllGlobals();
});

describe('ImageTransport shared read contract', () => {
    it('passes through the in-memory UI conformance transport', async () => {
        const transport = new InMemoryImageTransport({
            opened,
            preview: {
                frameCount: 4,
                lanes: [
                    {
                        role: 'MONO',
                        sourceObjectId: 'wave-1',
                        frameCount: 4,
                        bins: [{ minimum: -1, maximum: 1 }],
                    },
                ],
            },
            operations: {
                contentChildren: async () => ({
                    items: [{ id: 'volume-1', name: 'Volume', kind: 'volume', childCount: 1 }],
                    totalCount: 1,
                }),
                objectPage: async () => ({ objects: [object], totalCount: 1 }),
                relationshipPage: async () => ({
                    relationships: [
                        {
                            id: 'relationship-1',
                            sourceObjectId: 'sample-1',
                            targetObjectId: 'sample-1',
                            candidateObjectIds: [],
                            relationshipType: 'TEST',
                            quality: 'known',
                            basis: 'test',
                            notes: [],
                            assignmentName: '',
                            assignmentState: '',
                            receiveChannelDisplay: '',
                        },
                    ],
                    totalCount: 1,
                }),
            },
        });
        await exerciseReadContract(
            transport,
            serverFileLocation({ rootId: 'workspace', relativePath: 'images/fixture.hds' }),
        );
    });

    it('passes through the Crow HTTP transport', async () => {
        vi.stubGlobal(
            'fetch',
            vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
                const url = new URL(String(input));
                if (url.pathname.endsWith('/images') && init?.method === 'POST') {
                    return json(
                        {
                            imageId: 'image-1',
                            source: { rootId: 'workspace', relativePath: 'images/fixture.hds' },
                            format: 'SFS',
                            rootCount: 1,
                            objectCount: 1,
                            relationshipCount: 1,
                            validation: { valid: true, infoCount: 0, warningCount: 0, errorCount: 0 },
                        },
                        201,
                    );
                }
                if (url.pathname.endsWith('/content')) {
                    return json({
                        items: [
                            { id: 'volume-1', parentId: null, kind: 'volume', displayName: 'Volume', childCount: 1 },
                        ],
                        totalCount: 1,
                        nextCursor: null,
                    });
                }
                if (url.pathname.endsWith('/objects')) {
                    return json({
                        items: [
                            {
                                id: 'sample-1',
                                type: 'SMPL',
                                name: 'Tone',
                                partitionIndex: 0,
                                partitionName: 'Partition 0',
                                volumeName: 'Volume',
                                categoryName: 'SMPL',
                                sizeBytes: 128,
                                waveform: { sampleRate: 44_100, sampleWidthBytes: 2, rootKey: 60, frameCount: 4 },
                            },
                        ],
                        totalCount: 1,
                        nextCursor: null,
                    });
                }
                if (url.pathname.endsWith('/relationships')) {
                    expect(url.searchParams.get('scopeId')).toBe('volume-1');
                    return json({
                        items: [
                            {
                                id: 'relationship-1',
                                sourceObjectId: 'sample-1',
                                targetObjectId: 'sample-1',
                                candidateObjectIds: [],
                                type: 'TEST',
                                quality: 'known',
                                basis: 'test',
                                notes: [],
                                assignmentIndex: null,
                                assignmentName: '',
                                assignmentState: '',
                                receiveChannelDisplay: '',
                            },
                        ],
                        totalCount: 1,
                        nextCursor: null,
                    });
                }
                if (url.pathname.endsWith('/preview')) {
                    return json({
                        frameCount: 4,
                        lanes: [
                            {
                                role: 'MONO',
                                sourceObjectId: 'wave-1',
                                frameCount: 4,
                                bins: [{ minimum: -1, maximum: 1 }],
                            },
                        ],
                    });
                }
                if (url.pathname.endsWith('/images/image-1') && init?.method === 'DELETE') {
                    return new Response(null, { status: 204 });
                }
                throw new Error(`unexpected HTTP request ${init?.method ?? 'GET'} ${url}`);
            }),
        );
        await exerciseReadContract(
            new HttpImageTransport({ baseUrl: 'http://127.0.0.1:7300/api/v1', bearerToken: 'secret' }),
            serverFileLocation({ rootId: 'workspace', relativePath: 'images/fixture.hds' }),
        );
    });
});
