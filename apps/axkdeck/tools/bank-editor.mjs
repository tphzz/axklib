import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { spawn } from 'node:child_process';
import { createConnection, createServer as createSocketServer } from 'node:net';
import { createServer } from 'vite';
const { chromium } = await import(pathToFileURL(process.env.PLAYWRIGHT_MODULE).href);
const output = resolve(process.argv[2] ?? '../../../build/logs/sample-bank-editor/00001');
const webkitOnly = process.env.BANK_EDITOR_WEBKIT_ONLY === '1';
await mkdir(output, {recursive: true});
const checks = await readFile(new URL('./bank-editor-checks.js', import.meta.url), 'utf8');
const availablePort = await new Promise((resolve, reject) => {
    const socket = createSocketServer();
    socket.once('error', reject);
    socket.listen(0, '127.0.0.1', () => {
        const port = socket.address().port;
        socket.close(error => error ? reject(error) : resolve(port));
    });
});
const server = await createServer({server: {host: '127.0.0.1', port: availablePort, strictPort: true}});
let browser, port, child, stopping;
const results = [];
function stopChild() {
    if (child && child.exitCode === null) {
        try { process.kill(-child.pid, 'SIGTERM'); } catch (error) { if (error.code !== 'ESRCH') throw error; }
    }
}
function stop() {
    return stopping ??= (async () => {
        stopChild();
        try { await browser?.close(); }
        finally { await server.close(); }
    })();
}
const signal = () => { process.exitCode = 1; void stop(); };
process.once('SIGTERM', signal);
process.once('SIGINT', signal);
const deadline = setTimeout(signal, 600000);
try {
    await server.listen();
    port = server.httpServer.address().port;
    const base = `http://127.0.0.1:${port}`;
    if (!webkitOnly) browser = await chromium.launch({executablePath: process.env.CHROMIUM_PATH, headless: true, args: ['--autoplay-policy=no-user-gesture-required']});
    for (const width of webkitOnly ? [] : [390, 800, 1184, 1600]) for (const zoom of [1, 1.25, 1.5]) for (const native of [false, true]) {
        if (stopping) break;
        const page = await browser.newPage({viewport: {width, height: 900}});
        page.setDefaultTimeout(8000);
        const name = `${width}-${zoom}-${native ? 'native' : 'later'}`;
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        try {
            await page.goto(`${base}/tools/layout-fixtures/bank-editor.html${native ? '?short-stereo' : ''}`);
            await page.getByRole('button', { name: 'Preview sample', exact: true }).waitFor();
            await page.evaluate(zoom => {
                document.documentElement.style.zoom = String(zoom);
                document.querySelector('main').style.height = `${innerHeight / zoom - 40}px`;
            }, zoom);
            const preview = page.getByRole('button', { name: 'Preview sample', exact: true });
            await preview.click();
            await page.getByRole('option', { name: 'DNBS drum loop 1', exact: true }).click();
            await preview.press('ArrowDown');
            await page.getByRole('listbox', { name: 'Preview sample' }).press('Home');
            await page.getByRole('listbox', { name: 'Preview sample' }).press('Enter');
            await page.addScriptTag({content: checks});
            const result = await page.evaluate(() => window.runBankEditorChecks());
            assert.deepEqual(result.failures, []);
            assert.deepEqual(errors, []);
            const footer = await page.locator('.sample-transport').boundingBox();
            assert.ok(footer.y + footer.height <= 901, 'The persistent transport fits the viewport');
            results.push({name, ...result});
            console.log(`PASS ${name}`);
        } catch (error) {
            results.push({name, failures: [error.message], errors});
            console.error(`FAIL ${name}: ${error.message}`); process.exitCode = 1;
        } finally {
            try { if (!page.isClosed()) await page.screenshot({path: resolve(output, `${name}.png`)}); }
            finally { await page.close(); }
        }
    }
    // Optional native validation uses the same checks and the same owned Vite server.
    if (!stopping && (webkitOnly || process.env.BANK_EDITOR_WEBKIT === '1')) for (const width of [390, 800, 1184, 1600]) {
        if (stopping) break;
        child = spawn('/usr/bin/python3', ['tools/sample-editor-webkit.py', base, resolve(output, `webkit-${width}`),
            '--width', String(width), '--zoom', '1', '--zoom', '1.25', '--zoom', '1.5',
            '--fixture', '/tools/layout-fixtures/bank-editor.html',
            '--checks', 'tools/bank-editor-checks.js', '--checks-function', 'runBankEditorChecks'],
            {stdio: 'inherit', detached: true});
        const timeout = setTimeout(stopChild, 180000);
        try {
            const code = await new Promise((resolve, reject) => { child.once('error', reject); child.once('exit', resolve); });
            if (code !== 0) process.exitCode = 1;
        } finally { clearTimeout(timeout); stopChild(); child = undefined; }
    }
} finally {
    clearTimeout(deadline);
    await stop();
    const portReleased = !port || await new Promise(resolve => {
        const socket = createConnection({ host: '127.0.0.1', port });
        socket.once('connect', () => { socket.destroy(); resolve(false); });
        socket.once('error', error => resolve(error.code === 'ECONNREFUSED'));
    });
    await writeFile(resolve(output, 'results.json'), JSON.stringify({
        port, portReleased, serverStopped: !server.httpServer?.listening, browserStopped: !browser?.isConnected(), webkitOnly, results,
    }, null, 2) + '\n');
    assert.equal(portReleased, true, 'The owned test port must be released');
}
