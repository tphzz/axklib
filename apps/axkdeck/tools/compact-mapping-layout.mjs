import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { spawn } from 'node:child_process';
import { createConnection, createServer as createSocketServer } from 'node:net';
import { createServer } from 'vite';

const { chromium } = await import(pathToFileURL(process.env.PLAYWRIGHT_MODULE).href);
const output = resolve(process.argv[2] ?? '../../../build/reports/00200_compact_mapping_alignment');
const webkitOnly = process.env.COMPACT_MAPPING_WEBKIT_ONLY === '1';
const widths = [1498, 3440, 800, 600, 390];
const fixtures = ['sample-editor', 'bank-editor'];
const checks = await readFile(new URL('./compact-mapping-checks.js', import.meta.url), 'utf8');
await mkdir(output, {recursive: true});
const port = await new Promise((resolve, reject) => {
    const socket = createSocketServer();
    socket.once('error', reject);
    socket.listen(0, '127.0.0.1', () => {
        const port = socket.address().port;
        socket.close(error => error ? reject(error) : resolve(port));
    });
});
const server = await createServer({server: {host: '127.0.0.1', port, strictPort: true}});
let browser, child, stopping;
const results = [];
function stopChild() {
    if (child && child.exitCode === null) {
        try { process.kill(-child.pid, 'SIGTERM'); } catch (error) { if (error.code !== 'ESRCH') throw error; }
    }
}
function stop() {
    return stopping ??= (async () => {
        stopChild();
        try { await browser?.close(); } finally { await server.close(); }
    })();
}
const signal = () => { process.exitCode = 1; void stop(); };
process.once('SIGTERM', signal);
process.once('SIGINT', signal);
const deadline = setTimeout(signal, 240000);
try {
    await server.listen();
    const base = `http://127.0.0.1:${port}`;
    if (webkitOnly) {
        for (const width of widths) for (const fixture of fixtures) {
            if (stopping) break;
            const destination = resolve(output, `webkit-${fixture}-${width}`);
            child = spawn('/usr/bin/python3', ['tools/sample-editor-webkit.py', base, destination,
                '--width', String(width), '--zoom', '1', '--zoom', '1.25', '--zoom', '1.5',
                '--fixture', `/tools/layout-fixtures/${fixture}.html`,
                '--checks', 'tools/compact-mapping-checks.js', '--checks-function', 'runCompactMappingChecks'],
                {stdio: 'inherit', detached: true});
            const timeout = setTimeout(stopChild, 75000);
            try {
                const code = await new Promise((resolve, reject) => { child.once('error', reject); child.once('exit', resolve); });
                assert.equal(code, 0, `${fixture}/${width}: native GTK checks`);
                const outcomes = JSON.parse(await readFile(resolve(destination, 'webkit-results.json'), 'utf8'));
                results.push(...outcomes.map(outcome => ({fixture, width, ...outcome})));
            } finally { clearTimeout(timeout); stopChild(); child = undefined; }
        }
    } else {
        browser = await chromium.launch({executablePath: process.env.CHROMIUM_PATH, headless: true});
        for (const width of widths) for (const zoom of [1, 1.25, 1.5]) for (const fixture of fixtures) for (const short of [false, true]) {
            if (stopping) break;
            const page = await browser.newPage({viewport: {width, height: 900}});
            page.setDefaultTimeout(8000);
            const name = `${fixture}-${width}-${zoom}-${short ? 'short' : 'later'}`;
            const errors = [];
            page.on('pageerror', error => errors.push(error.message));
            try {
                await page.goto(`${base}/tools/layout-fixtures/${fixture}.html${short ? '?short-stereo' : ''}`);
                await page.getByRole('tab', {name: 'Map/Out', exact: true}).waitFor();
                await page.evaluate(zoom => {
                    document.documentElement.style.zoom = String(zoom);
                    document.querySelector('main').style.height = `${innerHeight / zoom - 40}px`;
                }, zoom);
                await page.addScriptTag({content: checks});
                const result = await page.evaluate(() => window.runCompactMappingChecks());
                results.push({name, ...result, errors});
                assert.deepEqual(result.failures, []);
                assert.deepEqual(errors, []);
                console.log(`PASS ${name}`);
            } finally {
                try { if (!page.isClosed()) await page.screenshot({path: resolve(output, `${name}.png`)}); }
                finally { await page.close(); }
            }
        }
    }
    assert.equal(results.length, 60, 'Complete width/scale/Sample/Bank/format matrix');
} finally {
    clearTimeout(deadline);
    await stop();
    const portReleased = await new Promise(resolve => {
        const socket = createConnection({host: '127.0.0.1', port});
        socket.once('connect', () => { socket.destroy(); resolve(false); });
        socket.once('error', error => resolve(error.code === 'ECONNREFUSED'));
    });
    await writeFile(resolve(output, 'results.json'), JSON.stringify({
        port, portReleased, serverStopped: !server.httpServer?.listening, browserStopped: !browser?.isConnected(), webkitOnly, results,
    }, null, 2) + '\n');
    assert.equal(portReleased, true, 'The owned test port must be released');
}
