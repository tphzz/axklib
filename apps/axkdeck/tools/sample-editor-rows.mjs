import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
const { chromium } = await import(pathToFileURL(process.env.PLAYWRIGHT_MODULE).href);
const base = process.argv[2] ?? 'http://127.0.0.1:5191';
const output = resolve(process.argv[3] ?? '../../../build/logs/sample-editor/00012');
await mkdir(output, { recursive: true });
const checks = await readFile(new URL('./sample-editor-row-checks.js', import.meta.url), 'utf8');
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const [width, zoom] of [[1600, 1], [1600, 1.25], [1600, 1.5], [1600, 2], [800, 1], [390, 1]]) {
        for (const stereo of [false, true]) {
            const name = `rows-${width}-${zoom}-${stereo ? 'stereo' : 'mono'}`;
            const page = await browser.newPage({ viewport: { width, height: 1000 } });
            const errors = [];
            page.on('pageerror', error => errors.push(error.message));
            try {
                await page.goto(`${base}/tools/layout-fixtures/sample-editor.html?workspace${stereo ? '&short-stereo' : ''}`);
                await page.getByRole('button', { name: 'Editor panel', exact: true }).waitFor();
                await page.evaluate(zoom => {
                    document.documentElement.style.zoom = String(zoom);
                    document.querySelector('.workspace-fixture').style.height = `${innerHeight / zoom - 40}px`;
                }, zoom);
                await page.addScriptTag({ content: checks });
                const result = await page.evaluate(() => window.runSampleEditorRegression());
                assert.deepEqual(result.failures, []);
                if (width === 1600 && zoom <= 1.5) {
                    const value = page.getByRole('spinbutton', { name: 'EQ width', exact: true });
                    await value.fill('6'); await value.press('Tab');
                    const handle = page.locator('[data-handle="frequency-gain"]');
                    const before = await page.locator('[data-handle="frequency-gain"]').evaluate(n => [n.style.left, n.style.top]);
                    const bounds = await page.locator('.parameter-plot').boundingBox();
                    const dx = bounds.width / 10;
                    for (const fine of [false, true]) {
                        const point = await handle.boundingBox();
                        await page.keyboard.down('Alt');
                        if (fine) await page.keyboard.down('Shift');
                        await page.mouse.move(point.x + point.width / 2, point.y + point.height / 2);
                        await page.mouse.down(); await page.mouse.move(point.x + point.width / 2 + dx, point.y + point.height / 2, { steps: 10 }); await page.mouse.up();
                        await page.keyboard.up('Alt'); if (fine) await page.keyboard.up('Shift');
                        const expected = fine ? 6.6 : 8.2;
                        assert(Math.abs(Number(await value.inputValue()) - expected) <= 0.1, `Alt${fine ? '+Shift' : ''} drag width`);
                        assert.deepEqual(await handle.evaluate(n => [n.style.left, n.style.top]), before);
                        await page.getByRole('button', { name: 'Undo Sample edit' }).click();
                        assert.equal(await value.inputValue(), '6');
                    }
                }
                assert.deepEqual(errors, []);
                await page.screenshot({ path: resolve(output, `${name}.png`) });
                results.push({ name, ...result }); console.log(`PASS ${name}`);
            } catch (error) {
                results.push({ name, failures: [error.message], errors }); process.exitCode = 1;
                console.error(`FAIL ${name}: ${error.message}`);
                await page.screenshot({ path: resolve(output, `${name}-failure.png`) });
            } finally { await page.close(); }
        }
    }
} finally {
    await browser.close();
    await writeFile(resolve(output, 'row-results.json'), JSON.stringify(results, null, 2) + '\n');
}
