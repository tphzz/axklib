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
