import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const { chromium } = await import(pathToFileURL(process.env.PLAYWRIGHT_MODULE).href);
const [base, outputArg] = process.argv.slice(2);
const output = resolve(outputArg);
await mkdir(output, { recursive: true });
const checks = await readFile(new URL('./a-series-preferences-checks.js', import.meta.url), 'utf8');
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const width of [390, 800, 1600]) {
        const name = `a-series-${width}`;
        const page = await browser.newPage({ viewport: { width, height: 1000 } });
        const errors = [];
        let result = null;
        page.on('pageerror', (error) => errors.push(error.message));
        await page.exposeFunction('captureASeriesPreferences', async (state) => {
            await page.screenshot({ path: resolve(output, `${name}-${state}.png`) });
        });
        try {
            await page.goto(`${base}/tools/layout-fixtures/a-series-preferences.html`);
            await page.getByRole('button', { name: 'Open preferences', exact: true }).waitFor();
            await page.addScriptTag({ content: checks });
            result = await page.evaluate(() => window.runASeriesPreferencesChecks());
            assert.deepEqual(result.failures, []);
            assert.deepEqual(errors, []);
            await page.keyboard.press('Escape');
            await page.getByRole('dialog').waitFor({ state: 'detached' });
            await page.getByRole('button', { name: 'Open preferences', exact: true }).focus();
            await page.keyboard.press('Enter');
            await page.getByRole('dialog', { name: 'Preferences' }).waitFor();
            await page.getByRole('button', { name: 'Save', exact: true }).focus();
            await page.keyboard.press('Tab');
            await assert.doesNotReject(() =>
                page.getByRole('button', { name: 'Close', exact: true }).evaluate((node) => {
                    if (document.activeElement !== node) throw new Error('Trusted Tab did not wrap focus');
                }),
            );
            await page.keyboard.press('Escape');
            await page.getByRole('dialog').waitFor({ state: 'detached' });
            results.push({ name, ...result, errors });
            console.log(`PASS ${name}`);
        } catch (error) {
            results.push({ name, ...result, failures: [error.stack ?? error.message], errors });
            process.exitCode = 1;
            console.error(`FAIL ${name}: ${error.message}`);
        } finally {
            await page.screenshot({ path: resolve(output, `${name}-final.png`) });
            await page.close();
        }
    }
} finally {
    await browser.close();
    await writeFile(resolve(output, 'results.json'), JSON.stringify(results, null, 2) + '\n');
}
