import { sampleFormatFixture } from '../../test/sampleFormatFixture';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { fireEvent, render, waitFor } from '@testing-library/svelte';
import SampleFormatDialog from './SampleFormatDialog.svelte';
import { HttpImageTransport } from '../../lib/httpTransport';
import { serverFileLocation } from '../../lib/storageLocations';
import type { ObjectDetail } from '../../lib/transport';
import { ObjectEditorWorkflow } from './workflow.svelte';

function sampleDetail(): ObjectDetail {
    return {
        schemaVersion: 1,
        image: { imageId: 'image-1', revision: 1, format: 'sfs' },
        object: {
            sampleFormat: sampleFormatFixture().sampleFormat,
            id: 'sample-1',
            key: 'SBNK:1',
            type: 'SBNK',
            name: 'Tone',
            format: 'current',
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
                volumeDirectoryId: 1,
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
            payloadSha256: 'a'.repeat(64),
            parameters: { level: 100, pan: 0 },
            blockedParameters: [],
            blockedParameterReasons: {},
            ...sampleFormatFixture(),
            unavailableParameters: {},
            partitionIndex: 0,
            volumeName: 'Volume',
            playbackWindow: { start_frame: 0, length_frames: 100 },
            canEditPlayback: true,
            eqCoefficients: [-15904, 7738, 8192, 15904, -7738],
            maximumFrames: 100,
            sources: [{ frames: 100, objectId: 'wave-1', role: 'mono', sampleRate: 44100 }],
        },
    };
}

// Mock the HTTP boundary, not objectDetail(): the real client unwraps this envelope.
function response(data: unknown): Response {
    return Response.json({ data, meta: { requestId: 'editor-test' } });
}

async function setup(detail: ObjectDetail, failConversion = false) {
    vi.stubGlobal(
        'fetch',
        vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
            const path = new URL(String(input)).pathname;
            if (failConversion && path.endsWith('/system/capabilities'))
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
            if (failConversion && path.endsWith('/image-session-alterations'))
                return response({ jobId: 'conversion-1', operationId: 'images.alter', state: 'QUEUED' });
            if (failConversion && path.endsWith('/jobs/conversion-1/events')) return response({ events: [] });
            if (failConversion && path.endsWith('/jobs/conversion-1'))
                return response({
                    jobId: 'conversion-1',
                    operationId: 'images.alter',
                    state: 'FAILED',
                    error: {
                        code: 'entry_in_use',
                        message: 'close open images and wait for active file operations to finish',
                        retryable: true,
                    },
                });
            if (path.endsWith('/images') && init?.method === 'POST')
                return response({
                    jobId: 'open-1',
                    operationId: 'images.open',
                    state: 'COMPLETED',
                    result: {
                        imageId: 'image-1',
                        revision: 1,
                        format: 'sfs',
                        companionSources: [],
                        floppySet: null,
                        validation: { valid: true, infoCount: 0, warningCount: 0, errorCount: 0 },
                    },
                });
            if (path.endsWith('/content')) return response({ items: [], totalCount: 0, nextCursor: null });
            if (path.endsWith('/objects/sample-1')) return response(detail);
            throw new Error(`Unexpected request: ${init?.method} ${path}`);
        }),
    );
    const transport = new HttpImageTransport({
        baseUrl: 'http://127.0.0.1:4000/api/v1',
        bearerToken: 'test',
        mode: 'local',
    });
    const image = await transport.openImage(serverFileLocation({ rootId: 'test', relativePath: 'test.hds' }));
    const workflow = new ObjectEditorWorkflow({
        transport,
        refresh: vi.fn(),
        stopPlayback: vi.fn(),
        status: vi.fn(),
    });
    return { transport, workflow, sessionId: image.sessionId };
}

describe('Sample editor over HTTP', () => {
    afterEach(() => vi.unstubAllGlobals());

    it('restores dismissal and editing after an HTTP conversion job is rejected by the file lock', async () => {
        const detail = sampleDetail();
        const { workflow, sessionId } = await setup(detail, true);
        await workflow.openConversion(sessionId, 'sample-1');
        const document = workflow.conversionDocument!;
        const view = render(SampleFormatDialog, { workflow, document });
        await fireEvent.click(view.getByRole('button', { name: /^Convert$/ }));
        await waitFor(() => expect(view.getByRole('status').textContent).toContain('Conversion not started'));
        expect(workflow.locked).toBe(false);
        expect(document.jobId).toBeNull();
        expect(document.conversionTarget).toBeNull();
        expect(document.detail).toEqual(detail);
        await fireEvent.click(view.getByRole('button', { name: 'Cancel' }));
        expect(workflow.conversionDocument).toBeNull();
        document.draft.set('level', 80);
        expect(document.canSave).toBe(true);
    });

    it('loads the server snapshot, revalidates and discards through the real transport', async () => {
        const detail = sampleDetail();
        const { transport, workflow, sessionId } = await setup(detail);
        await expect(transport.objectDetail(sessionId, 'sample-1')).resolves.toEqual(detail);
        const document = await workflow.load(sessionId, 'sample-1');
        expect(document).not.toBeNull();
        expect(document!.draft.values.level).toBe(100);
        expect(document!.canSave).toBe(false);
        document!.draft.set('level', 80);
        expect(document!.canSave).toBe(true);
        detail.image.revision = 2;
        await workflow.revalidate(sessionId);
        expect(document!.detail!.image.revision).toBe(2);
        expect(document!.draft.values.level).toBe(80);
        await workflow.discard(document!);
        expect(document!.draft.values.level).toBe(100);
        expect(document!.canSave).toBe(false);
    });

    it.each(['absent', 'null', 'read-only'] as const)(
        'handles an %s editing profile without an exception',
        async (kind) => {
            const detail = sampleDetail();
            if (kind === 'absent') delete detail.editing;
            else if (kind === 'null') detail.editing = null;
            else {
                detail.editing!.editable = false;
                detail.editing!.reason = 'Image is read-only';
            }
            const { workflow, sessionId } = await setup(detail);
            const document = await workflow.load(sessionId, 'sample-1');
            if (kind !== 'read-only') expect(document).toBeNull();
            else {
                document!.draft.set('level', 80);
                expect(document!.canSave).toBe(false);
                expect(document!.validation).toBe('Image is read-only');
            }
        },
    );

    it('edits short Samples without inventing extension fields', async () => {
        const detail = sampleDetail();
        detail.object.storedSizeBytes = 356;
        detail.object.header.layoutSelector0x14 = 2;
        const { workflow, sessionId } = await setup(detail);
        const document = await workflow.load(sessionId, 'sample-1');
        expect(document).not.toBeNull();
        expect(document!.draft.values.output1_destination).toBeUndefined();
        document!.draft.set('level', 80);
        expect(document!.canSave).toBe(true);
        document!.draft.set('output1_destination', 1);
        expect(document!.canSave).toBe(false);
        expect(document!.validation).toContain('unavailable');
    });
});
