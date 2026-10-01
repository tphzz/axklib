import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
const { chromium } = await import(pathToFileURL(process.env.PLAYWRIGHT_MODULE).href);
const base = process.argv[2] ?? 'http://127.0.0.1:5191';
const output = resolve(process.argv[3]);
await mkdir(output, { recursive: true });
const checks = await readFile(new URL('./sample-hardware-checks.js', import.meta.url), 'utf8');
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const [width, zoom] of [[1600, 1], [1600, 1.5], [800, 1], [390, 1]]) {
        const name = `hardware-${width}-${zoom}`;
        const page = await browser.newPage({ viewport: { width, height: 1000 } });
        page.setDefaultTimeout(10000);
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        try {
            await page.goto(`${base}/tools/layout-fixtures/sample-editor.html?workspace&short-stereo`);
            await page.getByRole('button', { name: 'Editor panel', exact: true }).waitFor();
            await page.evaluate(zoom => {
                document.documentElement.style.zoom = String(zoom);
                document.querySelector('.workspace-fixture').style.height = `${innerHeight / zoom - 40}px`;
            }, zoom);
            await page.addScriptTag({ content: checks });
            const result = await page.evaluate(() => window.runSampleEditorRegression());
            assert.deepEqual(result.failures, []);
            const handle = page.locator('[data-handle="gain"]');
            const gain = page.locator('input[aria-label="Gain"]');
            const drag = async shift => {
                await handle.scrollIntoViewIfNeeded();
                const point = await handle.boundingBox();
                await page.mouse.move(point.x + point.width / 2, point.y + point.height / 2);
                await page.waitForTimeout(450);
                assert.equal(await handle.evaluate(node => getComputedStyle(node).opacity), '1', 'hover shows trace handle');
                if (shift) await page.keyboard.down('Shift');
                await page.mouse.down();
                await page.mouse.move(point.x + point.width / 2, point.y + point.height / 2 - 15 * zoom, { steps: 6 });
                const frozen = await page.locator('[data-trace="filter"]').getAttribute('d');
                await page.mouse.up();
                if (shift) await page.keyboard.up('Shift');
                assert.equal(await page.locator('[data-trace="filter"]').getAttribute('d'), frozen, 'release preserves gain trace');
                return Number(await gain.inputValue());
            };
            const coarse = await drag(false);
            assert(coarse > 0, 'vertical trace drag raises gain');
            await page.getByRole('button', { name: 'Undo Sample edit', exact: true }).click();
            assert.equal(await gain.inputValue(), '0', 'pointer gain gesture is one undo');
            const fine = await drag(true);
            assert(fine > 0 && fine < coarse, 'Shift gain drag is finer');
            await page.getByRole('button', { name: 'Undo Sample edit', exact: true }).click();
            assert.equal(await page.locator('input[aria-label="Cutoff"]').inputValue(), '44');
            assert.equal(await page.locator('input[aria-label="Q / Width"]').inputValue(), '4');
            assert.deepEqual(errors, []);
            for (const [tab, sub] of [['Trim/Loop', 'Waveform'], ['Trim/Loop', 'Sample Info'], ['Map/Out', 'Pitch'], ['MIDI/CTRL', 'MIDI Set'], ['Filter', 'Filter']]) {
                await page.getByRole('tab', { name: tab, exact: true }).click();
                await page.getByLabel('Sample subpages', { exact: true }).getByRole('button', { name: sub, exact: true }).click();
                await page.screenshot({ path: resolve(output, `${name}-${sub.replaceAll(/[^a-z0-9]+/gi, '-')}.png`) });
            }
            results.push({ name, ...result, coarse, fine }); console.log(`PASS ${name}`);
        } catch (error) {
            results.push({ name, failures: [error.stack], errors }); process.exitCode = 1;
            console.error(`FAIL ${name}: ${error.message}`);
            await page.screenshot({ path: resolve(output, `${name}-failure.png`) });
        } finally { await page.close(); }
    }
} finally {
    await browser.close();
    await writeFile(resolve(output, 'hardware-results.json'), JSON.stringify(results, null, 2) + '\n');
}
