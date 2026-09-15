import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const base = process.argv[2] ?? 'http://127.0.0.1:5189';
const output = resolve(process.argv[3] ?? '../../../build/logs/inspector-relationships/00001');
await mkdir(output, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const kind of ['program', 'sample-bank', 'sample', 'wave-data']) {
        for (const width of [260, 320, 420]) {
            for (const scale of [1, 1.5]) {
                const page = await browser.newPage({ viewport: { width: 1000, height: 1050 } });
                const errors = [];
                page.on('pageerror', (error) => errors.push(error.message));
                await page.goto(`${base}/tools/layout-fixtures/inspector-relationships.html?kind=${kind}&width=${width}&scale=${scale}`);
                const section = page.locator('.inspector-relationships');
                await section.getByRole('heading', { name: 'Relationships' }).waitFor();
                const geometry = await page.evaluate(() => {
                    const property = document.querySelector('.metadata-list dd').getBoundingClientRect();
                    const label = document.querySelector('.metadata-list dt').getBoundingClientRect();
                    const body = document.querySelector('.inspector-body');
                    const rows = [...document.querySelectorAll('.inspector-relationship-group button, .inspector-relationship-unresolved')];
                    return {
                        overflow: body.scrollWidth > body.clientWidth,
                        rows: rows.map((row) => {
                            const name = row.querySelector('strong');
                            const detail = row.querySelector('span');
                            const bounds = row.getBoundingClientRect();
                            const detailBounds = detail?.getBoundingClientRect();
                            return {
                                leftDelta: name.getBoundingClientRect().left - label.left,
                                rightDelta: detailBounds ? detailBounds.right - property.right : 0,
                                detail: detail?.textContent ?? '',
                                wraps: detail ? detail.clientHeight > parseFloat(getComputedStyle(detail).lineHeight) * 1.5 : false,
                                detailOverflow: detail ? detail.scrollWidth > detail.clientWidth + 1 : false,
                                columnsOverlap: detailBounds ? name.getBoundingClientRect().right > detailBounds.left : false,
                                title: name.title,
                                height: bounds.height,
                            };
                        }),
                    };
                });
                assert.equal(geometry.overflow, false);
                for (const row of geometry.rows) {
                    assert(Math.abs(row.leftDelta) <= 1, JSON.stringify(row));
                    assert(Math.abs(row.rightDelta) <= 1, JSON.stringify(row));
                    assert(!row.detailOverflow && !row.columnsOverlap, JSON.stringify(row));
                    assert(row.title.length > 0);
                    assert(!/Assignment|Assigned by|Member|Wave Data|\.\.\./.test(row.detail));
                }
                if (kind !== 'wave-data') {
                    assert(geometry.rows.some((row) => row.detail.startsWith('A01') && row.wraps));
                    assert(await section.getByTitle('Receive channel').count() > 0);
                }
                if (kind === 'sample' || kind === 'wave-data') {
                    assert(geometry.rows.some((row) => row.detail === 'Left / Right' && !row.wraps));
                }
                assert.equal(await section.locator('span').filter({ hasText: /^$/ }).count(), 0);
                const footer = page.locator('.inspector-mode-footer');
                const before = await footer.boundingBox();
                const button = section.getByRole('button').first();
                await button.click();
                assert.match(await page.locator('.fixture').getAttribute('data-navigation'), /:false$/);
                await button.focus();
                await button.press('Enter');
                assert.match(await page.locator('.fixture').getAttribute('data-navigation'), /:true$/);
                await page.locator('.inspector-body').evaluate((body) => body.scrollTop = body.scrollHeight);
                assert.deepEqual(await footer.boundingBox(), before);
                await page.locator('.inspector-body').evaluate((body) => body.scrollTop = 0);
                await page.screenshot({ path: resolve(output, `${kind}-${width}-${scale}.png`) });
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
