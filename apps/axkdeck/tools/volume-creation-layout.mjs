import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { spawn } from 'node:child_process';
import { createServer } from 'vite';
const { chromium } = await import(pathToFileURL(process.env.PLAYWRIGHT_MODULE).href);
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const checks = await readFile(new URL('./volume-creation-checks.js', import.meta.url), 'utf8');
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
    browser = await chromium.launch({ executablePath: '/usr/bin/chromium', headless: true });
    for (const [width, zoom] of [390, 700, 1366].flatMap(width => [1, 1.5, 2].map(zoom => [width, zoom]))) {
        const page = await browser.newPage({ viewport: { width, height: 900 } });
        const errors = []; page.on('pageerror', error => errors.push(error.message));
        const entry = { width, zoom, failures: [], errors }; results.push(entry);
        try {
            await page.goto(`${base}/tools/layout-fixtures/volume-creation.html`);
            await page.evaluate(zoom => { document.body.style.zoom = String(zoom); }, zoom);
            await page.addScriptTag({ content: checks });
            Object.assign(entry, await page.evaluate(() => window.runVolumeCreationChecks()));
            assert.deepEqual(entry.failures, []); assert.deepEqual(errors, []);
            await page.screenshot({ path: resolve(output, `${width}-${zoom}-nested.png`) });
            await page.keyboard.press('Tab');
            assert.equal(await page.getByRole('dialog', { name: 'Sampler load capacity' }).evaluate(node => node.contains(document.activeElement)), true);
            await page.keyboard.press('Escape');
            await page.getByRole('dialog', { name: 'Sampler load capacity' }).waitFor({ state: 'detached' });
            const parent = page.getByRole('dialog', { name: 'Add volume' });
            assert.equal(await parent.getByLabel('Volume name').inputValue(), 'bar');
            await page.screenshot({ path: resolve(output, `${width}-${zoom}-restored.png`) });
            await parent.getByRole('button', { name: 'Add', exact: true }).click();
            const childDialog = page.getByRole('dialog', { name: 'Sampler load capacity' });
            await childDialog.waitFor();
            await childDialog.getByRole('button', { name: 'Continue', exact: true }).click();
            await parent.waitFor({ state: 'detached' });
            assert.equal(await page.locator('[data-volume-state]').evaluate(node => JSON.parse(node.textContent).writes), entry.state.writes + 1);
            if (width === 1366 && zoom === 1) {
                await page.getByLabel('Scenario', { exact: true }).click();
                await page.screenshot({ path: resolve(output, '1366-1-dropdown.png') });
                await page.keyboard.press('Escape');
            }
            console.log(`PASS Chromium ${width} zoom ${zoom}`);
        } catch (error) { entry.failures.push(error.message); process.exitCode = 1; console.error(error.message); }
        finally { await page.screenshot({ path: resolve(output, `${width}-${zoom}-final.png`) }); await page.close(); }
    }
    for (const width of process.env.CAPACITY_WEBKIT === '1' ? [700, 1366] : []) {
        child = spawn('/usr/bin/python3', ['tools/sample-editor-webkit.py', base, resolve(output, `webkit-${width}`), '--width', String(width), '--height', '900', '--zoom', '1', '--zoom', '1.5', '--zoom', '2', '--focused', '--fixture', '/tools/layout-fixtures/volume-creation.html', '--checks', 'tools/volume-creation-checks.js', '--checks-function', 'runVolumeCreationChecks'], { detached: true, stdio: 'inherit' });
        childExit = new Promise((resolve, reject) => { child.once('error', reject); child.once('exit', resolve); });
        if (await childExit !== 0) process.exitCode = 1;
        await stopChild();
    }
} finally {
    clearTimeout(deadline); process.removeListener('SIGTERM', signal); process.removeListener('SIGINT', signal);
    await stop();
    await writeFile(resolve(output, 'results.json'), JSON.stringify({ port, serverStopped: true, results }, null, 2) + '\n');
}
