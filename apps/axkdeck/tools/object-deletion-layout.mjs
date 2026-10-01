import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const base = process.argv[2] ?? 'http://127.0.0.1:5191';
const output = resolve(process.argv[3] ?? '../../../build/logs/program-deletion/browser');
await mkdir(output, { recursive: true });
const checks = await readFile(new URL('./object-deletion-checks.js', import.meta.url), 'utf8');
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const width of [390, 800, 1280]) {
        const page = await browser.newPage({ viewport: { width, height: 800 } });
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        await page.goto(`${base}/tools/layout-fixtures/object-deletion.html`);
        await page.getByRole('dialog').waitFor();
        await page.evaluate(checks);
        const result = await page.evaluate(() => window.runObjectDeletionChecks());
        await page.screenshot({ path: resolve(output, `failure-${width}.png`) });
        assert.deepEqual(result.failures, [], JSON.stringify(result));
        await page.keyboard.press('Escape');
        await page.getByRole('dialog').waitFor({ state: 'detached' });
        await page.getByRole('button', { name: 'Open deletion' }).click();
        await page.getByRole('button', { name: 'Delete 119 objects', exact: true }).click();
        await page.getByRole('alert').waitFor();
        assert.equal(await page.getByTestId('attempts').textContent(), '2');
        await page.getByRole('button', { name: 'Cancel', exact: true }).click();
        await page.getByRole('dialog').waitFor({ state: 'detached' });
        assert.deepEqual(errors, []);
        results.push({ width, ...result });
        await page.close();
    }
    await writeFile(resolve(output, 'results.json'), `${JSON.stringify(results, null, 2)}\n`);
    console.log(`${results.length} object deletion layout cases passed`);
} finally {
    await browser.close();
}
