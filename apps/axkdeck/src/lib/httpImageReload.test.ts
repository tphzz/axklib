import { afterEach, expect, it, vi } from 'vitest';
import { nativeWorkspaceFixture } from '../test/nativeWorkspaceFixture';
import { HttpImageTransport } from './httpTransport';
import { serverFileLocation } from './storageLocations';

const target = serverFileLocation({ rootId: 'workspace', relativePath: 'target.hds' });
const transport = (port = 4000) =>
    new HttpImageTransport({ baseUrl: `http://localhost:${port}/api/v1`, bearerToken: 'test', mode: 'remote' });
const hide = (persisted: boolean) => window.dispatchEvent(Object.assign(new Event('pagehide'), { persisted }));

function server() {
    const calls: string[] = [];
    let next = 0;
    let busy = false;
    const fixture = nativeWorkspaceFixture();
    vi.stubGlobal('fetch', async (url: string, init: RequestInit) => {
        const { port, pathname } = new URL(url);
        expect(new Headers(init.headers).get('Authorization')).toBe('Bearer test');
        calls.push(`${port} ${init.method} ${pathname}${init.keepalive ? ' keepalive' : ''}`);
        if (init.method === 'DELETE') {
            if (init.keepalive) throw new TypeError('Window was unloaded');
            if (busy)
                return Response.json(
                    { error: { code: 'entry_in_use', message: 'Mutation in progress' } },
                    { status: 409 },
                );
            return Response.json({ data: {} });
        }
        if (pathname.endsWith('/images'))
            return Response.json({
                data: {
                    jobId: `job-${++next}`,
                    operationId: 'images.open',
                    state: 'COMPLETED',
                    latestSequence: 1,
                    result: {
                        ...fixture,
                        imageId: `image-${next}`,
                        documents: [{ ...fixture.documents[0], formatId: 'yamaha.tx16w' }],
                    },
                },
            });
        throw new Error(`Unexpected request: ${pathname}`);
    });
    return { calls, setBusy: (value: boolean) => (busy = value) };
}

afterEach(() => {
    vi.unstubAllGlobals();
    sessionStorage.clear();
});

it('releases only departed sessions before reopening a target, even when the unload request was lost', async () => {
    const { calls } = server();
    const first = transport();
    const old = await first.openImage([target]);
    await first.closeImage(old.sessionId);
    await first.openImage([target]);
    calls.length = 0;
    window.dispatchEvent(new Event('beforeunload', { cancelable: true }));
    hide(true);
    expect(calls).toEqual([]);
    hide(false);
    expect(calls).toEqual(['4000 DELETE /api/v1/images/image-2 keepalive']);
    const other = transport(5000);
    const different = await other.openImage([target]);
    await other.closeImage(different.sessionId);
    const next = transport();
    const reopened = await next.openImage([target]);
    expect(calls.slice(1)).toEqual([
        '5000 POST /api/v1/images',
        '5000 DELETE /api/v1/images/image-3',
        '4000 DELETE /api/v1/images/image-2',
        '4000 POST /api/v1/images',
    ]);
    await next.closeImage(reopened.sessionId);
});

it('never closes a live reader and keeps a busy departing session for a later retry', async () => {
    const { calls, setBusy } = server();
    const first = transport();
    await first.openImage([target]);
    const live = transport();
    const reader = await live.openImage([target]);
    expect(calls).toEqual(['4000 POST /api/v1/images', '4000 POST /api/v1/images']);
    await live.closeImage(reader.sessionId);
    hide(false);
    const next = transport();
    setBusy(true);
    await expect(next.openImage([target])).rejects.toMatchObject({ code: 'entry_in_use' });
    setBusy(false);
    const reopened = await next.openImage([target]);
    expect(calls.slice(-3)).toEqual([
        '4000 DELETE /api/v1/images/image-1',
        '4000 DELETE /api/v1/images/image-1',
        '4000 POST /api/v1/images',
    ]);
    await next.closeImage(reopened.sessionId);
});

it('does not release physical devices on page exit', async () => {
    const { calls } = server();
    await transport().openImage([
        { kind: 'device', reference: { sessionId: 'owned', targetId: 'disk' }, displayName: 'USB disk' },
    ]);
    hide(false);
    const next = transport();
    const opened = await next.openImage([target]);
    expect(calls).toEqual(['4000 POST /api/v1/images', '4000 POST /api/v1/images']);
    await next.closeImage(opened.sessionId);
});
