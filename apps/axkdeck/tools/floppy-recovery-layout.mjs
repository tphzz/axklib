import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const server = await createServer({ server: { host: '127.0.0.1', port: 0, strictPort: true } });
let browser, port;
const results = [];
async function stop() { await browser?.close(); await server.close(); }
const signal = () => void stop();
process.once('SIGTERM', signal); process.once('SIGINT', signal);
const deadline = setTimeout(signal, 90000);
try {
    await server.listen();
    port = server.httpServer.address().port;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    for (const width of [1366, 640, 390]) {
        const page = await browser.newPage({ viewport: { width, height: 844 } });
        const errors = []; page.on('pageerror', error => errors.push(error.message));
        await page.goto(`http://127.0.0.1:${port}/tools/layout-fixtures/floppy-import.html?recovery`);
        const dialog = page.getByRole('dialog', { name: 'Import floppy', exact: true });
        await dialog.waitFor();
        const acknowledgement = dialog.getByRole('checkbox', { name: 'Import available objects despite source issues' });
        const review = dialog.getByRole('button', { name: 'Review', exact: true });
        const apply = dialog.getByRole('button', { name: 'Import', exact: true });
        assert.equal(await acknowledgement.isChecked(), false);
        assert.equal(await review.isDisabled(), true);
        assert.equal(await apply.isDisabled(), true);
        const geometry = await dialog.evaluate(node => {
            const shell = node.getBoundingClientRect();
            const footer = node.querySelector('footer').getBoundingClientRect();
            const rows = node.querySelector('.floppy-rows');
            return {
                shell: { x: shell.x, y: shell.y, right: shell.right, bottom: shell.bottom },
                footerTop: footer.top,
                overflowing: rows.scrollHeight > rows.clientHeight,
                actions: [...node.querySelectorAll('footer button')].map(button => ({
                    height: button.getBoundingClientRect().height,
                    top: button.getBoundingClientRect().top,
                    marginTop: getComputedStyle(button).marginTop,
                    marginBottom: getComputedStyle(button).marginBottom,
                })),
            };
        });
        assert(geometry.shell.x >= 0 && geometry.shell.right <= width + 1);
        assert(geometry.shell.y >= 0 && geometry.shell.bottom <= 845);
        assert(geometry.overflowing);
        for (const action of geometry.actions) {
            assert.equal(action.marginTop, '0px'); assert.equal(action.marginBottom, '0px');
            assert.equal(action.height, geometry.actions[0].height);
            assert.equal(action.top, geometry.actions[0].top);
        }
        await acknowledgement.focus(); await page.keyboard.press('Space');
        assert.equal(await acknowledgement.isChecked(), true);
        assert.equal(await review.isEnabled(), true);
        await review.click();
        await page.waitForFunction(() => !document.querySelector('.dialog-footer .primary-button').disabled);
        await page.screenshot({ path: resolve(output, `acknowledged-${width}.png`) });
        await acknowledgement.uncheck();
        assert.equal(await apply.isDisabled(), true);
        await dialog.locator('.floppy-rows').evaluate(node => { node.scrollTop = node.scrollHeight; });
        assert.equal(await dialog.locator('footer').evaluate(node => node.getBoundingClientRect().top), geometry.footerTop);
        await page.screenshot({ path: resolve(output, `excluded-${width}.png`) });
        await page.keyboard.press('Escape'); await dialog.waitFor({ state: 'detached' });
        assert.deepEqual(errors, []);
        results.push({ width, geometry, passed: true }); await page.close();
    }
} finally {
    clearTimeout(deadline); process.removeListener('SIGTERM', signal); process.removeListener('SIGINT', signal);
    await stop();
    await writeFile(resolve(output, 'results.json'), JSON.stringify({ port, serverStopped: true, browserStopped: true, results }, null, 2) + '\n');
}
