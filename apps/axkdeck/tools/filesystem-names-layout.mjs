import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const base = process.argv[2] ?? 'http://127.0.0.1:5191';
const output = resolve(process.argv[3] ?? '../../../build/logs/filesystem-names/00001');
await mkdir(output, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const kind of ['create', 'import']) {
        for (const width of [800, 1280]) {
            for (const scale of [1, 1.5]) {
                const viewport = { width: width * scale, height: 900 * scale };
                const page = await browser.newPage({ viewport });
                const errors = [];
                page.on('pageerror', error => errors.push(error.message));
                await page.goto(`${base}/tools/layout-fixtures/filesystem-names.html?kind=${kind}&scale=${scale}`);
                const dialog = page.getByRole('dialog');
                await dialog.waitFor();
                const field = page.getByRole('textbox').first();
                if (kind === 'create') {
                    await field.fill('a.bin');
                    assert.equal(await field.inputValue(), 'A.BIN');
                    await field.press('Home');
                    await field.press('ArrowRight');
                    await field.press('b');
                    assert.equal(await field.inputValue(), 'AB.BIN');
                    assert.equal(await field.evaluate(node => node.selectionStart), 2);
                } else {
                    assert.equal(await page.getByRole('button', { name: 'Filename 1: Valid' }).count(), 1);
                    assert.equal(await page.getByRole('button', { name: /Filename [23]: Rejected/ }).count(), 2);
                }
                const geometry = await dialog.boundingBox();
                assert(geometry.x >= 0 && geometry.y >= 0 && geometry.x + geometry.width <= viewport.width && geometry.y + geometry.height <= viewport.height, JSON.stringify(geometry));
                const controls = await page.locator('.filesystem-name-field').evaluateAll(nodes => nodes.map(node => {
                    const input = node.querySelector('input');
                    const help = node.querySelector('.name-validation');
                    const a = input.getBoundingClientRect();
                    const b = help.getBoundingClientRect();
                    return { within: b.left >= a.left && b.right <= a.right && b.top >= a.top && b.bottom <= a.bottom,
                        padding: parseFloat(getComputedStyle(input).paddingRight), height: a.height };
                }));
                assert(controls.every(item => item.within && item.padding >= 30));
                for (const help of await page.locator('.name-validation button').all()) {
                    await help.focus();
                    const tooltip = page.getByRole('tooltip');
                    await tooltip.waitFor();
                    const bounds = await tooltip.boundingBox();
                    assert(bounds.x >= 7 && bounds.y >= 7 && bounds.x + bounds.width <= viewport.width - 7 && bounds.y + bounds.height <= viewport.height - 7, JSON.stringify(bounds));
                    assert.deepEqual(await dialog.boundingBox(), geometry);
                    await help.press('Escape');
                    assert.equal(await tooltip.count(), 0);
                    assert.equal(await dialog.count(), 1);
                    await field.focus();
                    await help.hover();
                    await tooltip.waitFor();
                    await help.click();
                    await page.mouse.move(1, 1);
                    assert.equal(await tooltip.count(), 1);
                    await page.screenshot({ path: resolve(output, `${kind}-${width}-${scale}.png`) });
                    await field.click();
                    assert.equal(await tooltip.count(), 0);
                }
                assert.deepEqual(errors, []);
                results.push({ kind, width, scale, controls });
                await page.close();
            }
        }
    }
    await writeFile(resolve(output, 'results.json'), `${JSON.stringify(results, null, 2)}\n`);
} finally {
    await browser.close();
}
