import assert from 'node:assert/strict';
import { test } from 'node:test';
import { createServer } from 'vite';

test('cold component styles and concurrent parent requests return compiled CSS', { timeout: 30000 }, async () => {
    const server = await createServer({ server: { port: 0, strictPort: false }, logLevel: 'error' });
    try {
        await server.listen();
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
