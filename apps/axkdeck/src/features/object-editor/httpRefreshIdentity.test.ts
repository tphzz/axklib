import { afterEach, describe, expect, it, vi } from 'vitest';
import { HttpImageTransport } from '../../lib/httpTransport';
import type { ApiContentItem, ApiImageSummary, ApiObjectItem } from '../../lib/httpTransportModels';
import type { SampleStorageFormat } from '../../lib/objectEditing';
import { serverFileLocation } from '../../lib/storageLocations';
import type { ObjectDetail } from '../../lib/transport';
import { sampleFormatFixture } from '../../test/sampleFormatFixture';
import { CatalogWorkflow } from '../catalog/workflow.svelte';
import { PickerController } from '../dialogs/picker';
import { ImageSessionWorkflow } from '../image-session/workflow.svelte';
import { ObjectEditorWorkflow } from './workflow.svelte';

type KnownFormat = Exclude<SampleStorageFormat, 'UNKNOWN'>;

function response(data: unknown): Response {
    return Response.json({ data, meta: { requestId: 'refresh-identity-test' } });
}

function page(items: unknown[]): Response {
    return response({ items, totalCount: items.length, nextCursor: null });
}

function detail(format: KnownFormat, revision: number, level: number): ObjectDetail {
    const formats = sampleFormatFixture(format);
    return {
        schemaVersion: 1,
        image: { imageId: 'image-1', revision, format: 'sfs' },
        object: {
            id: 'sample-1',
            key: 'SBNK:1',
            type: 'SBNK',
            name: 'Tone',
            format: 'current',
            sampleFormat: formats.sampleFormat,
            partitionIndex: 0,
            scopeKey: 'partition:0',
            sfsId: 1,
            storedSizeBytes: 512,
            placementResolution: 'EXACT',
            placementCandidates: [],
            omissions: [],
            placement: {
                categoryName: 'SBNK',
                containerDirectory: '/Volume/SBNK',
                entryName: 'Tone',
                partitionIndex: 0,
                partitionName: 'Partition',
                volumeDirectoryId: 17,
                volumeName: 'Volume',
            },
            descriptor: {
                dataOffsetBytes: 0,
                groupLabel: 'SBNK',
                groupLabelBasis: 'directory',
                groupLabelStatus: 'exact',
                logicalPath: '/Volume/SBNK/Tone',
                rawGroup: 'SBNK',
                rawVolume: 'Volume',
                scopeKey: 'partition:0',
                volumeLabel: 'Volume',
                volumeLabelBasis: 'directory',
                volumeLabelStatus: 'exact',
            },
            header: {
                headerSizeBytes: 168,
                layoutSelector0x14: 4,
                name: 'Tone',
                payloadBytes0x1c: 344,
                payloadBytes0x20: 0,
                payloadOffset0x24: 168,
                rawPrefixHex: '53424e4b',
                rawType: 'SBNK',
                recordSizeOrHeaderUsed0x18: 512,
            },
            decoded: { kind: 'sample' },
        },
        relationships: [],
        editing: {
            profile: 'a-series/sample',
            editable: true,
            reason: '',
            payloadSha256: String(revision).repeat(64),
            parameters: { level, pan: 0 },
            blockedParameters: [],
            blockedParameterReasons: {},
            ...formats,
            unavailableParameters: {},
            partitionIndex: 0,
            volumeName: 'Volume',
            playbackWindow: { start_frame: 0, length_frames: 100 },
            canEditPlayback: true,
            eqCoefficients: [-15904, 7738, 8192, 15904, -7738],
            maximumFrames: 100,
            sources: [],
        },
    };
}

