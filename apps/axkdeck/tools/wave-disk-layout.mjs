import assert from 'node:assert/strict';
import {mkdir} from 'node:fs/promises';
import {resolve} from 'node:path';
import {pathToFileURL} from 'node:url';
import {createServer} from 'vite';

const {chromium} = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const output = resolve(process.argv[2]);
await mkdir(output, {recursive:true});
const server = await createServer({server:{host:'127.0.0.1',port:0}});
let browser;
try {
    await server.listen();
    browser = await chromium.launch({headless:true,executablePath:process.env.CHROMIUM_PATH});
    for (const width of [1366, 640, 390]) {
        const page = await browser.newPage({viewport:{width,height:844}});
        page.setDefaultTimeout(8000);
        const errors = [];
        page.on('pageerror', err => errors.push(err.message));
        await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/tools/layout-fixtures/wave-disk.html`);
        const dialog = page.getByRole('dialog',{name:'Convert'});
        await dialog.getByRole('status').filter({hasText:'Ready'}).waitFor();
        const primary = dialog.getByRole('button',{name:'Create disk image'});
        assert(await primary.isEnabled());
        const bounds = await dialog.boundingBox(), footer = await primary.boundingBox();
        assert(bounds.x >= 0 && bounds.x + bounds.width <= width);
        assert(footer.y >= bounds.y && footer.y + footer.height <= bounds.y + bounds.height);
        assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);
        await page.screenshot({path:resolve(output,`wave-disk-${width}.png`)});
        const checkbox = dialog.getByRole('checkbox',{name:/Piano Low/});
        await checkbox.focus();
        await page.keyboard.press('Space');
        assert(!await checkbox.isChecked());
        assert(!await primary.isEnabled());
        await page.keyboard.press('Space');
        await dialog.getByRole('status').filter({hasText:'Ready'}).waitFor();
        await dialog.getByRole('button',{name:'Strict'}).click();
        await dialog.getByRole('status').filter({hasText:'blocked'}).waitFor();
        assert(!await primary.isEnabled());
        assert.deepEqual(errors,[]);
        await page.close();
        console.log(`Verified keyboard selection, strict blocker and dialog bounds at ${width}px`);
    }
} finally {
    await browser?.close();
    await server.close();
}
