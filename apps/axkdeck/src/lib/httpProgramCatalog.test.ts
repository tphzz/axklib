import { afterEach, describe, expect, it, vi } from 'vitest';
import { ObjectEditorWorkflow } from '../features/object-editor/workflow.svelte';
import { programEditorFixture } from '../test/programEditorFixture';
import { HttpImageTransport } from './httpTransport';

function transport() {
    return new HttpImageTransport({
        baseUrl: 'http://127.0.0.1:8080/api/v1',
        bearerToken: 'catalog-test-token',
        mode: 'local',
    });
}

function response(data: unknown) {
    return new Response(JSON.stringify({ data, meta: { requestId: 'catalog-test' } }), {
        headers: { 'Content-Type': 'application/json' },
    });
}

afterEach(() => vi.unstubAllGlobals());

describe('Program catalog HTTP integration', () => {
    it.each([true, false])('loads editable Program controls from the wire envelope (native=%s)', async (native) => {
        const fixture = programEditorFixture(native);
        const fetch = vi.fn().mockResolvedValue(response(fixture.catalog));
        vi.stubGlobal('fetch', fetch);
        const http = transport();
        const workflow = new ObjectEditorWorkflow({
            transport: {
                objectDetail: async () => fixture.document.detail!,
                programEditorCatalog: () => http.programEditorCatalog(),
                startObjectParameterEdit: vi.fn(),
                waitForJob: vi.fn(),
            },
            refresh: vi.fn(),
            stopPlayback: vi.fn(),
            status: vi.fn(),
        });

        const document = (await workflow.load(1, 'program'))!;
        expect(document.programFormat).toEqual(fixture.format);
        expect(document.validation).toBe('');
        document.draft.set('level', 80);
        expect(document.canSave).toBe(true);
        expect(fetch).toHaveBeenCalledExactlyOnceWith(
            'http://127.0.0.1:8080/api/v1/program-editor-catalog',
            expect.objectContaining({
                method: 'GET',
                headers: { Authorization: 'Bearer catalog-test-token' },
            }),
        );
    });

    it('shares pending and completed catalogs only within the same server client', async () => {
        const { catalog } = programEditorFixture();
        const fetch = vi.fn().mockImplementation(async () => response(catalog));
        vi.stubGlobal('fetch', fetch);
        const http = transport();
        const first = http.programEditorCatalog();
        expect(http.programEditorCatalog()).toBe(first);
        await expect(first).resolves.toEqual(catalog);
        expect(http.programEditorCatalog()).toBe(first);
        expect(fetch).toHaveBeenCalledTimes(1);
        await expect(transport().programEditorCatalog()).resolves.toEqual(catalog);
        expect(fetch).toHaveBeenCalledTimes(2);
    });

    it('evicts failed catalog requests so a subsequent load can retry', async () => {
        const { catalog } = programEditorFixture();
        const fetch = vi
            .fn()
            .mockResolvedValueOnce(
                new Response(JSON.stringify({ error: { code: 'unavailable', message: 'Catalog request failed' } }), {
                    status: 503,
                    headers: { 'Content-Type': 'application/json' },
                }),
            )
            .mockResolvedValueOnce(response(catalog));
        vi.stubGlobal('fetch', fetch);
        const http = transport();
        await expect(http.programEditorCatalog()).rejects.toThrow('Catalog request failed');
        await expect(http.programEditorCatalog()).resolves.toEqual(catalog);
        expect(fetch).toHaveBeenCalledTimes(2);
    });
});
