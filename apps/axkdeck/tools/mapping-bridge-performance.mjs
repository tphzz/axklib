import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createConnection, createServer as createSocketServer } from 'node:net';
import { createServer } from 'vite';

assert.ok(process.argv[2], 'Provide a build/reports output directory');
const output = resolve(process.argv[2]); await mkdir(output, { recursive: true });
const { chromium } = await import(pathToFileURL(process.env.PLAYWRIGHT_MODULE).href);
const availablePort = await new Promise((resolve, reject) => { const socket = createSocketServer(); socket.once('error', reject); socket.listen(0, '127.0.0.1', () => { const port = socket.address().port; socket.close(error => error ? reject(error) : resolve(port)); }); });
const server = await createServer({ server: { host: '127.0.0.1', port: availablePort, strictPort: true } });
let browser, port, stopping;
const results = [];
const stop = () => stopping ??= (async () => { try { await browser?.close(); } finally { await server.close(); } })();
const terminate = () => { process.exitCode = 1; void stop(); };
process.once('SIGTERM', terminate); process.once('SIGINT', terminate);
const deadline = setTimeout(terminate, 150000);
const summarize = values => { values.sort((a, b) => a-b); return { median: values[Math.floor(values.length / 2)], p95: values[Math.ceil(values.length * .95) - 1], max: values.at(-1) }; };
try {
    await server.listen(); port = server.httpServer.address().port;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    for (const count of [8, 32, 128, 512]) for (const dense of [false, true]) {
        const context = await browser.newContext({ viewport: { width: 1040, height: 680 } });
        const main = await context.newPage(), errors = [];
        main.on('pageerror', error => errors.push(error.message));
        const query = `benchmark&count=${count}&channel=bridge-${count}-${dense}${dense ? '&dense' : ''}`;
        await main.goto(`http://127.0.0.1:${port}/tools/layout-fixtures/mapping-window.html?${query}`);
        const opened = context.waitForEvent('page'); await main.getByRole('button', { name: 'Mapping Editor', exact: true }).click();
        const child = await opened; child.on('pageerror', error => errors.push(error.message));
        await child.setViewportSize({ width: 1040, height: 680 }); await child.locator('.zone').first().waitFor();
        const measurements = await child.evaluate(async count => {
            const zones = [...document.querySelectorAll('.zone')], frame = () => new Promise(requestAnimationFrame);
            const feedback = [], sync = [], longTasks = [];
            const observer = new PerformanceObserver(list => longTasks.push(...list.getEntries().map(entry => entry.duration))); observer.observe({ type: 'longtask' });
            for (let index = 0; index < 35; index++) {
                const ordinal = (index + 1) % count, zone = zones[ordinal], start = performance.now(); zone.click();
                await frame(); const painted = performance.now() - start;
                if (!zone.classList.contains('chosen')) throw new Error('Local selection did not paint');
                while (JSON.parse(document.querySelector('[data-mapping-snapshot]').textContent).state.selectionId !== ordinal) {
                    if (performance.now() - start > 2000) throw new Error('Selection bridge did not synchronize');
                    await new Promise(resolve => setTimeout(resolve, 0));
                }
                if (index >= 5) { feedback.push(painted); sync.push(performance.now() - start); }
                if (index === 4) { observer.takeRecords(); longTasks.length = 0; }
            }
            await new Promise(resolve => setTimeout(resolve, 0));
            longTasks.push(...observer.takeRecords().map(entry => entry.duration));
            observer.disconnect(); return { feedback, sync, longTasks };
        }, count);
        const result = { count, dense, selectionFrameMs: summarize(measurements.feedback), synchronizationMs: summarize(measurements.sync), longTasksMs: measurements.longTasks };
        results.push(result); console.log(JSON.stringify(result)); assert.deepEqual(errors, []);
        if (process.argv.includes('--verify')) { assert.ok(result.selectionFrameMs.p95 < 33, 'Selection feedback must meet 33 ms p95'); assert.ok(result.synchronizationMs.p95 < 50, 'Complete bridge synchronization must meet 50 ms p95'); assert.equal(result.longTasksMs.length, 0); }
        await context.close();
    }
} finally {
    clearTimeout(deadline); await stop();
    const portReleased = port === undefined || await new Promise(resolve => { const socket = createConnection({ host: '127.0.0.1', port }); socket.once('connect', () => { socket.destroy(); resolve(false); }); socket.once('error', error => { socket.destroy(); resolve(error.code === 'ECONNREFUSED'); }); socket.setTimeout(2000, () => { socket.destroy(); resolve(false); }); });
    await writeFile(resolve(output, 'bridge-performance.json'), JSON.stringify({ transport: 'JSON BroadcastChannel adapter, real MappingClient and MappingController', port, portReleased, browserStopped: !browser?.isConnected(), serverStopped: !server.httpServer?.listening, results }, null, 2) + '\n');
    assert.ok(portReleased, 'Owned benchmark port must be released');
}
