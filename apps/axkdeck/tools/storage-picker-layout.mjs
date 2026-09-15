import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const { chromium } = await import(
    process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright'
);
const base = process.argv[2] ?? 'http://127.0.0.1:5189';
const output = resolve(process.argv[3] ?? '../../../build/logs/storage-picker/00001/layout');
await mkdir(output, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const viewport of [{ width: 1280, height: 900 }, { width: 640, height: 640 }]) {
        const page = await browser.newPage({ viewport });
        const errors = [];
        page.on('pageerror', (error) => errors.push(error.message));
        await page.goto(`${base}/tools/layout-fixtures/storage-picker.html`);
        const list = page.getByRole('listbox', { name: 'Storage entries' });
        await page.getByRole('option', { name: /industrialkit/ }).waitFor();
        const initial = await list.locator('strong').allTextContents();
        assert(initial.indexOf('industrialkit') < initial.indexOf('ROKTON-factorykit'));
        assert(initial.indexOf('norddrms') < initial.indexOf('ROKTON-factorykit'));
        await page.screenshot({ path: resolve(output, `folders-${viewport.width}.png`) });
        await page.getByRole('option', { name: /^disk2\.hds/ }).click();
        const footer = page.locator('.dialog-footer');
        const before = await footer.boundingBox();
        await page.getByRole('button', { name: 'Load more' }).click();
        await page.getByRole('option', { name: /^Disk10\.hds/ }).waitFor();
        assert.equal(await page.getByRole('option', { name: /^disk2\.hds/ }).getAttribute('aria-selected'), 'true');
        assert.deepEqual(await footer.boundingBox(), before);
        assert.deepEqual((await list.locator('strong').allTextContents()).slice(-3), ['Disk02.hds', 'disk2.hds', 'Disk10.hds']);
        await page.goto(`${base}/tools/layout-fixtures/storage-picker.html?long`);
        await page.getByRole('option', { name: /^Disk45\.hds/ }).waitFor();
        const geometry = await list.evaluate((element) => ({ height: element.clientHeight, scroll: element.scrollHeight }));
        assert(geometry.scroll > geometry.height);
        await list.focus();
        await list.press('End');
        await page.getByRole('option', { name: /^Disk45\.hds/ }).scrollIntoViewIfNeeded();
        assert(await list.evaluate((element) => element.scrollTop > 0));
        const longFooter = await footer.boundingBox();
        await page.getByRole('button', { name: 'Load more' }).click();
        await page.getByRole('option', { name: /^Disk90\.hds/ }).waitFor();
        assert.deepEqual(await footer.boundingBox(), longFooter);
        assert.deepEqual(await list.locator('strong').allTextContents(), Array.from({ length: 90 }, (_, index) => `Disk${index + 1}.hds`));
        await list.focus();
        await list.press('End');
        const activeId = await list.getAttribute('aria-activedescendant');
        assert.match(await page.locator(`[id="${activeId}"]`).innerText(), /Disk90\.hds/);
        await page.screenshot({ path: resolve(output, `long-${viewport.width}.png`) });
        const shell = await page.getByRole('dialog').boundingBox();
        assert(shell.x >= 0 && shell.y >= 0 && shell.x + shell.width <= viewport.width + 1 && shell.y + shell.height <= viewport.height + 1);
        assert.deepEqual(errors, []);
        results.push({ viewport, geometry, shell, errors });
        await page.close();
    }
    await writeFile(resolve(output, 'results.json'), `${JSON.stringify(results, null, 2)}\n`);
} finally {
    await browser.close();
}
