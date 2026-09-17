import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const base = process.argv[2] ?? 'http://127.0.0.1:5191';
const output = resolve(process.argv[3] ?? '../../../build/logs/import-capacity/00001/layout');
await mkdir(output, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const viewport of [{width:1366,height:768}, {width:640,height:700}, {width:390,height:844}]) {
        const page = await browser.newPage({viewport});
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        await page.goto(`${base}/tools/layout-fixtures/floppy-import.html?capacity`);
        await page.getByText('Not enough space on Partition 1', {exact:true}).waitFor();
        assert.equal(await page.getByText('63 issues prevent import', {exact:true}).count(), 0);
        assert.equal(await page.getByText('Insert', {exact:true}).count(), 0);
        assert(await page.getByRole('button', {name:'Import', exact:true}).isDisabled());
        const footer = page.locator('.dialog-footer');
        const initialFooter = await footer.boundingBox();
        const disclosure = page.locator('summary').filter({hasText:'Technical details'});
        assert.equal(await page.locator('details').getAttribute('open'), null);
        await page.screenshot({path:resolve(output,`collapsed-${viewport.width}.png`)});
        await disclosure.focus();
        await page.keyboard.press('Enter');
        assert.notEqual(await page.locator('details').getAttribute('open'), null);
        const scroll = page.locator('.floppy-results');
        assert(await scroll.evaluate(element => element.scrollHeight > element.clientHeight));
        await scroll.evaluate(element => { element.scrollTop = element.scrollHeight; });
        assert.deepEqual(await footer.boundingBox(), initialFooter);
        const shell = await page.getByRole('dialog').boundingBox();
        assert(shell.x >= 0 && shell.x + shell.width <= viewport.width + 1);
        assert(shell.y >= 0 && shell.y + shell.height <= viewport.height + 1);
        await page.screenshot({path:resolve(output,`expanded-${viewport.width}.png`)});
        await disclosure.focus();
        await page.keyboard.press('Space');
        assert.equal(await page.locator('details').getAttribute('open'), null);
        assert.deepEqual(await footer.boundingBox(), initialFooter);
        assert.deepEqual(errors, []);
        results.push({viewport,footer:initialFooter,shell});
        await page.close();
    }
    await writeFile(resolve(output, 'geometry.json'), JSON.stringify(results, null, 2) + '\n');
    console.log(`Verified ${results.length} viewports`);
} finally { await browser.close(); }