async function setup(initialFormat: KnownFormat, failFirstCatalogRefresh = false) {
    let revision = 1;
    let format = initialFormat;
    let level = 100;
    let failedRead = false;
    const mutations: Array<Record<string, unknown>> = [];
    const readScopes: string[] = [];
    const currentVolume = () => `volume-r${revision}`;
    const summary = (): ApiImageSummary => ({
        imageId: 'image-1',
        revision,
        format: 'sfs',
        source: { kind: 'FILE', file: { rootId: 'test', relativePath: 'test.hds' } },
        companionSources: [],
        floppySet: null,
        rootCount: 1,
        objectCount: 1,
        relationshipCount: 0,
        validation: { valid: true, infoCount: 0, warningCount: 0, errorCount: 0 },
    });
    const content = (kind: 'partition' | 'volume' | 'object'): ApiContentItem => ({
        id: kind === 'volume' ? currentVolume() : `${kind}-r${revision}`,
        kind,
        name: kind === 'volume' ? 'Volume' : kind === 'object' ? 'Tone' : 'Partition',
        displayName: kind,
        parentId: null,
        depth: 0,
        childCount: kind === 'object' ? 0 : 1,
        objectId: kind === 'object' ? 'sample-1' : null,
        objectType: kind === 'object' ? 'SBNK' : null,
        partitionIndex: 0,
        volumeDirectoryId: kind === 'volume' ? 17 : null,
        partitionCapacity: null,
        scopeRole: 'CONTAINED',
        sizeBytes: null,
        basis: '',
        quality: 'KNOWN',
        details: [],
        notes: [],
    });
    const sample = (): ApiObjectItem => ({
        id: 'sample-1',
        type: 'SBNK',
        name: 'Tone',
        entryName: 'Tone',
        format: 'current',
        sampleFormat: sampleFormatFixture(format).sampleFormat,
        partitionIndex: 0,
        partitionName: 'Partition',
        volumeName: 'Volume',
        categoryName: 'SBNK',
        sizeBytes: 512,
        sizeWithDependenciesBytes: 512,
        waveform: null,
        sequence: null,
    });
    // Only the fetch boundary is replaced; all identity, refresh and write-state workflows are real.
    vi.stubGlobal(
        'fetch',
        vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
            const url = new URL(String(input));
            const path = url.pathname;
            if (path.endsWith('/system/capabilities'))
                return response({
                    apiVersion: 'v1',
                    operations: [
                        {
                            id: 'images.alter',
                            method: 'POST',
                            route: '/api/v1/image-session-alterations',
                            implemented: true,
                        },
                    ],
                });
            if (path.endsWith('/images') && init?.method === 'POST')
                return response({
                    jobId: 'open-1',
                    operationId: 'images.open',
                    state: 'COMPLETED',
                    result: summary(),
                });
            if (path.endsWith('/images/image-1')) return init?.method === 'DELETE' ? response({}) : response(summary());
            if (path.endsWith('/content')) {
                const parent = url.searchParams.get('parentId');
                if (!parent) return page([content('partition')]);
                if (parent === `partition-r${revision}`) return page([content('volume')]);
                if (parent === currentVolume()) return page([content('object')]);
                throw new Error(`Stale content parent: ${parent}`);
            }
            if (path.endsWith('/objects/sample-1')) return response(detail(format, revision, level));
            if (path.endsWith('/objects') || path.endsWith('/relationships')) {
                const scope = url.searchParams.get('scopeId')!;
                readScopes.push(scope);
                if (scope !== currentVolume()) throw new Error(`Stale catalog scope: ${scope}`);
                if (path.endsWith('/objects') && revision > 1 && failFirstCatalogRefresh && !failedRead) {
                    failedRead = true;
                    return Response.json(
                        {
                            error: {
                                code: 'temporarily_unavailable',
                                message: 'Temporary catalog read failure',
                                retryable: true,
                            },
                        },
                        { status: 503 },
                    );
                }
                return page(path.endsWith('/objects') ? [sample()] : []);
            }
            if (path.endsWith('/system-program-contexts'))
                return response({ partitionIndex: 0, files: [], message: '' });
            if (path.endsWith('/image-session-alterations')) {
                const submitted = JSON.parse(String(init?.body));
                expect(submitted.expectedRevision).toBe(revision);
                const operation = submitted.manifest.inline.operations[0];
                expect(operation.expected_payload_sha256).toBe(String(revision).repeat(64));
                mutations.push(operation);
                if (operation.type === 'convert_sbnk_format')
                    format = operation.target_format === 'a3000_188' ? 'A3000_188' : 'A4000_A5000_224';
                else level = operation.parameters.level;
                revision++;
                return response({ jobId: `write-${mutations.length}`, operationId: 'images.alter', state: 'QUEUED' });
            }
            if (/\/jobs\/write-\d+\/events$/.test(path)) return response({ events: [] });
            if (/\/jobs\/write-\d+$/.test(path))
                return response({
                    jobId: path.split('/').at(-1),
                    operationId: 'images.alter',
                    state: 'COMPLETED',
                    result: summary(),
                });
            throw new Error(`Unexpected request: ${init?.method} ${url}`);
        }),
    );
    const transport = new HttpImageTransport({
        baseUrl: 'http://127.0.0.1:4000/api/v1',
        bearerToken: 'test',
        mode: 'local',
    });
    const image = new ImageSessionWorkflow(transport, new PickerController(() => {}));
    const catalog = new CatalogWorkflow({
        transport,
        sessionId: () => image.sessionId,
        stopPlayback: async () => {},
        resetPreviews: () => {},
        resetCleanup: () => {},
        setStatus: (status) => image.setStatus(status),
    });
    const clearExportSelection = vi.fn();
    image.connect({
        catalog,
        audition: { invalidateSession: async () => {} },
        mutation: { setCapabilities: () => {} },
        clearExportSelection,
    } as unknown as Parameters<ImageSessionWorkflow['connect']>[0]);
    await image.open(serverFileLocation({ rootId: 'test', relativePath: 'test.hds' }));
    expect(image.status).not.toContain('failed');
    expect(catalog.samples.map((item) => item.objectId)).toEqual(['sample-1']);
    catalog.selectedSampleId = 'sample-1';
    catalog.inspectorObjectId = 'sample-1';
    catalog.editorObjectIds.samples = 'sample-1';
    clearExportSelection.mockClear();
    const editor = new ObjectEditorWorkflow({
        transport,
        refresh: () => image.refresh(image.currentSourcePreference(), 'editor'),
        stopPlayback: () => {},
        status: (status) => image.setStatus(status),
    });
    const navigation = editor.navigation('a-series/sample');
    navigation.selectTab('filter');
    navigation.page = 'sample-eq';
    await editor.openConversion(image.sessionId!, 'sample-1');
    const document = editor.conversionDocument!;
    return { image, catalog, editor, navigation, document, mutations, readScopes, clearExportSelection };
}

