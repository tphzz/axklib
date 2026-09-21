import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
const { chromium } = await import(pathToFileURL(process.env.PLAYWRIGHT_MODULE).href);
const [base, outputArg] = process.argv.slice(2);
const output = resolve(outputArg);
await mkdir(output, { recursive: true });
const checks = await readFile(new URL('./bank-format-checks.js', import.meta.url), 'utf8');
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const width of [390, 800, 1600]) for (const query of ['', '?native', '?pending']) {
        const name = `bank-${width}-${query.slice(1) || 'later'}`;
        const page = await browser.newPage({ viewport: { width, height: 1000 } });
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        try {
            await page.goto(`${base}/tools/layout-fixtures/bank-format.html${query}`);
            await page.getByRole('button', { name: 'Inspect Bank', exact: true }).waitFor();
            await page.addScriptTag({ content: checks });
            const result = await page.evaluate(() => window.runBankFormatChecks());
            assert.deepEqual(result.failures, []);
            assert.deepEqual(errors, []);
            await page.getByRole('button', { name: 'Inspect Bank', exact: true }).focus();
            await page.keyboard.press('Shift+F10');
            await page.getByRole('menuitem', { name: /Convert to/ }).click();
            await page.getByRole('dialog').waitFor();
            await page.screenshot({ path: resolve(output, `${name}-dialog.png`) });
            await page.keyboard.press('Escape');
            await page.getByRole('dialog').waitFor({ state: 'detached' });
            results.push({ name, ...result });
            console.log(`PASS ${name}`);
        } catch (error) {
            results.push({ name, failures: [error.message], errors });
            process.exitCode = 1;
            console.error(`FAIL ${name}: ${error.message}`);
        } finally {
            await page.screenshot({ path: resolve(output, `${name}.png`) });
            await page.close();
        }
    }
} finally {
    await browser.close();
    await writeFile(resolve(output, 'results.json'), JSON.stringify(results, null, 2) + '\n');
}
