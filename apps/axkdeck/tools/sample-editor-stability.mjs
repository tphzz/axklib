import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const base = process.argv[2] ?? 'http://127.0.0.1:5191';
const output = resolve(process.argv[3] ?? '../../../build/logs/sample-editor/00008');
await mkdir(output, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const [width, height, zoom] of [[1600, 900, 1], [1600, 900, 1.25], [1600, 900, 1.5], [1600, 900, 2], [800, 640, 1], [390, 740, 1]]) {
        for (const stereo of [false, true]) {
            const page = await browser.newPage({ viewport: { width, height } });
            const name = `stability-${width}-${zoom}-${stereo ? 'stereo' : 'mono'}`;
            const errors = [];
            page.on('pageerror', error => errors.push(error.message));
            try {
                await page.goto(`${base}/tools/layout-fixtures/sample-editor.html${stereo ? '?short-stereo' : ''}`);
                await page.getByRole('button', { name: 'Save', exact: true }).waitFor();
                await page.evaluate(zoom => { document.documentElement.style.zoom = String(zoom); document.querySelector('main').style.height = `${innerHeight / zoom - 40}px`; }, zoom);
                await page.addScriptTag({ path: resolve('tools/sample-editor-regression-checks.js') });
                const result = await page.evaluate(() => window.runSampleEditorRegression());
                assert.deepEqual(result.failures, []);
                for (const [tab, sub] of [['Map/Out', 'Mix & Key'], ['Map/Out', 'Pitch'], ['Map/Out', 'Expansion & Velocity'], ['Filter', 'Sample EQ'], ['EG', 'Amplitude'], ['LFO', null]]) {
                    await page.getByRole('tab', { name: tab, exact: true }).click();
                    if (sub) await page.getByLabel('Sample subpages', { exact: true }).getByRole('button', { name: sub, exact: true }).click();
                    await page.screenshot({ path: resolve(output, `${name}-${(sub ?? tab).replaceAll(/[^a-z0-9]+/gi, '-')}.png`) });
                }
                await page.getByRole('tab', { name: 'EG', exact: true }).click();
                await page.getByLabel('Sample subpages', { exact: true }).getByRole('button', { name: 'Amplitude', exact: true }).click();
                await page.getByRole('button', { name: 'Fit envelope to width' }).click();
                const handle = page.locator('[data-handle="1"]');
                const box = await handle.boundingBox();
                await page.mouse.move(box.x + box.width / 2 + 3, box.y + box.height / 2);
                await page.mouse.down();
                await page.mouse.move(box.x - 25, box.y + box.height / 2, { steps: 1 });
                await page.mouse.move(box.x - 45, box.y + box.height / 2, { steps: 1 });
                await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
                const during = await page.locator('[data-trace="envelope"]').getAttribute('d');
                await page.mouse.up();
                assert.equal(await page.locator('[data-trace="envelope"]').getAttribute('d'), during, 'rapid pointer release must not rescale');
                assert.deepEqual(errors, []);
                results.push({ name, status: 'passed', ...result });
                console.log(`PASS ${name}`);
            } catch (error) {
                results.push({ name, status: 'failed', error: error.message, errors });
                console.error(`FAIL ${name}: ${error.message}`);
                process.exitCode = 1;
            } finally {
                await page.screenshot({ path: resolve(output, `${name}.png`) });
                await page.close();
            }
        }
    }
} finally {
    await writeFile(resolve(output, 'stability-results.json'), JSON.stringify(results, null, 2) + '\n');
    await browser.close();
}
