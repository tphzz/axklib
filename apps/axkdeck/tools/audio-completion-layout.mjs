import assert from 'node:assert/strict';
import { mkdir } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const base = process.argv[2] ?? 'http://127.0.0.1:5191';
const output = resolve(process.argv[3] ?? '../../../build/logs/audio-import-regressions/00001/layout');
await mkdir(output, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
try {
    for (const viewport of [{width:1366,height:768}, {width:640,height:700}]) {
        const page = await browser.newPage({viewport});
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        for (const warning of [false, true]) {
            await page.goto(`${base}/tools/layout-fixtures/audio-completion.html${warning ? '?warning' : ''}`);
            await page.getByText(/^Fits/).waitFor();
            const formats = page.getByRole('group', {name:'Sample format'});
            const native = formats.getByRole('button', {name:'a3k',exact:true});
            const later = formats.getByRole('button', {name:'a4k/a5k',exact:true});
            assert.equal(await native.getAttribute('aria-pressed'), 'true');
            const before = await page.getByRole('dialog').boundingBox();
            await later.focus();
            await page.keyboard.press('Space');
            assert.equal(await later.getAttribute('aria-pressed'), 'true');
            assert.deepEqual(await page.getByRole('dialog').boundingBox(), before);
            const controls = await formats.locator('button').evaluateAll(items => items.map(item => {
                const style = getComputedStyle(item);
                const rect = item.getBoundingClientRect();
                return {height:rect.height, top:rect.top, bottom:rect.bottom, right:rect.right, font:style.fontSize,
                    sharedFont:style.getPropertyValue('--dialog-table-header-font-size').trim()};
            }));
            assert.equal(controls[0].height, controls[1].height);
            assert.equal(controls[0].top, controls[1].top);
            assert.equal(controls[0].font, controls[0].sharedFont);
            assert(controls[1].right <= viewport.width);
            assert(controls[1].bottom <= viewport.height);
            await page.screenshot({path:resolve(output,`format-${viewport.width}.png`)});
            const footer = page.locator('.dialog-footer');
            const buttons = footer.locator('button');
            const heights = await buttons.evaluateAll(items => items.map(item => item.getBoundingClientRect().height));
            assert.equal(new Set(heights).size, 1);
            await page.getByRole('button', {name:'Import',exact:true}).click();
            if (warning) {
                const done = page.getByRole('button', {name:'Done',exact:true});
                await done.waitFor();
                assert.equal(await page.getByText(/name already exists/).count(), 0);
                await page.getByText('Imported', {exact:true}).waitFor();
                const shell = await page.getByRole('dialog').boundingBox();
                assert(shell.x >= 0 && shell.x + shell.width <= viewport.width + 1);
                assert(shell.y >= 0 && shell.y + shell.height <= viewport.height + 1);
                await page.screenshot({path:resolve(output,`warning-${viewport.width}.png`)});
                await done.click();
            }
            await page.getByText('Import closed', {exact:true}).waitFor();
        }
        assert.deepEqual(errors, []);
        await page.close();
    }
    console.log('Verified clean auto-close and retained warnings at desktop and narrow widths');
} finally { await browser.close(); }
