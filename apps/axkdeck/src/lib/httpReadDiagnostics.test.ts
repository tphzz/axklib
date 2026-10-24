import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { AxklibHttpApiClient } from './httpApiClient';
import { beginSampleRead, readContext } from './readDiagnostics';
import type { ImageTransport } from './transport';

const diagnostics = vi.hoisted(() => ({ enabled: true, report: vi.fn() }));
vi.mock('./diagnostics', () => ({
    diagnosticsEnabled: () => diagnostics.enabled,
    reportDiagnostic: diagnostics.report,
}));
const identity = { imageId: 'image-a', documentId: 'document-a', revision: 1 };
const transport = { audioIdentity: () => identity } as unknown as ImageTransport;
const path = '/images/image-a/documents/document-a/preview?objectId=sample-a&bins=1024&expectedRevision=1';
const client = () => new AxklibHttpApiClient({ baseUrl: 'http://localhost/api/v1', bearerToken: 'private-token' });
const reply = (data: unknown, status = 200) =>
    new Response(JSON.stringify({ data, meta: { requestId: 'request-7' } }), {
        status,
        headers: { 'Content-Type': 'application/json', 'X-Request-Id': 'request-7' },
    });

beforeEach(() => {
    diagnostics.enabled = true;
    diagnostics.report.mockClear();
    beginSampleRead(transport, 1, 'sample-a');
});
afterEach(() => vi.unstubAllGlobals());

describe('HTTP read diagnostics', () => {
    it('joins response metadata to the selection captured before the asynchronous request', async () => {
        const context = readContext(1, 'sample-a')!;
        let resolve!: (response: Response) => void;
        vi.stubGlobal(
            'fetch',
            vi.fn(
                () =>
                    new Promise<Response>((done) => {
                        resolve = done;
                    }),
            ),
        );
        const pending = client().request('GET', path);
        beginSampleRead(transport, 1, 'sample-b');
        resolve(reply({ objectId: 'sample-a', lanes: [] }));
        await expect(pending).resolves.toMatchObject({ objectId: 'sample-a' });
        const events = diagnostics.report.mock.calls.map(([, fields]) => fields);
        expect(events).toContainEqual(
            expect.objectContaining({
                phase: 'preview-http',
                requestId: 'request-7',
                selectionId: context.selectionId,
                objectId: 'sample-a',
                current: false,
                outcome: 'success',
                bins: 1024,
            }),
        );
        expect(events.map((event) => event.phase)).toEqual(
            expect.arrayContaining(['preview-http/headers', 'preview-http/body-read', 'preview-http/json-parse']),
        );
        expect(JSON.stringify(events)).not.toContain('private-token');
        expect(JSON.stringify(events)).not.toContain('lanes');
    });

    it('keeps errors and cancellation unchanged and logs the failed request ID', async () => {
        vi.stubGlobal(
            'fetch',
            vi.fn(
                async () =>
                    new Response(
                        JSON.stringify({
                            error: { code: 'sample_unreadable', message: 'Cannot read', requestId: 'request-failed' },
                        }),
                        { status: 422 },
                    ),
            ),
        );
        await expect(client().request('GET', path)).rejects.toMatchObject({ code: 'sample_unreadable', status: 422 });
        expect(diagnostics.report.mock.calls.at(-1)![1]).toMatchObject({
            phase: 'preview-http',
            requestId: 'request-failed',
            outcome: 'failed',
        });
        const error = new DOMException('Cancelled', 'AbortError');
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => {
                throw error;
            }),
        );
        await expect(client().request('GET', path)).rejects.toBe(error);
        expect(diagnostics.report.mock.calls.at(-1)![1]).toMatchObject({ outcome: 'cancelled' });
    });

    it('links audition jobs and range headers without consuming binary response contents', async () => {
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => reply({ jobId: 'job-a' })),
        );
        await client().request('POST', '/auditions', { ...identity, objectIds: ['sample-a'] });
        expect(diagnostics.report.mock.calls.at(-1)![1]).toMatchObject({
            phase: 'audition-submit-http',
            jobId: 'job-a',
            requestId: 'request-7',
        });
        const response = new Response(new Uint8Array([1, 2, 3]), {
            status: 206,
            headers: { 'X-Request-Id': 'request-range' },
        });
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => response),
        );
        const result = await client().openAuditionContent('audition-a', 0, 2);
        expect(result.bodyUsed).toBe(false);
        expect(diagnostics.report.mock.calls.at(-1)![1]).toMatchObject({
            phase: 'audition-range-headers',
            auditionId: 'audition-a',
            requestId: 'request-range',
            byteCount: 3,
        });
    });

    it('does not capture unrelated requests or enable extra reads when logging is off', async () => {
        const fetch = vi.fn(async () => reply({ ok: true }));
        vi.stubGlobal('fetch', fetch);
        diagnostics.report.mockClear();
        await client().request('POST', '/unrelated', { credentials: 'not-for-logs' });
        expect(diagnostics.report).not.toHaveBeenCalled();
        diagnostics.enabled = false;
        await client().request('GET', path);
        expect(diagnostics.report).not.toHaveBeenCalled();
        expect(fetch).toHaveBeenCalledTimes(2);
    });
});
