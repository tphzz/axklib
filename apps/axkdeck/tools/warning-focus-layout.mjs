import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { createServer as createSocketServer } from 'node:net';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const checks = await readFile(new URL('./warning-focus-checks.js', import.meta.url), 'utf8');
const server = await createServer({ server: { host: '127.0.0.1', port: 0, strictPort: false } });
let browser, child, childExit, port, stopping;
const results = [];
function stop() {
    return stopping ??= (async () => {
        if (child?.pid) {
            try { process.kill(-child.pid, 'SIGTERM'); } catch (error) { if (error.code !== 'ESRCH') throw error; }
            const force = setTimeout(() => { try { process.kill(-child.pid, 'SIGKILL'); } catch {} }, 3000);
            try { await childExit; } finally { clearTimeout(force); }
        }
        try { await browser?.close(); } finally { await server.close(); }
    })();
}
const interrupted = () => { process.exitCode = 1; void stop(); };
process.once('SIGTERM', interrupted); process.once('SIGINT', interrupted);
const deadline = setTimeout(interrupted, 180000);
try {
    await server.listen();
    port = server.httpServer.address().port;
    const base = `http://127.0.0.1:${port}`;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    for (const [width, zoom] of [[1440, 1], [1440, 1.25], [1440, 1.5], [800, 1]]) {
        const name = `${width}-${zoom}`;
        const page = await browser.newPage({ viewport: { width, height: 1000 } });
        page.setDefaultTimeout(10000);
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        try {
            await page.goto(`${base}/tools/layout-fixtures/warning-focus.html`);
            const title = page.getByRole('heading', { name: 'Experimental software' });
            const action = page.getByRole('button', { name: 'I understand', exact: true });
            await title.waitFor();
            await page.evaluate(value => { document.documentElement.style.zoom = String(value); }, zoom);
            await page.addScriptTag({ content: checks });
            const result = await page.evaluate(() => window.runWarningFocusChecks());
            await page.screenshot({ path: resolve(output, `initial-${name}.png`) });
            assert.deepEqual(result.failures, []);
            for (const key of ['Enter', 'Space', 'Escape']) await page.keyboard.press(key);
            assert.equal(await page.getByLabel('Acknowledgements').textContent(), '0');
            await page.locator('.dialog-backdrop').click({ position: { x: 4, y: 4 } });
            assert.equal(await action.isVisible(), true);
            await title.focus(); await page.keyboard.press('Shift+Tab');
            assert.equal(await action.evaluate(node => document.activeElement === node), true);
            await title.focus(); await page.keyboard.press('Tab'); await page.waitForTimeout(180);
            assert.equal(await action.evaluate(node => document.activeElement === node && node.matches(':focus-visible') && parseFloat(getComputedStyle(node).outlineWidth) > 0), true);
            await page.screenshot({ path: resolve(output, `keyboard-${name}.png`) });
            await page.keyboard.press('Tab'); await page.keyboard.press('Shift+Tab');
            assert.equal(await action.evaluate(node => document.activeElement === node), true);
            for (const [key, count] of [['Enter', 1], ['Space', 2]]) {
                if (count === 2) {
                    await page.getByRole('button', { name: 'Show warning' }).click();
                    await page.waitForFunction(() => document.activeElement?.id === 'experimental-warning-title');
                    await page.keyboard.press('Tab');
                }
                await page.keyboard.press(key);
                await page.waitForFunction(value => document.querySelector('output').textContent === String(value), count);
                assert.equal(await page.getByRole('dialog').count(), 0);
                assert.equal(await page.getByRole('button', { name: 'Show warning' }).evaluate(node => document.activeElement === node && !node.inert), true);
            }
            await page.getByRole('button', { name: 'Show warning' }).click();
            await action.click();
            await page.waitForFunction(() => document.querySelector('output').textContent === '3');
            assert.deepEqual(errors, []);
            results.push({ name, ...result, errors });
            console.log(`PASS ${name}`);
        } catch (error) {
            process.exitCode = 1;
            results.push({ name, failures: [error.stack], errors });
            await page.screenshot({ path: resolve(output, `failure-${name}.png`) });
            console.error(`FAIL ${name}: ${error.message}`);
        } finally { await page.close(); }
    }
    if (process.env.WARNING_FOCUS_WEBKIT === '1') {
        const args = ['tools/sample-editor-webkit.py', base, resolve(output, 'webkit-initial'), '--fixture', '/tools/layout-fixtures/warning-focus.html', '--checks', 'tools/warning-focus-checks.js', '--checks-function', 'runWarningFocusChecks', '--zoom', '1', '--zoom', '1.25', '--zoom', '1.5'];
        await new Promise((done, reject) => {
            child = spawn('/usr/bin/python3', args, { stdio: 'inherit', detached: true });
            childExit = new Promise(resolveExit => child.once('close', resolveExit));
            child.once('error', reject);
            child.once('close', code => { child = undefined; code === 0 ? done() : reject(Error(`WebKitGTK initial focus exited ${code}`)); });
        });
    }
} finally {
    clearTimeout(deadline);
    await stop();
    process.removeListener('SIGTERM', interrupted); process.removeListener('SIGINT', interrupted);
    if (port) await new Promise((done, reject) => {
        const socket = createSocketServer(); socket.once('error', reject);
        socket.listen(port, '127.0.0.1', () => socket.close(error => error ? reject(error) : done()));
    });
    await writeFile(resolve(output, 'results.json'), `${JSON.stringify({ serverStopped: true, browserStopped: true, childrenStopped: !child, portReleased: !!port, nativeKeyboardQualification: 'not evaluated by this runner; real keyboard coverage is Chromium', results }, null, 2)}\n`);
}
process.exit(process.exitCode ?? 0);
