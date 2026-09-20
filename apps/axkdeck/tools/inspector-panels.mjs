import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const base = process.argv[2] ?? 'http://127.0.0.1:5192';
const output = resolve(process.argv[3] ?? '../../../build/logs/sample-editor/00024/inspector-browser');
await mkdir(output, { recursive: true });
const checks = await readFile(new URL('./inspector-panels-checks.js', import.meta.url), 'utf8');
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    const page = await browser.newPage({ viewport: { width: 1000, height: 1500 } });
    page.setDefaultTimeout(10000);
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    try {
        await page.goto(`${base}/tools/layout-fixtures/inspector-panels.html`);
        await page.getByRole('button', { name: 'Stored format', exact: true }).waitFor();
        await page.addScriptTag({ content: checks });
        const result = await page.evaluate(() => window.runInspectorPanelsRegression());
        assert.deepEqual(result.failures, []);
        assert.deepEqual(errors, []);
        const properties = page.getByRole('button', { name: 'Properties', exact: true });
        await properties.focus();
        await properties.press('Enter');
        assert.equal(await properties.getAttribute('aria-expanded'), 'false');
        await properties.press('Space');
        assert.equal(await properties.getAttribute('aria-expanded'), 'true');
        const relationships = page.getByRole('button', { name: 'Relationships', exact: true });
        assert.equal(await relationships.getAttribute('aria-expanded'), 'false');
        await relationships.focus();
        await relationships.press('Space');
        assert.equal(await relationships.getAttribute('aria-expanded'), 'true');
        await relationships.press('Space');
        assert.equal(await relationships.getAttribute('aria-expanded'), 'false');
        await page.keyboard.press('Tab');
        assert.equal(await page.evaluate(() => document.activeElement?.getAttribute('aria-label')), 'Stored format');
        await relationships.focus();
        await relationships.press('Enter');
        await page.keyboard.press('Tab');
        assert.equal(await page.evaluate(() => !!document.activeElement?.closest('.inspector-relationship-group')), true);
        await page.keyboard.press('Enter');
        assert.match(await page.locator('.inspector-panels-fixture').getAttribute('data-navigation'), /:true$/);
        result.trustedKeyboard = 'Enter/Space disclosure, Tab exclusion/restoration, keyboard relationship navigation passed';
        for (const kind of ['program', 'sample-bank', 'sample', 'wave-data', 'sequence', 'files']) {
            await page.evaluate(kind => { window.inspectorPanelsFixture.reset(); window.inspectorPanelsFixture.configure({ kind, width: 260, scale: 2 }); }, kind);
            await page.waitForTimeout(150);
            await page.screenshot({ path: resolve(output, `${kind}-260-2.png`) });
        }
        results.push(result);
        console.log(`PASS inspector panels: ${result.measurements.length} layouts and trusted keyboard interactions`);
    } catch (error) {
        results.push({ failures: [error.stack], errors });
        process.exitCode = 1;
        console.error(error.stack);
        await page.screenshot({ path: resolve(output, 'failure.png') });
    } finally {
        await page.close();
    }
} finally {
    await browser.close();
    await writeFile(resolve(output, 'results.json'), JSON.stringify(results, null, 2) + '\n');
}
