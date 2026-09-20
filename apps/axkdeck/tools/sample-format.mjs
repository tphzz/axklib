import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
const { chromium } = await import(pathToFileURL(process.env.PLAYWRIGHT_MODULE).href);
const base = process.argv[2] ?? 'http://127.0.0.1:5192';
const output = resolve(process.argv[3]);
await mkdir(output, { recursive: true });
const checks = await readFile(new URL('./sample-format-checks.js', import.meta.url), 'utf8');
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const width of [390, 800, 1600]) for (const native of [false, true]) {
        const name = `format-${width}-${native ? '188' : '224'}`;
        const page = await browser.newPage({ viewport: { width, height: 1000 } });
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        try {
            await page.goto(`${base}/tools/layout-fixtures/sample-editor.html?workspace&conversion-lock${native ? '&short-stereo' : ''}`);
            await page.getByRole('button', { name: 'Inspect Sample A', exact: true }).waitFor();
            await page.addScriptTag({ content: checks });
            const result = await page.evaluate(() => window.runSampleEditorRegression());
            assert.deepEqual(result.failures, []);
            assert.deepEqual(errors, []);
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
    await writeFile(resolve(output, 'format-results.json'), JSON.stringify(results, null, 2) + '\n');
}
