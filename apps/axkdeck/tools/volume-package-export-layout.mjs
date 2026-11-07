import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { spawn } from 'node:child_process';
import { createServer } from 'vite';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const output = resolve(process.argv[2] ?? '../../../build/logs/native-volume-package-export/00001/layout');
await mkdir(output, { recursive: true });
const checks = await readFile(new URL('./volume-package-export-checks.js', import.meta.url), 'utf8');
const server = await createServer({ server: { host: '127.0.0.1', strictPort: false } });
let browser, child, childExit, port;
const results = [];
async function stopChild() {
    if (!child?.pid) return;
    try { process.kill(-child.pid, 'SIGTERM'); } catch (error) { if (error.code !== 'ESRCH') throw error; }
    const force = setTimeout(() => { try { process.kill(-child.pid, 'SIGKILL'); } catch {} }, 5000);
    try { await childExit; } finally { clearTimeout(force); child = undefined; }
}
async function stop() { await stopChild(); await browser?.close(); await server.close(); }
const deadline = setTimeout(() => void stop(), 240000);
const signal = () => void stop();
process.once('SIGTERM', signal); process.once('SIGINT', signal);
try {
    await server.listen(); port = server.httpServer.address().port;
    const base = `http://127.0.0.1:${port}`;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH ?? '/usr/bin/chromium', headless: true });
    for (const width of [390, 700, 1366]) {
        for (const zoom of [1, 1.25, 1.5]) {
            const page = await browser.newPage({
                viewport: { width: Math.floor(width / zoom), height: Math.floor(900 / zoom) },
                deviceScaleFactor: zoom,
            });
            const entry = { width, zoom, failures: [], errors: [] }; results.push(entry);
            page.on('pageerror', error => entry.errors.push(error.message));
            try {
                await page.goto(`${base}/tools/layout-fixtures/volume-package-export.html`);
                await page.addScriptTag({ content: checks });
                Object.assign(entry, await page.evaluate(() => window.runVolumePackageExportChecks()));
                assert.deepEqual(entry.failures, []); assert.deepEqual(entry.errors, []);
                await page.screenshot({ path: resolve(output, `${width}-${zoom}-failed.png`) });
                const dialog = page.getByRole('dialog', { name: 'Export volume packages' });
                await dialog.getByRole('button', { name: 'Retry', exact: true }).focus();
                await page.keyboard.press('Tab');
                assert(await dialog.evaluate(node => node.contains(document.activeElement)));
                await dialog.getByRole('button', { name: 'Retry', exact: true }).focus();
                await page.keyboard.press('Space');
                await page.waitForFunction(() => document.querySelector('[role="dialog"]')?.getAttribute('aria-busy') === 'true');
                assert(await dialog.getByRole('button', { name: 'Retry', exact: true }).isDisabled());
                await page.waitForFunction(() => {
                    const retry = [...document.querySelectorAll('.dialog-footer button')].find(node => node.textContent.trim() === 'Retry');
                    return retry && Math.abs(Number(getComputedStyle(retry).opacity) - 0.45) < 0.01;
                });
                await page.screenshot({ path: resolve(output, `${width}-${zoom}-inspecting.png`) });
                await page.keyboard.press('Escape');
                await dialog.waitFor({ state: 'detached' });
                await page.evaluate(() => window.volumePackageExportFixture.show('failed', true));
                const details = page.getByRole('alert');
                await details.focus();
                await page.keyboard.press('PageDown');
                await page.waitForFunction(() => document.querySelector('[role="alert"]')?.scrollTop > 0);
                await page.screenshot({ path: resolve(output, `${width}-${zoom}-keyboard-scroll.png`) });
                console.log(`PASS Chromium ${width} zoom ${zoom}`);
            } catch (error) { entry.failures.push(error.message); process.exitCode = 1; console.error(error.message); }
            finally { await page.screenshot({ path: resolve(output, `${width}-${zoom}-final.png`) }); await page.close(); }
        }
    }
    if (process.env.VOLUME_PACKAGE_WEBKIT === '1') {
        child = spawn('/usr/bin/python3', ['tools/volume-package-export-webkit.py', base, resolve(output, 'webkit')], { detached: true, stdio: 'inherit' });
        childExit = new Promise((resolve, reject) => { child.once('error', reject); child.once('exit', resolve); });
        if (await childExit !== 0) process.exitCode = 1;
        await stopChild();
    }
} finally {
    clearTimeout(deadline); process.removeListener('SIGTERM', signal); process.removeListener('SIGINT', signal);
    await stop();
    await writeFile(resolve(output, 'results.json'), JSON.stringify({ port, serverStopped: true, results }, null, 2) + '\n');
}
