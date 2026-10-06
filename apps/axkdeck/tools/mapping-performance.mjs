import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createConnection, createServer as createSocketServer } from 'node:net';
import { createServer } from 'vite';

assert.ok(process.argv[2], 'Provide a build/reports output directory');
const selectedCase = process.argv.find(argument => argument.startsWith('--case='))?.slice(7);
assert.ok(!selectedCase || /^(8|32|128|512)-(adjacent|dense)$/.test(selectedCase), 'Unknown benchmark case');
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const { chromium } = await import(pathToFileURL(process.env.PLAYWRIGHT_MODULE).href);
const availablePort = await new Promise((resolve, reject) => {
    const socket = createSocketServer();
    socket.once('error', reject);
    socket.listen(0, '127.0.0.1', () => {
        const port = socket.address().port;
        socket.close(error => error ? reject(error) : resolve(port));
    });
});
const server = await createServer({ server: { host: '127.0.0.1', port: availablePort, strictPort: true } });
let browser, stopping, port;
const results = [];
const stop = () => stopping ??= (async () => {
    try { await browser?.close(); }
    finally { await server.close(); }
})();
const signal = () => { process.exitCode = 1; void stop(); };
process.once('SIGTERM', signal);
process.once('SIGINT', signal);
const deadline = setTimeout(signal, 150000);
try {
    await server.listen();
    port = server.httpServer.address().port;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    for (const count of [8, 32, 128, 512]) for (const dense of [false, true]) {
        if (selectedCase && selectedCase !== `${count}-${dense ? 'dense' : 'adjacent'}`) continue;
        if (stopping) throw new Error('Benchmark stopped');
        const page = await browser.newPage({ viewport: { width: 1040, height: 680 } });
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        await page.goto(`http://127.0.0.1:${port}/tools/layout-fixtures/mapping-presentation.html?count=${count}${dense ? '&dense' : ''}`);
        await page.locator('.full .zone').first().waitFor();
        let tracing;
        const traceEvents = [];
        if (process.argv.includes('--trace')) {
            tracing = await page.context().newCDPSession(page);
            tracing.on('Tracing.dataCollected', ({ value }) => traceEvents.push(...value));
            await tracing.send('Tracing.start', {
                categories: 'devtools.timeline,v8,blink.user_timing,disabled-by-default-devtools.timeline',
                options: 'record-as-much-as-possible',
            });
        }
        const metrics = await page.evaluate(async count => {
            const root = document.querySelector('.full');
            const zones = [...root.querySelectorAll('.zone')];
            const frame = () => new Promise(requestAnimationFrame);
            const flush = () => new Promise(resolve => queueMicrotask(() => queueMicrotask(resolve)));
            const times = [], paints = [], longTasks = [], operations = [];
            const mark = phase => {
                operations.push({ phase, startTime: performance.now() });
                performance.mark(phase);
            };
            let measurementStart = Infinity;
            const observer = new PerformanceObserver(list => longTasks.push(...list.getEntries().filter(entry => entry.startTime >= measurementStart).map(entry => ({ duration: entry.duration, startTime: entry.startTime }))));
            observer.observe({ type: 'longtask' });
            for (let i = 0; i < 5; i++) { zones[i % count].click(); await flush(); await frame(); }
            longTasks.length = 0;
            observer.takeRecords();
            measurementStart = performance.now();
            for (let i = 0; i < 30; i++) {
                mark('selection');
                const start = performance.now();
                zones[(i + 5) % count].click();
                await flush();
                times.push(performance.now() - start);
                await frame();
                paints.push(performance.now() - start);
            }
            const zoom = [];
            for (let i = 0; i < 12; i++) {
                mark('zoom');
                const start = performance.now();
                root.querySelector(`[aria-label="Zoom keyboard ${i % 2 ? 'out' : 'in'}"]`).click();
                await flush();
                zoom.push(performance.now() - start);
                await frame();
            }
            mark('pan setup');
            root.querySelector('[aria-label="Zoom keyboard in"]').click();
            await flush();
            await frame();
            const pan = [];
            for (let i = 0; i < 12; i++) {
                mark('pan');
                const start = performance.now();
                root.querySelector(`[aria-label="Pan keyboard ${i % 2 ? 'right' : 'left'}"]`).click();
                await flush();
                pan.push(performance.now() - start);
                await frame();
            }
            mark('viewport restore');
            root.querySelector('[aria-label="Zoom keyboard out"]').click();
            await flush();
            await frame();
            await new Promise(resolve => setTimeout(resolve, 0));
            longTasks.push(...observer.takeRecords().filter(entry => entry.startTime >= measurementStart).map(entry => ({ duration: entry.duration, startTime: entry.startTime })));
            observer.disconnect();
            const summarize = values => {
                values.sort((a, b) => a - b);
                return { median: values[Math.floor(values.length / 2)], p95: values[Math.ceil(values.length * .95) - 1], max: values.at(-1) };
            };
            return { selectionWorkMs: summarize(times), selectionFrameMs: summarize(paints), zoomWorkMs: summarize(zoom), panWorkMs: summarize(pan), longTasksMs: longTasks.map(entry => entry.duration), longTasks: longTasks.map(entry => ({ ...entry, phase: operations.findLast(operation => operation.startTime <= entry.startTime + entry.duration)?.phase ?? 'unknown' })) };
        }, count);
        assert.deepEqual(errors, []);
        const handle = page.getByRole('button', { name: 'High velocity limit', exact: true });
        const box = await handle.boundingBox(), plot = await page.locator('.full .plot').boundingBox();
        await page.evaluate(() => {
            window.mappingFrames = [];
            let previous;
            const record = now => {
                if (previous !== undefined) window.mappingFrames.push(now - previous);
                previous = now;
                window.mappingFrame = requestAnimationFrame(record);
            };
            window.mappingFrame = requestAnimationFrame(record);
        });
        await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
        await page.mouse.down();
        await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2 + plot.height / 4, { steps: 45 });
        await page.mouse.up();
        const dragFrames = await page.evaluate(() => {
            cancelAnimationFrame(window.mappingFrame);
            return window.mappingFrames;
        });
        if (tracing) {
            const completed = new Promise(resolve => tracing.once('Tracing.tracingComplete', resolve));
            await tracing.send('Tracing.end');
            await completed;
            await writeFile(resolve(output, `trace-${count}-${dense ? 'dense' : 'adjacent'}.json`), JSON.stringify({ traceEvents }) + '\n');
            await tracing.detach();
        }
        results.push({ count, dense, ...metrics, dragFramesMs: dragFrames });
        if (process.argv.includes('--verify')) {
            assert.ok(metrics.selectionWorkMs.p95 < 50, 'Selection feedback work stays below 50 ms');
            assert.ok(metrics.selectionFrameMs.p95 < 50, 'Selection reaches a frame within 50 ms');
            assert.ok(metrics.zoomWorkMs.p95 < 50, 'Zoom feedback work stays below 50 ms');
            assert.ok(metrics.panWorkMs.p95 < 50, 'Pan feedback work stays below 50 ms');
            const sorted = [...dragFrames].sort((a, b) => a-b);
            assert.ok(sorted[Math.ceil(sorted.length * .95) - 1] < 33, 'Drag frames stay below 33 ms at p95');
            assert.equal(metrics.longTasksMs.length, 0, 'No long tasks after warm-up');
        }
        console.log(JSON.stringify({ ...results.at(-1), dragFramesMs: { max: Math.max(...dragFrames), samples: dragFrames.length } }));
        await page.close();
    }
} finally {
    clearTimeout(deadline);
    await stop();
    const portReleased = port === undefined || await new Promise(resolve => {
        const socket = createConnection({ host: '127.0.0.1', port });
        socket.once('connect', () => { socket.destroy(); resolve(false); });
        socket.once('error', error => { socket.destroy(); resolve(error.code === 'ECONNREFUSED'); });
        socket.setTimeout(2000, () => { socket.destroy(); resolve(false); });
    });
    await writeFile(resolve(output, 'performance.json'), JSON.stringify({ port, portReleased, browserStopped: !browser?.isConnected(), serverStopped: !server.httpServer?.listening, results }, null, 2) + '\n');
    assert.ok(portReleased, 'Owned benchmark port must be released');
}
