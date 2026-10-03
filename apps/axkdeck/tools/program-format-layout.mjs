import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const server = await createServer({ server: { host: '127.0.0.1', port: 0, strictPort: true } });
let browser, port;
const results = [];
const audit = process.argv[3] ? JSON.parse(await readFile(resolve(process.argv[3]), 'utf8')) : null;
async function stop() { await browser?.close(); await server.close(); }
const signal = () => void stop();
process.once('SIGTERM', signal); process.once('SIGINT', signal);
const deadline = setTimeout(signal, 120000);
try {
    await server.listen();
    port = server.httpServer.address().port;
    const base = `http://127.0.0.1:${port}/tools/layout-fixtures/program-format.html`;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    for (const width of [1366, 800, 390]) {
        const page = await browser.newPage({ viewport: { width, height: 800 } });
        const errors = []; page.on('pageerror', error => errors.push(error.message));
        const result = { width, errors }; results.push(result);
        try {
            const open = async () => {
                await page.getByText('Ambient pads', { exact: true }).locator('xpath=ancestor::button').focus();
                await page.keyboard.press('Shift+F10');
                await page.getByRole('menuitem', { name: /Convert to .* program format/ }).click();
                const dialog = page.getByRole('dialog', { name: /program format/ });
                await dialog.waitFor();
                return dialog;
            };
            await page.goto(base);
            await page.getByText('a4k/a5k', { exact: true }).first().waitFor();
            await page.getByRole('button', { name: 'Multi Part view', exact: true }).click();
            assert.equal(await page.locator('.program-multi-program').getByText('a4k/a5k', { exact: true }).count(), 1);
            await page.screenshot({ path: resolve(output, `${width}-multi.png`) });
            await page.getByRole('button', { name: 'Single Program view', exact: true }).click();
            let dialog = await open();
            const geometry = await dialog.evaluate(node => {
                const buttons = [...node.querySelectorAll('.dialog-footer-actions button')].map(button => {
                    const rect = button.getBoundingClientRect(), style = getComputedStyle(button);
                    return { height: rect.height, y: rect.y, top: style.marginTop, bottom: style.marginBottom };
                });
                const rect = node.getBoundingClientRect();
                return { buttons, left: rect.left, right: rect.right, bottom: rect.bottom, overflow: node.scrollWidth > node.clientWidth };
            });
            assert.deepEqual(geometry.buttons[0], geometry.buttons[1]);
            assert.equal(geometry.buttons[0].top, '0px');
            assert.equal(geometry.buttons[0].bottom, '0px');
            assert.ok(geometry.left >= 0 && geometry.right <= width && geometry.bottom <= 800 && !geometry.overflow);
            await page.screenshot({ path: resolve(output, `${width}-ready.png`) });
            await dialog.getByRole('button', { name: 'Convert', exact: true }).click();
            await dialog.getByRole('button', { name: 'Refresh', exact: true }).click();
            await dialog.waitFor({ state: 'detached' });
            assert.equal(await page.getByLabel('Conversion writes').textContent(), '1');
            await page.getByText('a3k', { exact: true }).first().waitFor();
            for (const mode of ['blocked', 'readonly']) {
                await page.goto(`${base}?${mode}`);
                dialog = await open();
                assert.equal(await dialog.getByRole('button', { name: 'Convert', exact: true }).isDisabled(), true);
                assert.equal(await dialog.getByRole('checkbox').count(), 0);
                if (mode === 'blocked') assert.ok(await dialog.locator('.format-results').evaluate(node => node.scrollHeight > node.clientHeight));
                await page.screenshot({ path: resolve(output, `${width}-${mode}.png`) });
                await page.keyboard.press('Escape');
                await dialog.waitFor({ state: 'detached' });
                assert.equal(await page.getByLabel('Conversion writes').textContent(), '0');
            }
            if (audit) {
                const records = audit.results.flatMap(source => {
                    assert.equal(source.status, 'passed');
                    const byCapacity = new Map(source.records.map(record => [record.format.assignmentCapacity, record]));
                    return [...byCapacity.values()].map(record => ({ name: record.name, format: record.format }));
                });
                records.push({ name: 'Unknown', format: { format: 'UNKNOWN', structurallyValid: false, headerRevision: 3, logicalSize: null, storedAssignmentCount: null, assignmentCapacity: null, parameterTailBytes: null } });
                await page.addInitScript(records => { window.programStorageAudit = records; }, records);
                await page.goto(base);
                const section = page.getByRole('button', { name: /Stored format/ });
                if (await section.getAttribute('aria-expanded') !== 'true') await section.click();
                for (const [index, record] of records.entries()) {
                    await page.getByLabel('Source Program', { exact: true }).selectOption(String(index));
                    const label = record.format.format === 'A3000' ? 'a3k' : record.format.format === 'A4000_A5000' ? 'a4k/a5k' : '?';
                    await page.waitForFunction(expected => document.querySelector('.program-row .format-badge')?.textContent.trim() === expected, label);
                    assert.equal(await page.locator('aside .format-badge').textContent(), label);
                    assert.ok((await page.locator('aside').textContent()).includes(record.format.parameterTailBytes === null ? 'Unavailable' : `${record.format.parameterTailBytes} bytes`));
                    await page.getByRole('button', { name: 'Multi Part view', exact: true }).click();
                    assert.equal(await page.locator('.program-multi-program .format-badge').textContent(), label);
                    await page.getByRole('button', { name: 'Single Program view', exact: true }).click();
                }
                await page.screenshot({ path: resolve(output, `${width}-unknown.png`) });
                await page.getByLabel('Source Program', { exact: true }).selectOption('0');
                const row = page.locator('.program-row');
                const original = await row.boundingBox();
                await row.hover();
                await page.getByRole('tooltip').waitFor();
                assert.ok((await page.getByRole('tooltip').textContent()).includes('stored Program format'));
                assert.deepEqual(await row.boundingBox(), original);
                await page.keyboard.press('Escape');
                await page.getByRole('tooltip').waitFor({ state: 'detached' });
                await page.getByLabel('Source Program', { exact: true }).focus();
                await page.keyboard.press('Tab');
                await row.focus();
                await page.getByRole('tooltip').waitFor();
                await page.keyboard.press('Escape');
                await page.getByRole('button', { name: 'Header revision', exact: true }).click();
                await page.getByRole('tooltip').waitFor();
                await page.screenshot({ path: resolve(output, `${width}-source-help.png`) });
                await page.locator('main').click({ position: { x: 1, y: 1 } });
                await page.getByRole('tooltip').waitFor({ state: 'detached' });
                if (width === 1366) {
                    await page.evaluate(() => { document.body.style.zoom = '1.5'; });
                    await page.screenshot({ path: resolve(output, '1366-source-150pct.png') });
                }
                assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);
                result.realMetadataCases = records.length - 1;
            }
            assert.deepEqual(errors, []);
            result.passed = true;
            console.log(`${width}: badges, keyboard conversion, footer geometry, recovery, blockers and read-only passed`);
        } catch (error) { result.error = error.message; throw error; }
        finally { await page.screenshot({ path: resolve(output, `${width}-final.png`) }); await page.close(); }
    }
} finally {
    clearTimeout(deadline); process.removeListener('SIGTERM', signal); process.removeListener('SIGINT', signal);
    await stop();
    await writeFile(resolve(output, 'results.json'), JSON.stringify({ port, serverStopped: true, results }, null, 2) + '\n');
}
