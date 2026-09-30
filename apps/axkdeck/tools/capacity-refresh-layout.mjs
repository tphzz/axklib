import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { spawn } from 'node:child_process';
import { createServer } from 'vite';

const { chromium } = await import(pathToFileURL(process.env.PLAYWRIGHT_MODULE).href);
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const checks = await readFile(new URL('./capacity-refresh-checks.js', import.meta.url), 'utf8');
const server = await createServer({ server: { host: '127.0.0.1', port: 0, strictPort: true } });
let browser, port, child, childExit;
const results = [];
async function stopChild() {
    if (!child?.pid) return;
    try { process.kill(-child.pid, 'SIGTERM'); } catch (error) { if (error.code !== 'ESRCH') throw error; }
    const force = setTimeout(() => { try { process.kill(-child.pid, 'SIGKILL'); } catch {} }, 5000);
    try { await childExit; } finally { clearTimeout(force); child = undefined; }
}
async function stop() { await stopChild(); await browser?.close(); await server.close(); }
const signal = () => void stop();
process.once('SIGTERM', signal); process.once('SIGINT', signal);
const deadline = setTimeout(signal, 180000);
try {
    await server.listen(); port = server.httpServer.address().port;
    const base = `http://127.0.0.1:${port}`;
    const fixture = '/tools/layout-fixtures/capacity-refresh.html';
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH || '/usr/bin/chromium', headless: true });
    for (const width of [1000, 1366, 1600]) {
        const page = await browser.newPage({ viewport: { width, height: 900 } });
        const errors = []; page.on('pageerror', error => errors.push(error.message));
        const entry = { width, errors }; results.push(entry);
        try {
            await page.goto(base + fixture); await page.addScriptTag({ content: checks });
            Object.assign(entry, await page.evaluate(() => window.runCapacityRefreshChecks()));
            assert.deepEqual(entry.failures, []); assert.deepEqual(errors, []);
            await page.goto(base + fixture);
            await page.locator('[data-tree-id="volume-1"]').click();
            const inspector = page.getByRole('complementary', { name: 'Volume inspector' });
            const profiles = inspector.locator('.capacity-profile');
            await profiles.first().waitFor({ state: 'attached' });
            await page.getByRole('button', { name: 'Complete floppy import', exact: true }).click();
            await page.waitForFunction(() => document.querySelector('[data-inspections]').textContent.includes('2:volume-2'));
            await profiles.first().waitFor({ state: 'attached' });
            await page.screenshot({ path: resolve(output, `${width}-collapsed.png`) });
            const panel = inspector.getByRole('button', { name: 'Sampler Capacity', exact: true });
            await panel.click();
            await page.screenshot({ path: resolve(output, `${width}-expanded.png`) });
            await page.getByRole('button', { name: 'Delay responses', exact: true }).click();
            await page.getByRole('button', { name: 'Complete floppy import', exact: true }).click();
            await inspector.getByText('Inspecting capacity...', { exact: true }).waitFor({ state: 'attached' });
            await panel.click();
            await page.screenshot({ path: resolve(output, `${width}-loading.png`) });
            await page.getByRole('button', { name: 'Release responses', exact: true }).click();
            await profiles.first().waitFor();
            await page.screenshot({ path: resolve(output, `${width}-refreshed.png`) });
            console.log(`PASS Chromium capacity refresh ${width}`);
        } finally { await page.close(); }
    }
    if (process.env.CAPACITY_WEBKIT === '1') {
        child = spawn('/usr/bin/python3', ['tools/sample-editor-webkit.py', base, resolve(output, 'webkit-1366'),
            '--width', '1366', '--height', '900', '--zoom', '1', '--zoom', '1.5', '--focused', '--fixture', fixture,
            '--checks', 'tools/capacity-refresh-checks.js', '--checks-function', 'runCapacityRefreshChecks'],
            { detached: true, stdio: 'inherit' });
        childExit = new Promise((resolve, reject) => { child.once('error', reject); child.once('exit', resolve); });
        assert.equal(await childExit, 0, 'Native WebKitGTK capacity refresh failed'); await stopChild();
    }
} finally {
    clearTimeout(deadline); process.removeListener('SIGTERM', signal); process.removeListener('SIGINT', signal);
    await stop();
    await writeFile(resolve(output, 'results.json'), JSON.stringify({ port, serverStopped: true, results }, null, 2) + '\n');
}
