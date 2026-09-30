import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { spawn } from 'node:child_process';
import { createServer } from 'vite';
const { chromium } = await import(pathToFileURL(process.env.PLAYWRIGHT_MODULE).href);
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const checks = await readFile(new URL('./capacity-layout-checks.js', import.meta.url), 'utf8');
const server = await createServer({ server: { host: '127.0.0.1', strictPort: false } });
let browser, child, childExit, port, stopped = false;
const results = [];
async function stopChild() {
    if (!child?.pid) return;
    try { process.kill(-child.pid, 'SIGTERM'); } catch (error) { if (error.code !== 'ESRCH') throw error; }
    const force = setTimeout(() => { try { process.kill(-child.pid, 'SIGKILL'); } catch {} }, 5000);
    try { await childExit; } finally { clearTimeout(force); child = undefined; }
}
async function stop() { stopped = true; await stopChild(); await browser?.close(); await server.close(); }
const deadline = setTimeout(() => void stop(), 240000);
const signal = () => void stop();
process.once('SIGTERM', signal); process.once('SIGINT', signal);
try {
    await server.listen(); port = server.httpServer.address().port;
    const base = `http://127.0.0.1:${port}`;
    browser = await chromium.launch({ executablePath: '/usr/bin/chromium', headless: true });
    for (const [width, zoom] of [390, 700, 1366].flatMap(width => [1, 1.5, 2].map(zoom => [width, zoom]))) {
        if (stopped) throw new Error('Capacity regression interrupted');
        const page = await browser.newPage({ viewport: { width, height: 900 } });
        const errors = []; page.on('pageerror', error => errors.push(error.message));
        const entry = { width, zoom, failures: [], errors }; results.push(entry);
        try {
            await page.goto(`${base}/tools/layout-fixtures/capacity.html`);
            await page.evaluate(zoom => { document.body.style.zoom = String(zoom); }, zoom);
            await page.addScriptTag({ content: checks });
            Object.assign(entry, await page.evaluate(() => window.runCapacityLayoutChecks()));
            assert.deepEqual(entry.failures, []); assert.deepEqual(errors, []);
            // Real keyboard focus must reveal the dual-profile tooltip without another request.
            await page.getByRole('button', { name: 'Show object', exact: true }).click();
            await page.keyboard.press('Tab');
            await page.keyboard.press('Tab');
            await page.getByRole('tooltip').waitFor();
            assert.match(await page.getByRole('tooltip').innerText(), /A3000[\s\S]*Does not fit[\s\S]*A4000\/A5000[\s\S]*Does not fit/);
            await page.screenshot({ path: resolve(output, `${width}-${zoom}-tooltip.png`) });
            await page.keyboard.press('Escape');
            await page.getByRole('tooltip').waitFor({ state: 'detached' });
            await page.locator('.tree-item-select').hover();
            assert.equal(await page.getByRole('tooltip').count(), 0);
            await page.getByRole('tooltip').waitFor();
            await page.mouse.move(width - 2, 2);
            // Keyboard-focused help remains open until focus moves elsewhere.
            await page.getByRole('button', { name: 'Show object', exact: true }).focus();
            await page.getByRole('tooltip').waitFor({ state: 'detached' });
            await page.locator('.tree-item-select').click();
            await page.getByRole('button', { name: 'Apply change', exact: true }).click();
            await page.getByRole('dialog', { name: 'Sampler load capacity', exact: true }).waitFor();
            await page.keyboard.press('Escape');
            await page.getByRole('dialog', { name: 'Sampler load capacity', exact: true }).waitFor({ state: 'detached' });
            for (const [scenario, label] of [['FITS', 'Fits'], ['DOES_NOT_FIT', 'Does not fit'], ['ERROR', 'Error']]) {
                await page.getByLabel('Inspector scenario', { exact: true }).selectOption(scenario);
                const inspector = page.getByRole('complementary', { name: 'Volume inspector', exact: true });
                if (scenario === 'ERROR') await inspector.locator('[role="alert"]').waitFor({ state: 'attached' });
                else await inspector.locator('.capacity-profile-heading span').first().filter({ hasText: label }).waitFor({ state: 'attached' });
                const section = inspector.getByRole('button', { name: 'Sampler Capacity', exact: true });
                if (await section.getAttribute('aria-expanded') === 'false') await section.click();
                await page.screenshot({ path: resolve(output, `${width}-${zoom}-inspector-${scenario}.png`) });
                await page.getByLabel('Capacity scenario', { exact: true }).selectOption(scenario === 'ERROR' ? 'FITS' : scenario);
                await page.getByRole('button', { name: 'Apply change', exact: true }).click();
                const dialog = page.getByRole('dialog', { name: 'Sampler load capacity', exact: true });
                await dialog.waitFor();
                if (scenario === 'ERROR') {
                    await page.getByLabel('Capacity scenario', { exact: true }).evaluate(node => {
                        node.value = 'ERROR'; node.dispatchEvent(new Event('change', { bubbles: true }));
                    });
                    await dialog.getByRole('button', { name: 'a3k', exact: true }).click();
                    await dialog.getByText('Capacity service disconnected', { exact: true }).waitFor();
                }
                else await dialog.locator('.capacity-summary').filter({ hasText: label }).waitFor();
                assert.equal(await dialog.getByRole('button', { name: 'Continue', exact: true }).isEnabled(), scenario === 'FITS');
                await page.screenshot({ path: resolve(output, `${width}-${zoom}-mutation-${scenario}.png`) });
                await dialog.getByRole('button', { name: 'Cancel', exact: true }).click();
                await dialog.waitFor({ state: 'detached' });
            }
            entry.dropdownStyles = await page.locator('nav select').evaluateAll(nodes => nodes.map(node => ({
                background: getComputedStyle(node).backgroundColor,
                foreground: getComputedStyle(node).color,
                colorScheme: getComputedStyle(node).colorScheme,
            })));
            if (width === 1366 && zoom === 1) {
                await page.getByLabel('Capacity scenario', { exact: true }).click();
                await page.screenshot({ path: resolve(output, '1366-1-dropdown.png') });
                await page.keyboard.press('Escape');
            }
            console.log(`PASS Chromium ${width} zoom ${zoom}`);
        } catch (error) { entry.failures.push(error.message); process.exitCode = 1; console.error(error.message); }
        finally { await page.screenshot({ path: resolve(output, `${width}-${zoom}.png`) }); await page.close(); }
    }
    for (const width of process.env.CAPACITY_WEBKIT === '1' ? [700, 1366] : []) {
        if (stopped) throw new Error('Capacity regression interrupted');
        child = spawn('/usr/bin/python3', ['tools/sample-editor-webkit.py', base, resolve(output, `webkit-${width}`), '--width', String(width), '--height', '900', '--zoom', '1', '--zoom', '1.5', '--zoom', '2', '--focused', '--fixture', '/tools/layout-fixtures/capacity.html', '--checks', 'tools/capacity-layout-checks.js', '--checks-function', 'runCapacityLayoutChecks'], { detached: true, stdio: 'inherit' });
        childExit = new Promise((resolve, reject) => { child.once('error', reject); child.once('exit', resolve); });
        if (await childExit !== 0) process.exitCode = 1;
        await stopChild();
    }
} finally {
    clearTimeout(deadline); process.removeListener('SIGTERM', signal); process.removeListener('SIGINT', signal);
    await stopChild(); await browser?.close(); await server.close();
    await writeFile(resolve(output, 'results.json'), JSON.stringify({ port, serverStopped: true, results }, null, 2) + '\n');
}
