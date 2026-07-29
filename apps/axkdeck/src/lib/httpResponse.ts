import { AxklibApiError } from './httpErrors';

export const requestTimeoutMs = 60_000;
export const downloadTimeoutMs = 5 * 60_000;
export const maximumJsonResponseBytes = 16 * 1024 * 1024;

function validateDeclaredSize(response: Response, expectedBytes: number): void {
    const declared = response.headers.get('Content-Length');
    if (declared === null) return;
    const declaredBytes = Number(declared);
    if (!Number.isSafeInteger(declaredBytes) || declaredBytes !== expectedBytes) {
        throw new AxklibApiError(
            'invalid_download_size',
            'axklib-server returned inconsistent download metadata',
            response.status,
        );
    }
}

export async function fetchWithTimeout(
    url: string,
    init: Omit<RequestInit, 'signal'>,
    signal: AbortSignal | undefined,
    timeoutMs: number,
): Promise<Response> {
    if (!Number.isSafeInteger(timeoutMs) || timeoutMs <= 0) {
        throw new Error('Invalid HTTP request timeout');
    }
    const controller = new AbortController();
    const forwardCancellation = (): void => controller.abort(signal?.reason);
    if (signal?.aborted) forwardCancellation();
    else signal?.addEventListener('abort', forwardCancellation, { once: true });
    const timeout = setTimeout(
        () => controller.abort(new DOMException('The HTTP request timed out', 'TimeoutError')),
        timeoutMs,
    );
    try {
        return await fetch(url, { ...init, signal: controller.signal });
    } finally {
        clearTimeout(timeout);
        signal?.removeEventListener('abort', forwardCancellation);
    }
}

export async function readBoundedBlob(response: Response, expectedBytes: number, maximumBytes: number): Promise<Blob> {
    if (
        !Number.isSafeInteger(expectedBytes) ||
        expectedBytes < 0 ||
        !Number.isSafeInteger(maximumBytes) ||
        maximumBytes < 0 ||
        expectedBytes > maximumBytes
    ) {
        throw new AxklibApiError('download_too_large', 'Download exceeds the renderer memory limit', 413);
    }
    validateDeclaredSize(response, expectedBytes);
    if (!response.body) {
        if (expectedBytes === 0) return new Blob([], { type: response.headers.get('Content-Type') ?? '' });
        throw new AxklibApiError('invalid_download_body', 'axklib-server returned no download body', response.status);
    }
    const chunks: Uint8Array[] = [];
    const reader = response.body.getReader();
    let received = 0;
    try {
        while (true) {
            const next = await reader.read();
            if (next.done) break;
            received += next.value.byteLength;
            if (received > expectedBytes || received > maximumBytes) {
                await reader.cancel();
                throw new AxklibApiError(
                    'invalid_download_size',
                    'axklib-server returned more download data than declared',
                    response.status,
                );
            }
            chunks.push(next.value);
        }
    } finally {
        reader.releaseLock();
    }
    if (received !== expectedBytes) {
        throw new AxklibApiError(
            'invalid_download_size',
            'axklib-server returned less download data than declared',
            response.status,
        );
    }
    const bytes = new Uint8Array(received);
    let offset = 0;
    for (const chunk of chunks) {
        bytes.set(chunk, offset);
        offset += chunk.byteLength;
    }
    return new Blob([bytes.buffer], { type: response.headers.get('Content-Type') ?? '' });
}

export async function readBoundedText(response: Response, maximumBytes: number): Promise<string> {
    const declared = response.headers.get('Content-Length');
    if (declared !== null) {
        const declaredBytes = Number(declared);
        if (!Number.isSafeInteger(declaredBytes) || declaredBytes < 0 || declaredBytes > maximumBytes) {
            throw new AxklibApiError(
                'response_too_large',
                'axklib-server response exceeds the client JSON limit',
                response.status,
            );
        }
    }
    if (!response.body) return '';
    const reader = response.body.getReader();
    const decoder = new TextDecoder();
    let received = 0;
    let result = '';
    try {
        while (true) {
            const next = await reader.read();
            if (next.done) break;
            received += next.value.byteLength;
            if (received > maximumBytes) {
                await reader.cancel();
                throw new AxklibApiError(
                    'response_too_large',
                    'axklib-server response exceeds the client JSON limit',
                    response.status,
                );
            }
            result += decoder.decode(next.value, { stream: true });
        }
    } finally {
        reader.releaseLock();
    }
    return result + decoder.decode();
}
