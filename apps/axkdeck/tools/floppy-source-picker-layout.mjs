import assert from 'node:assert/strict';
import { mkdir } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const output = resolve(process.argv[3]);
await mkdir(output, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
try {
    for (const width of [1366, 640, 390]) {
        const page = await browser.newPage({ viewport: { width, height: 768 } });
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        const url = `${process.argv[2]}/tools/layout-fixtures/floppy-source-picker.html`;
        await page.goto(url);
        const folder = page.getByRole('button', { name: 'Select current folder' });
        await folder.waitFor();
        const before = await page.locator('.dialog-footer').boundingBox();
        await page.getByRole('option', { name: /disk1.img/ }).click();
        const files = page.getByRole('button', { name: 'Select 1 file' });
        await files.waitFor();
        assert.deepEqual(await page.locator('.dialog-footer').boundingBox(), before);
        const buttons = await page.locator('.dialog-footer button').evaluateAll(elements => elements.map(element => element.getBoundingClientRect().height));
        assert.equal(buttons[0], buttons[1]);
        await page.screenshot({ path: resolve(output, `${width}-images.png`) });
        await files.click();
        assert.equal(await page.locator('output').textContent(), 'disk1.img');
        await page.goto(url);
        const list = page.getByRole('listbox');
        await list.focus();
        await list.press('Home');
        await list.press('Enter');
        await page.getByRole('option', { name: /^disk3/ }).waitFor();
        await page.screenshot({ path: resolve(output, `${width}-folders.png`) });
        await folder.click();
        assert.equal(await page.locator('output').textContent(), 'server-directory:norddrms');
        await page.goto(url);
        await page.getByRole('option', { name: /disk1.img/ }).waitFor();
        await page.getByRole('listbox').focus();
        await page.keyboard.press('End');
        assert.ok(await page.locator('.storage-picker-list').evaluate(element => element.scrollTop > 0));
        await page.keyboard.press('Escape');
        assert.equal(await page.getByRole('dialog').count(), 0);
        assert.deepEqual(errors, []);
        console.log(`${width}: file/folder selection, keyboard, scroll, footer and dismissal passed`);
        await page.close();
    }
} finally {
    await browser.close();
}
