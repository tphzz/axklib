import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { checkModalScrollbars } from './modal-scrollbar-checks.mjs';

const { chromium } = await import(
    process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright'
);
const base = process.argv[2] ?? 'http://127.0.0.1:5191';
const output = resolve(process.argv[3] ?? '../../../build/logs/modal-scrollbars/00001/chromium');
await mkdir(output, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const viewport of [
        { width: 1366, height: 768 },
        { width: 640, height: 700 },
        { width: 390, height: 844 },
    ]) {
        for (const folders of [false, true]) {
            const page = await browser.newPage({ viewport });
            const errors = [];
            page.on('pageerror', (error) => errors.push(error.message));
            await page.goto(`${base}/tools/layout-fixtures/modal-scrollbars.html${folders ? '?folders' : ''}`);
            await page.getByRole('button', { name: 'Import floppy' }).waitFor();
            const result = await page.evaluate(checkModalScrollbars, { folders });
            await page.screenshot({ path: resolve(output, `${viewport.width}-${folders ? 'nested' : 'picker'}.png`) });
            const list = page.getByRole('listbox');
            await list.hover();
            await page.mouse.wheel(0, 300);
            await page.waitForFunction(() => document.querySelector('.storage-picker-list').scrollTop > 0);
            const backgroundScroll = await page
                .locator('.background-pane')
                .evaluateAll((panes) => panes.map((pane) => pane.scrollTop));
            assert.deepEqual(backgroundScroll, [240, 240, 240, 240]);
            await page.getByRole('button', { name: 'Cancel', exact: true }).click();
            await page.getByRole('button', { name: 'Import floppy' }).click();
            await page.getByRole('button', { name: 'Close', exact: true }).click();
            await page.getByRole('button', { name: 'Import floppy' }).click();
            await page.mouse.click(2, 2);
            assert.equal(await page.getByRole('dialog').count(), 0);
            assert.deepEqual(errors, []);
            results.push({ viewport, ...result });
            await page.close();
        }
    }
    await writeFile(resolve(output, 'results.json'), `${JSON.stringify(results, null, 2)}\n`);
    console.log(`${results.length} modal scrollbar layout cases passed`);
} finally {
    await browser.close();
}
