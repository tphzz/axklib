import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { spawn } from 'node:child_process';
import { createServer as createSocketServer } from 'node:net';
import { createServer } from 'vite';
import { selectValue } from './select-control.mjs';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const server = await createServer({ server: { host: '127.0.0.1', port: 0, strictPort: false } });
const checks = await readFile(new URL('./ui-controls-checks.js', import.meta.url), 'utf8');
let browser, child;
let childExit;
let port;
const results = [];
let stopping;
async function stop() {
    if (stopping) return stopping;
    stopping = (async () => {
        const owned = child;
        if (owned?.pid) {
            try { process.kill(-owned.pid, 'SIGTERM'); } catch (error) { if (error.code !== 'ESRCH') throw error; }
            const force = setTimeout(() => { try { process.kill(-owned.pid, 'SIGKILL'); } catch {} }, 3000);
            try { await childExit; } finally { clearTimeout(force); }
        }
        try { await browser?.close(); }
        finally { await server.close(); }
    })();
    return stopping;
}
const interrupted = () => { process.exitCode = 1; void stop(); };
process.once('SIGTERM', interrupted); process.once('SIGINT', interrupted);
const deadline = setTimeout(interrupted, 300000);
try {
    await server.listen();
    port = server.httpServer.address().port;
    const base = `http://127.0.0.1:${port}`;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    for (const [width, scale, localScale] of [[1440, 1, 1], [800, 1, 1], [390, 1, 1], [1440, 1.25, 1], [1440, 1.5, 1], [1440, 1, 1.5]]) {
        const name = `${width}-${scale}-${localScale}`;
        const page = await browser.newPage({ viewport: { width, height: 1000 }, deviceScaleFactor: scale });
        page.setDefaultTimeout(10000);
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        try {
            await page.goto(`${base}/tools/layout-fixtures/ui-controls.html?localScale=${localScale}`);
            await page.getByRole('combobox', { name: 'Output', exact: true }).waitFor();
            await page.evaluate(scale => { document.documentElement.style.zoom = String(scale); }, scale);
            await page.addScriptTag({ content: checks });
            const result = await page.evaluate(() => window.runUiControlsChecks());
            await page.screenshot({ path: resolve(output, `controls-${name}.png`) });
            assert.deepEqual(result.failures, []);
            const choice = page.getByRole('combobox', { name: 'Output', exact: true });
            await page.keyboard.press('Escape');
            await choice.focus();
            await page.keyboard.press('Enter');
            assert.equal(await choice.getAttribute('aria-expanded'), 'true');
            await page.keyboard.press('ArrowDown');
            await page.keyboard.press('Space');
            assert.equal(await choice.getAttribute('aria-expanded'), 'false');
            assert.equal(await page.getByLabel('Selection changes').textContent(), '1');
            await choice.focus(); await page.keyboard.press('Space');
            assert.equal(await choice.getAttribute('aria-expanded'), 'true');
            await page.keyboard.press('Home'); await page.keyboard.press('Tab');
            assert.equal(await page.getByLabel('Selection changes').textContent(), '1');
            await page.getByRole('button', { name: 'Rename fixture volume', exact: true }).click();
            await page.getByLabel('Volume name', { exact: true }).press('X');
            assert.equal(await page.getByLabel('Volume name', { exact: true }).inputValue(), 'New VolumeX');
            await page.keyboard.press('Escape');
            await page.emulateMedia({ forcedColors: 'active' });
            await choice.click();
            assert.equal(await page.getByRole('option', { name: 'Ef1', exact: true }).isVisible(), true);
            await page.screenshot({ path: resolve(output, `forced-colors-${name}.png`) });
            await page.keyboard.press('Escape');
            await page.emulateMedia({ forcedColors: 'none' });
            await page.goto(`${base}/tools/layout-fixtures/device-dialog.html?mode=empty`);
            const deviceDialog = page.getByRole('dialog');
            await deviceDialog.getByRole('tab', { name: 'Device', exact: true }).click();
            const device = deviceDialog.getByRole('combobox', { name: 'Device', exact: true });
            await device.click();
            const popup = page.getByRole('listbox', { name: 'Device', exact: true });
            assert.equal(await popup.evaluate(node => getComputedStyle(node).backgroundColor), 'rgb(16, 21, 22)');
            assert.equal(await popup.getByRole('option').count(), 1);
            await page.screenshot({ path: resolve(output, `empty-device-${name}.png`) });
            await page.keyboard.press('Escape');
            await page.goto(`${base}/tools/layout-fixtures/device-dialog.html?mode=device`);
            await page.getByRole('tab', { name: 'Device', exact: true }).click();
            await selectValue(page.getByRole('combobox', { name: 'Device', exact: true }), 'fixture');
            assert.equal(await page.getByRole('button', { name: 'Open', exact: true }).isDisabled(), true);
            assert.deepEqual(errors, []);
            results.push({ name, ...result });
            console.log(`PASS ${name}`);
        } catch (error) {
            process.exitCode = 1;
            results.push({ name, failures: [error.stack], errors });
            await page.screenshot({ path: resolve(output, `failure-${name}.png`) });
            console.error(`FAIL ${name}: ${error.message}`);
        } finally { await page.close(); }
    }
    if (process.env.UI_CONTROLS_WEBKIT === '1') {
        await new Promise((resolveRun, reject) => {
            child = spawn('/usr/bin/python3', ['tools/sample-editor-webkit.py', base, resolve(output, 'webkit'), '--fixture', '/tools/layout-fixtures/ui-controls.html', '--checks', 'tools/ui-controls-checks.js', '--checks-function', 'runUiControlsChecks', '--zoom', '1', '--zoom', '1.25', '--zoom', '1.5'], { stdio: 'inherit', detached: true });
            childExit = new Promise(done => child.once('close', done));
            child.once('error', reject);
            child.once('close', code => { child = undefined; code === 0 ? resolveRun() : reject(new Error(`WebKitGTK exited ${code}`)); });
        });
    }
} finally {
    clearTimeout(deadline);
    await stop();
    process.removeListener('SIGTERM', interrupted); process.removeListener('SIGINT', interrupted);
    if (port) await new Promise((resolvePort, reject) => {
        const socket = createSocketServer(); socket.once('error', reject);
        socket.listen(port, '127.0.0.1', () => socket.close(error => error ? reject(error) : resolvePort()));
    });
    await writeFile(resolve(output, 'results.json'), JSON.stringify({ serverStopped: true, browserStopped: true, portReleased: !!port, results }, null, 2) + '\n');
}