describe('Sample conversion refresh across regenerated content identities', () => {
    const opened: ImageSessionWorkflow[] = [];
    afterEach(async () => {
        for (const image of opened.splice(0)) await image.dispose();
        vi.unstubAllGlobals();
    });

    it.each([
        ['A3000_188', 'A4000_A5000_224'],
        ['A4000_A5000_224', 'A3000_188'],
    ] as const)(
        'retains the selected Sample and page after %s to %s conversion and another save',
        async (source, target) => {
            const state = await setup(source);
            opened.push(state.image);
            const { image, catalog, editor, document, navigation, mutations } = state;
            await editor.convert(document, target);
            expect(document.status).not.toContain('refresh failed');
            expect(editor.conversionDocument).toBeNull();
            expect(editor.locked).toBe(false);
            expect(document.detail!.editing!.sampleFormat.format).toBe(target);
            expect(catalog.samples[0]!.object.sampleFormat!.format).toBe(target);
            expect(image.selectedSource.id).toBe('volume-r2');
            expect(image.volumeSelection.items.map((item) => item.id)).toEqual(['volume-r2']);
            expect(image.volumeSelection.anchorId).toBe('volume-r2');
            expect(catalog.activeVolumeId).toBe('volume-r2');
            expect(catalog.selectedSampleId).toBe('sample-1');
            expect(catalog.inspectorObjectId).toBe('sample-1');
            expect(catalog.editorObjectIds.samples).toBe('sample-1');
            expect(navigation.tab).toBe('filter');
            expect(navigation.page).toBe('sample-eq');
            expect(state.clearExportSelection).not.toHaveBeenCalled();
            document.draft.set('level', 80);
            expect(document.canSave).toBe(true);
            await editor.save(document);
            expect(document.phase).toBe('editable');
            expect(document.draft.dirty).toBe(false);
            expect(document.draft.values.level).toBe(80);
            expect(document.detail!.image.revision).toBe(3);
            expect(image.selectedSource.id).toBe('volume-r3');
            expect(catalog.activeVolumeId).toBe('volume-r3');
            expect(catalog.selectedSampleId).toBe('sample-1');
            expect(navigation.page).toBe('sample-eq');
            expect(mutations).toHaveLength(2);
            expect(state.readScopes).toContain('volume-r3');
        },
    );

    it('retries a failed catalog refresh using the new scope without repeating the committed conversion', async () => {
        const { image, catalog, editor, document, mutations } = await setup('A4000_A5000_224', true);
        opened.push(image);
        await editor.convert(document, 'A3000_188');
        expect(document.phase).toBe('refresh-failed');
        expect(document.status).toContain('Temporary catalog read failure');
        expect(mutations).toHaveLength(1);
        expect(image.selectedSource.id).toBe('volume-r1');
        expect(catalog.activeVolumeId).toBe('volume-r1');
        expect(catalog.selectedSampleId).toBe('sample-1');
        await editor.recover(document);
        expect(mutations).toHaveLength(1);
        expect(editor.conversionDocument).toBeNull();
        expect(document.phase).toBe('editable');
        expect(document.detail!.editing!.sampleFormat.format).toBe('A3000_188');
        expect(image.selectedSource.id).toBe('volume-r2');
        expect(catalog.activeVolumeId).toBe('volume-r2');
        expect(catalog.selectedSampleId).toBe('sample-1');
    });
});
