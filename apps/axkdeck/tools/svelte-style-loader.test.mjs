import assert from 'node:assert/strict';
import { createServer as createHttpServer } from 'node:http';
import { test } from 'node:test';
import { createServer } from 'vite';
import { svelteStyleLoader } from './svelte-style-loader.mjs';

test('cold component styles and concurrent parent requests return compiled CSS', { timeout: 30000 }, async () => {
    const server = await createServer({ server: { middlewareMode: true, ws: false }, logLevel: 'error' });
    try {
        const cold = await server.transformRequest(
            '/src/features/object-editor/EditorBoundary.svelte?svelte&type=style&lang.css',
        );
        assert(cold?.code.includes('__vite__updateStyle'));
        assert(cold.code.includes('.draft-confirmation'));
        assert(!cold.code.includes('<script'));
        for (const name of ['EditorBoundary', 'DeviceEditorHost']) {
            const component = `/src/features/object-editor/${name}.svelte`;
            const [style, script] = await Promise.all([
                server.transformRequest(`${component}?svelte&type=style&lang.css`),
                server.transformRequest(component),
            ]);
            assert(style?.code.includes('__vite__updateStyle'));
            assert(!style.code.includes('<script'));
            assert(script?.code.includes('svelte'));
        }
    } finally {
        await server.close();
    }
});

test('stale stylesheets for components without local styles return empty CSS', { timeout: 30000 }, async () => {
    const server = await createServer({ server: { middlewareMode: true, ws: false }, logLevel: 'error' });
    const http = createHttpServer(server.middlewares);
    try {
        // The test owns its listener, independently of Tauri host settings and localhost DNS.
        await new Promise((resolve, reject) => {
            http.once('error', reject);
            http.listen(0, '127.0.0.1', resolve);
        });
        const address = http.address();
        assert(address && typeof address !== 'string');
        assert.equal(address.address, '127.0.0.1');
        assert(address.port > 0);
        const origin = new URL(`http://${address.address}:${address.port}/`);
        for (const name of ['SampleFormatBadge', 'ProgramFormatBadge']) {
            const component = `/src/features/object-editor/${name}.svelte`;
            const [style, script] = await Promise.all([
                server.transformRequest(`${component}?svelte&type=style&lang.css`),
                server.transformRequest(component),
            ]);
            assert(style?.code.includes('__vite__updateStyle'));
            assert(style.code.includes('const __vite__css = ""'));
            assert(!style.code.includes('<script'));
            assert(script?.code.includes('StorageFormatBadge'));
            const response = await fetch(
                new URL(`${component}?svelte&type=style&lang.css`, origin),
                { signal: AbortSignal.timeout(5000) },
            );
            assert.equal(response.status, 200);
            assert((await response.text()).includes('const __vite__css = ""'));
        }
        const badge = await server.transformRequest(
            '/src/features/object-editor/StorageFormatBadge.svelte?svelte&type=style&lang.css',
        );
        assert(badge?.code.includes('.format-badge'));
    } finally {
        try {
            if (http.listening) await new Promise((resolve, reject) => {
                http.close(error => error ? reject(error) : resolve());
                http.closeAllConnections();
            });
            assert.equal(http.listening, false);
        } finally {
            await server.close();
        }
    }
});

test('component compilation errors are not replaced with empty CSS', async () => {
    const failure = new Error('component compilation failed');
    const loader = svelteStyleLoader();
    loader.configureServer({
        environments: {
            client: {
                async transformRequest() {
                    throw failure;
                },
            },
        },
    });
    await assert.rejects(loader.load('/src/Broken.svelte?svelte&type=style&lang.css'), error => error === failure);
});
