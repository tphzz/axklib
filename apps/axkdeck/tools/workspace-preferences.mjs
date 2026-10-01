import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const { chromium } = await import(pathToFileURL(process.env.PLAYWRIGHT_MODULE).href);
const [base, outputArg] = process.argv.slice(2);
const output = resolve(outputArg);
await mkdir(output, { recursive: true });
const checks = await readFile(new URL('./workspace-preferences-checks.js', import.meta.url), 'utf8');
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const [width, height] of [
        [800, 600],
        [1280, 720],
        [1600, 1000],
    ]) {
        const page = await browser.newPage({ viewport: { width, height } });
        const errors = [];
        page.on('pageerror', (error) => errors.push(error.message));
        let result;
        try {
            await page.goto(`${base}/tools/layout-fixtures/workspace-preferences.html`);
            await page.getByRole('button', { name: 'Preferences', exact: true }).waitFor();
            await page.addScriptTag({ content: checks });
            result = await page.evaluate(() => window.runWorkspacePreferencesChecks());
            assert.deepEqual(result.failures, []);
            assert.deepEqual(errors, []);
            await page.keyboard.press('Escape');
            await page.getByRole('dialog').waitFor({ state: 'detached' });
            await page.getByRole('button', { name: 'Preferences', exact: true }).press('Enter');
            await page.getByRole('dialog').waitFor();
            console.log(`PASS ${width}x${height}`);
        } catch (error) {
            result = { ...result, failures: [...(result?.failures ?? []), error.message] };
            process.exitCode = 1;
            console.error(`FAIL ${width}x${height}: ${error.message}`);
        } finally {
            results.push({ width, height, ...result, errors });
            await page.screenshot({ path: resolve(output, `workspace-preferences-${width}.png`) });
            await page.close();
        }
    }
} finally {
    await browser.close();
    await writeFile(resolve(output, 'results.json'), JSON.stringify(results, null, 2) + '\n');
}
