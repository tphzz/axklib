import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const base = process.argv[2] ?? 'http://127.0.0.1:5191';
const output = resolve(process.argv[3] ?? '../../../build/logs/files-attributes/00001');
await mkdir(output, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const kind of ['file', 'directory', 'fat']) {
        for (const width of [260, 320, 420]) {
            for (const scale of [1, 1.5]) {
                const page = await browser.newPage({ viewport: { width: 900, height: 1000 } });
                const errors = [];
                page.on('pageerror', (error) => errors.push(error.message));
                await page.goto(`${base}/tools/layout-fixtures/files-attributes.html?kind=${kind}&width=${width}&scale=${scale}`);
                const details = page.locator('details');
                const summary = page.locator('summary');
                await summary.waitFor();
                assert.equal(await details.getAttribute('open'), null);
                const properties = page.getByRole('region', { name: 'Entry properties' });
                assert.doesNotMatch(await properties.innerText(), /0x|Filesystem references|Directory write flag/);
                const footer = page.locator('.inspector-mode-footer');
                const before = kind === 'directory' ? null : await footer.boundingBox();
                await summary.focus();
                await summary.press('Enter');
                assert.notEqual(await details.getAttribute('open'), null);
                const geometry = await page.evaluate(() => {
                    const body = document.querySelector('.inspector-body');
                    return {
                        overflow: body.scrollWidth > body.clientWidth,
                        rows: [...document.querySelectorAll('.metadata-list > div')].map((row) => {
                            const label = row.querySelector('dt').getBoundingClientRect();
                            const value = row.querySelector('dd').getBoundingClientRect();
                            return { overlap: label.right > value.left, right: value.right, bottom: value.bottom };
                        }),
                    };
                });
                assert.equal(geometry.overflow, false);
                assert(geometry.rows.every((row) => !row.overlap));
                assert(geometry.rows.every((row) => Math.abs(row.right - geometry.rows[0].right) <= 1));
                assert(await page.locator('.metadata-list > div:not(:last-child)').evaluateAll((rows) =>
                    rows.length > 0 && rows.every((row) => getComputedStyle(row).borderBottomStyle === 'solid' && parseFloat(getComputedStyle(row).borderBottomWidth) > 0)));
                assert.equal(await page.locator('.attribute-description').count(), 0);
                if (kind === 'file') assert.equal(await page.getByRole('button', { name: 'File write flag', exact: true }).count(), 1);
                if (kind === 'directory') assert.equal(await page.getByRole('button', { name: 'Temporary directory write flag', exact: true }).count(), 1);
                const labels = page.locator('.metadata-list .attribute-help-label');
                const inspectorBefore = await page.locator('.inspector').boundingBox();
                for (const label of await labels.all()) {
                    assert.equal(await label.evaluate((node) => getComputedStyle(node).textDecorationLine), 'none');
                    await label.focus();
                    assert.equal(await label.evaluate((node) => node.matches(':focus-visible') && getComputedStyle(node).outlineStyle === 'solid' && parseFloat(getComputedStyle(node).outlineWidth) > 0), true);
                    const tooltip = page.getByRole('tooltip');
                    await tooltip.waitFor();
                    assert((await tooltip.innerText()).length > 20);
                    assert.equal(await tooltip.evaluate((node) => getComputedStyle(node).whiteSpace), 'pre-line');
                    const labelText = await label.innerText();
                    if (['File write flag', 'Temporary directory write flag', 'Record state', 'Native record type', 'Allocation policy'].includes(labelText)) {
                        assert.match(await tooltip.textContent(), /\n\n\S/);
                    }
                    const bounds = await tooltip.boundingBox();
                    assert(bounds.x >= 7 && bounds.y >= 7, JSON.stringify(bounds));
                    assert(bounds.x + bounds.width <= 901 && bounds.y + bounds.height <= 1001, JSON.stringify(bounds));
                    assert.equal(await tooltip.evaluate((node) => node.scrollWidth > node.clientWidth), false);
                    assert.deepEqual(await page.locator('.inspector').boundingBox(), inspectorBefore);
                    await label.press('Escape');
                    assert.equal(await tooltip.count(), 0);
                }
                const help = labels.first();
                await page.locator('summary').focus();
                await help.hover();
                await page.getByRole('tooltip').waitFor();
                await help.click();
                await page.mouse.move(1, 1);
                assert.equal(await page.getByRole('tooltip').count(), 1);
                await page.screenshot({ path: resolve(output, `${kind}-${width}-${scale}-help.png`) });
                await page.mouse.click(1, 1);
                assert.equal(await page.getByRole('tooltip').count(), 0);
                await page.locator('.inspector-body').evaluate((body) => body.scrollTop = body.scrollHeight);
                if (before) {
                    assert.deepEqual(await footer.boundingBox(), before);
                    await page.getByRole('button', { name: 'To Device' }).click();
                    assert.equal(await page.locator('.fixture').getAttribute('data-navigated'), 'true');
                }
                await page.locator('.inspector-body').evaluate((body) => body.scrollTop = 0);
                await page.screenshot({ path: resolve(output, `${kind}-${width}-${scale}.png`) });
                await summary.click();
                assert.equal(await details.getAttribute('open'), null);
                assert.deepEqual(errors, []);
                results.push({ kind, width, scale, geometry });
                await page.close();
            }
        }
    }
    await writeFile(resolve(output, 'results.json'), `${JSON.stringify(results, null, 2)}\n`);
} finally {
    await browser.close();
}
