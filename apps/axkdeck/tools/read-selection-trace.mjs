import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const server = await createServer({ server: { host: '127.0.0.1', strictPort: false } });
let browser;
let port;
let stopped;
const results = [];
const stop = () => stopped ??= (async () => { await browser?.close(); await server.close(); })();
const deadline = setTimeout(() => void stop(), 90000);
const signal = () => void stop();
process.once('SIGTERM', signal);
process.once('SIGINT', signal);
try {
    await server.listen();
    port = server.httpServer.address().port;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    if (stopped) { await browser.close(); throw new Error('Draw trace check interrupted during startup'); }
    for (const width of [390, 800, 1280]) for (const ratio of [1, 2]) {
        const page = await browser.newPage({ viewport: { width, height: 800 }, deviceScaleFactor: ratio });
        page.setDefaultTimeout(10000);
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        await page.goto(`http://127.0.0.1:${port}/tools/layout-fixtures/sample-editor.html`);
        await page.locator('canvas').first().waitFor();
        await page.evaluate(async () => {
            // Only this synthetic fixture gets a fake desktop logging bridge.
            window.readTraceEvents = [];
            window.__TAURI_EVENT_PLUGIN_INTERNALS__ = { unregisterListener() {} };
            window.__TAURI_INTERNALS__ = {
                transformCallback: () => 1,
                invoke: async (command, args) => {
                    if (command === 'diagnostic_log_level') return 'debug';
                    if (command === 'plugin:event|listen') return 1;
                    if (command === 'plugin:log|log') window.readTraceEvents.push(JSON.parse(args.message));
                },
            };
            const diagnostics = await import('/src/lib/diagnostics.ts');
            await diagnostics.installDiagnostics();
            if (!diagnostics.diagnosticsEnabled()) throw new Error('Debug capture did not activate');
        });
        await page.getByRole('button', { name: 'Select B', exact: true }).click();
        const selections = [];
        for (const objectId of ['Sample A', 'Sample B', 'Sample A']) {
            const selectionId = await page.evaluate(async (id) => {
                const trace = await import('/src/lib/readDiagnostics.ts');
                trace.beginSampleRead({ audioIdentity: () => ({ imageId: 'image-1', documentId: 'document-1', revision: 1 }) }, 1, id);
                return trace.readContext(1, id).selectionId;
            }, objectId);
            await page.getByRole('button', { name: objectId === 'Sample A' ? 'Select A' : 'Select B', exact: true }).click();
            await page.waitForFunction(id => window.readTraceEvents.some(event => event.selectionId === id && event.phase === 'waveform-first-draw'), selectionId);
            const events = await page.evaluate(id => window.readTraceEvents.filter(event => event.selectionId === id), selectionId);
            const first = events.filter(event => event.phase === 'waveform-first-draw');
            assert.equal(first.length, 1);
            assert.equal(first[0].objectId, objectId);
            assert.equal(first[0].current, true);
            assert(first[0].bins > 0 && first[0].width > 1 && first[0].height > 1);
            selections.push({ objectId, selectionId, firstDraw: first[0] });
        }
        const pixels = await page.locator('canvas').first().evaluate(canvas => {
            const data = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
            let visible = 0;
            for (let index = 3; index < data.length; index += 4) if (data[index]) visible++;
            return visible;
        });
        assert(pixels > 100);
        assert.deepEqual(errors, []);
        assert.equal(await page.getByLabel('Write count', { exact: true }).textContent(), '0');
        await page.screenshot({ path: resolve(output, `${width}-dpr-${ratio}.png`) });
        results.push({ width, ratio, pixels, selections, errors });
        console.log(`PASS ${width} DPR ${ratio}: A/B/A draw traces and waveform pixels`);
        await page.close();
    }
} finally {
    clearTimeout(deadline);
    process.removeListener('SIGTERM', signal);
    process.removeListener('SIGINT', signal);
    await stop();
    await writeFile(resolve(output, 'results.json'), JSON.stringify({ port, serverStopped: true, results }, null, 2) + '\n');
}
