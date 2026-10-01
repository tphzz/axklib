import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
const { chromium } = await import(pathToFileURL(process.env.PLAYWRIGHT_MODULE).href);
const base = process.argv[2] ?? 'http://127.0.0.1:5191';
const output = resolve(process.argv[3] ?? '../../../build/logs/sample-editor/00016');
await mkdir(output, { recursive: true });
const checks = await readFile(new URL('./sample-filter-checks.js', import.meta.url), 'utf8');
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const [width, zoom] of [[1600, 1], [1600, 1.5], [800, 1], [390, 1]]) {
        const name = `filter-${width}-${zoom}`;
        const page = await browser.newPage({ viewport: { width, height: 1000 } });
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        try {
            await page.goto(`${base}/tools/layout-fixtures/sample-editor.html?workspace`);
            await page.getByRole('button', { name: 'Editor panel', exact: true }).waitFor();
            await page.evaluate(zoom => {
                document.documentElement.style.zoom = String(zoom);
                document.querySelector('.workspace-fixture').style.height = `${innerHeight / zoom - 40}px`;
            }, zoom);
            await page.addScriptTag({ content: checks });
            const result = await page.evaluate(() => window.runSampleEditorRegression());
            assert.deepEqual(result.failures, []);
            assert.deepEqual(errors, []);
            const handle = page.locator('[data-handle="cutoff-q"]');
            await handle.scrollIntoViewIfNeeded();
            const drag = async (dy, alt, shift) => {
                const rect = await handle.boundingBox();
                if (alt) await page.keyboard.down('Alt');
                if (shift) await page.keyboard.down('Shift');
                await page.mouse.move(rect.x + rect.width / 2, rect.y + rect.height / 2);
                await page.mouse.down();
                await page.mouse.move(rect.x + rect.width / 2 + (alt ? 0 : 30), rect.y + rect.height / 2 - dy, { steps: 12 });
                await page.mouse.up();
                if (alt) await page.keyboard.up('Alt');
                if (shift) await page.keyboard.up('Shift');
            };
            const q = page.locator('input[aria-label="Q / Width"]');
            const cutoff = page.locator('input[aria-label="Cutoff"]');
            await drag(15, false, false);
            assert.equal(await q.inputValue(), '4');
            assert.ok(Number(await cutoff.inputValue()) > 44);
            await q.fill('4');
            await drag(30, true, false);
            const coarse = Number(await q.inputValue()) - 4;
            await q.fill('4');
            await drag(30, true, true);
            const fine = Number(await q.inputValue()) - 4;
            assert.ok(coarse > fine && fine >= 0, 'Shift reduces Alt-drag sensitivity');
            results.push({ name, ...result, coarse, fine }); console.log(`PASS ${name}`);
        } catch (error) {
            results.push({ name, failures: [error.message], errors }); process.exitCode = 1;
            console.error(`FAIL ${name}: ${error.message}`);
        } finally {
            await page.screenshot({ path: resolve(output, `${name}.png`) });
            await page.close();
        }
    }
} finally {
    await browser.close();
    await writeFile(resolve(output, 'filter-results.json'), JSON.stringify(results, null, 2) + '\n');
}
