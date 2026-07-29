import { afterEach, describe, expect, it, vi } from 'vitest';

import { AxklibApiError } from './httpErrors';
import { fetchWithTimeout, readBoundedBlob, readBoundedText } from './httpResponse';

describe('bounded HTTP responses', () => {
    afterEach(() => {
        vi.useRealTimers();
        vi.restoreAllMocks();
    });

    it('rejects declared and streamed JSON bodies above the byte limit', async () => {
        await expect(
            readBoundedText(new Response('small', { headers: { 'Content-Length': '100' } }), 10),
        ).rejects.toMatchObject({ code: 'response_too_large' } satisfies Partial<AxklibApiError>);
        await expect(readBoundedText(new Response('eleven-byte'), 10)).rejects.toMatchObject({
            code: 'response_too_large',
        } satisfies Partial<AxklibApiError>);
    });

    it('rejects truncated, excessive, and renderer-unsafe downloads', async () => {
        await expect(readBoundedBlob(new Response('abc'), 4, 4)).rejects.toMatchObject({
            code: 'invalid_download_size',
        } satisfies Partial<AxklibApiError>);
        await expect(readBoundedBlob(new Response('abcde'), 4, 4)).rejects.toMatchObject({
            code: 'invalid_download_size',
        } satisfies Partial<AxklibApiError>);
        await expect(readBoundedBlob(new Response(''), 5, 4)).rejects.toMatchObject({
            code: 'download_too_large',
        } satisfies Partial<AxklibApiError>);
    });

    it('combines caller cancellation with the request timeout', async () => {
        const controller = new AbortController();
        let effectiveSignal: AbortSignal | undefined;
        vi.spyOn(globalThis, 'fetch').mockImplementation(
            (_input, init) =>
                new Promise((_resolve, reject) => {
                    effectiveSignal = init?.signal ?? undefined;
                    effectiveSignal?.addEventListener('abort', () => reject(effectiveSignal?.reason), { once: true });
                }),
        );
        const request = fetchWithTimeout('http://localhost/test', { method: 'GET' }, controller.signal, 60_000);
        controller.abort(new Error('cancelled'));
        await expect(request).rejects.toThrow('cancelled');
        expect(effectiveSignal?.aborted).toBe(true);
    });

    it('aborts a stalled request after the configured timeout', async () => {
        vi.useFakeTimers();
        vi.spyOn(globalThis, 'fetch').mockImplementation(
            (_input, init) =>
                new Promise((_resolve, reject) => {
                    init?.signal?.addEventListener('abort', () => reject(init.signal?.reason), { once: true });
                }),
        );
        const request = fetchWithTimeout('http://localhost/test', { method: 'GET' }, undefined, 1_000);
        const rejection = expect(request).rejects.toMatchObject({ name: 'TimeoutError' });
        await vi.advanceTimersByTimeAsync(1_000);
        await rejection;
    });
});
