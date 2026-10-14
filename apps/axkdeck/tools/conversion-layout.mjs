import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { createServer as socketServer } from 'node:net';
import { pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const server = await createServer({ server: { host: '127.0.0.1', port: 0 } });
const results = [];
let browser, port;
async function stop() { await browser?.close(); await server.close(); }
const signal = () => void stop();
process.once('SIGTERM', signal);
process.once('SIGINT', signal);
const deadline = setTimeout(signal, 120000);
try {
    await server.listen();
    port = server.httpServer.address().port;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    for (const width of [1366, 640, 390]) {
        for (const native of [false, true]) {
            const page = await browser.newPage({ viewport: { width, height: 844 } });
            page.setDefaultTimeout(10000);
            const errors = [];
            page.on('pageerror', (error) => errors.push(error.message));
            await page.goto(`http://127.0.0.1:${port}/tools/layout-fixtures/conversion.html${native ? '?native' : ''}`);
            const dialog = page.getByRole('dialog', { name: 'Import TX16W disk set' });
            await dialog.waitFor();
            const bounds = await dialog.boundingBox();
            const footer = dialog.locator('footer');
            const footerBounds = await footer.boundingBox();
            const primary = dialog.getByRole('button', { name: 'Import', exact: true });
            assert(await primary.isDisabled());
            assert(await dialog.getByRole('button', { name: 'New', exact: true }).isDisabled());
            const style = await footer.locator('button').evaluateAll((buttons) => buttons.map((button) => ({
                height: button.getBoundingClientRect().height, marginTop: getComputedStyle(button).marginTop,
                marginBottom: getComputedStyle(button).marginBottom,
            })));
            style.forEach((item) => assert.deepEqual(item, style[0]));
            assert.equal(style[0].marginTop, '0px');
            assert.equal(style[0].marginBottom, '0px');
            const report = dialog.locator('.report-scroll');
            assert(await report.evaluate((node) => node.scrollHeight > node.clientHeight));
            await dialog.getByRole('button', { name: 'Next report page' }).click();
            await dialog.getByRole('cell', { name: /^Concert-Piano-100/ }).waitFor();
            assert.deepEqual(await footer.boundingBox(), footerBounds);
            await dialog.getByRole('button', { name: 'Previous report page' }).click();
            const checks = dialog.locator('.report-scroll input[type=checkbox]');
            for (let index = 0; index < 2; ++index) {
                await checks.nth(index).focus();
                await page.keyboard.press('Space');
            }
            assert(await primary.isEnabled());
            assert.deepEqual(await dialog.boundingBox(), bounds);
            await dialog.getByRole('button', { name: 'Strict', exact: true }).click();
            assert(await primary.isDisabled());
            await dialog.getByRole('status').getByText('Conversion blocked', { exact: true }).waitFor();
            await dialog.getByRole('button', { name: 'Best', exact: true }).click();
            for (let index = 0; index < 2; ++index) await checks.nth(index).check();
            assert(await primary.isEnabled());
            assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > window.innerWidth), false);
            const source = await dialog.getByRole('region', { name: 'Disk set' }).boundingBox();
            const review = await dialog.getByRole('region', { name: 'Conversion report' }).boundingBox();
            assert(source.x + source.width <= review.x);
            assert(review.y + review.height <= footerBounds.y + 1);
            await report.evaluate((node) => { node.scrollTop = 0; });
            await page.screenshot({ path: resolve(output, `${native ? 'native' : 'import'}-${width}.png`) });
            await primary.click();
            assert.equal(await dialog.getByRole('button', { name: 'Import', exact: true }).count(), 0);
            await dialog.getByRole('button', { name: 'Refresh', exact: true }).click();
            await page.getByText('Conversion closed', { exact: true }).waitFor();
            assert.deepEqual(errors, []);
            results.push({ width, native, passed: true, style, bounds, footerBounds });
            await page.close();
        }
    }
} finally {
    clearTimeout(deadline);
    process.removeListener('SIGTERM', signal);
    process.removeListener('SIGINT', signal);
    await stop();
    if (port) await new Promise((done, reject) => {
        const socket = socketServer(); socket.once('error', reject);
        socket.listen(port, '127.0.0.1', () => socket.close((error) => error ? reject(error) : done()));
    });
    await writeFile(resolve(output, 'results.json'), `${JSON.stringify({ serverStopped: true, browserStopped: true, portReleased: !!port, results }, null, 2)}\n`);
}
