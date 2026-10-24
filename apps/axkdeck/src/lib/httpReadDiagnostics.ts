import { diagnosticsEnabled } from './diagnostics';
import { finishReadTrace, readContext, readTrace, type ReadTrace } from './readDiagnostics';

function requestTrace(method: string, path: string, body: unknown): ReadTrace | undefined {
    if (!diagnosticsEnabled()) return undefined;
    try {
        const url = new URL(path, 'http://diagnostics.invalid');
        const match = url.pathname.match(/\/images\/([^/]+)\/documents\/([^/]+)\/objects\/([^/]+)$/);
        if (method === 'GET' && match) {
            const identity = {
                imageId: decodeURIComponent(match[1]!),
                documentId: decodeURIComponent(match[2]!),
                ...(url.searchParams.has('expectedRevision')
                    ? { revision: Number(url.searchParams.get('expectedRevision')) }
                    : {}),
            };
            const objectId = decodeURIComponent(match[3]!);
            return readTrace('object-http', { ...identity, objectId }, readContext(null, objectId, identity));
        }
        const preview = url.pathname.endsWith('/image-waveform-previews');
        if (method === 'POST' && (preview || url.pathname.endsWith('/auditions')) && body && typeof body === 'object') {
            const input = body as Record<string, unknown>;
            const objectId =
                preview && typeof input.objectId === 'string'
                    ? input.objectId
                    : Array.isArray(input.objectIds) && typeof input.objectIds[0] === 'string'
                      ? input.objectIds[0]
                      : '';
            const identity =
                typeof input.imageId === 'string' && typeof input.documentId === 'string'
                    ? {
                          imageId: input.imageId,
                          documentId: input.documentId,
                          ...(typeof input.expectedRevision === 'number' ? { revision: input.expectedRevision } : {}),
                      }
                    : undefined;
            return readTrace(
                preview ? 'preview-submit-http' : 'audition-submit-http',
                {
                    ...identity,
                    objectId,
                    ...(preview && typeof input.bins === 'number' ? { bins: input.bins } : {}),
                    ...(['SELECTED', 'REFINEMENT', 'PLAYBACK', 'PREFETCH'].includes(String(input.readIntent))
                        ? { readIntent: input.readIntent }
                        : {}),
                },
                identity ? readContext(null, objectId, identity) : undefined,
            );
        }
    } catch {
        // Malformed input remains the request/contract owner's responsibility.
    }
    return undefined;
}

export function captureReadHttp<T>(
    method: string,
    path: string,
    body: unknown,
    fetch: () => Promise<Response>,
    decode: (response: Response, trace?: ReadTrace) => Promise<T>,
): Promise<T> {
    const trace = requestTrace(method, path, body);
    if (!trace) return fetch().then((response) => decode(response));
    return finishReadTrace(trace, async () => {
        const response = await fetch();
        trace.mark('headers', {
            requestId: response.headers.get('X-Request-Id') ?? undefined,
            status: response.status,
        });
        return decode(response, trace);
    });
}

export function captureAuditionHeaders(
    auditionId: string,
    start: number,
    end: number,
    fetch: () => Promise<Response>,
): Promise<Response> {
    const trace = readTrace('audition-range-headers', { auditionId, offset: start, byteCount: end - start + 1 });
    if (!trace) return fetch();
    return finishReadTrace(trace, async () => {
        const response = await fetch();
        trace.mark('headers', {
            requestId: response.headers.get('X-Request-Id') ?? undefined,
            status: response.status,
        });
        trace.finish(response.ok ? 'success' : 'failed');
        return response;
    });
}

export function parsedReadEnvelope(trace: ReadTrace | undefined, document: unknown, response: Response): void {
    if (!trace || !document || typeof document !== 'object') return;
    const envelope = document as { meta?: { requestId?: unknown }; data?: unknown; error?: { requestId?: unknown } };
    const requestId = envelope.meta?.requestId ?? envelope.error?.requestId ?? response.headers.get('X-Request-Id');
    const jobId =
        envelope.data && typeof envelope.data === 'object' && 'jobId' in envelope.data
            ? envelope.data.jobId
            : undefined;
    trace.mark('json-parse', {
        ...(typeof requestId === 'string' ? { requestId: requestId.slice(0, 256) } : {}),
        ...(typeof jobId === 'string' ? { jobId: jobId.slice(0, 256) } : {}),
    });
}
