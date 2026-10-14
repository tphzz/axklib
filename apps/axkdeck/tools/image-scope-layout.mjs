import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { createServer as socketServer } from 'node:net';
import { pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const availablePort = await new Promise((resolve, reject) => {
    const socket = socketServer(); socket.once('error', reject);
    socket.listen(0, '127.0.0.1', () => {
        const port = socket.address().port;
        socket.close((error) => error ? reject(error) : resolve(port));
    });
});
const server = await createServer({ server: { host: '127.0.0.1', port: availablePort, strictPort: true } });
const results = [];
let browser, port, currentPage;
async function stop() { await browser?.close(); await server.close(); }
const signal = () => void stop();
process.once('SIGTERM', signal);
process.once('SIGINT', signal);
const deadline = setTimeout(signal, 180000);
try {
    await server.listen();
    port = server.httpServer.address().port;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    for (const [width, scale] of [[1366, 1], [915, 1], [640, 1], [390, 1], [1366, 1.25], [1366, 1.5]]) {
        const logicalWidth = Math.ceil(width / scale);
        const page = await browser.newPage({ viewport: { width: logicalWidth, height: Math.ceil(844 / scale) }, deviceScaleFactor: scale });
        currentPage = page;
        page.setDefaultTimeout(10000);
        const errors = [];
        page.on('pageerror', (error) => errors.push(error.message));
        await page.goto(`http://127.0.0.1:${port}/tools/layout-fixtures/image-scope.html`);
        const selector = page.getByRole('combobox', { name: 'Image', exact: true });
        await selector.waitFor();
        await page.waitForFunction(() => !document.querySelector('[aria-label="Image"]').disabled);
        assert.deepEqual(await selector.locator('option').allTextContents(), [
            'HD50_partition-test.hds', 'single-cycle-waveforms.hds', 'tx16w-1.img', 'tx16w-2.img', 'plain.img',
        ]);
        assert.equal(await page.locator('.source-context').count(), 0);
        assert.equal(await page.getByText('Files only', { exact: true }).count(), 0);
        assert.equal(await page.getByRole('combobox', { name: 'Native document' }).count(), 0);
        await page.getByText('Multiple', { exact: true }).waitFor();
        assert.equal(await page.getByText('Multiple', { exact: true }).evaluate((label) => label.scrollWidth > label.clientWidth), false);
        await page.getByRole('button', { name: 'Eject all images', exact: true }).waitFor();
        await page.getByRole('button', { name: 'Files', exact: true }).click();
        const rows = page.getByRole('treegrid', { name: 'Filesystem entries' });
        const search = page.getByRole('searchbox', { name: 'Search filesystem' });
        const select = async (id) => {
            await selector.selectOption(id);
            await page.waitForFunction((value) => {
                const control = document.querySelector('[aria-label="Image"]');
                return control?.value === value && !control.disabled;
            }, id);
        };
        await rows.getByText('File 1', { exact: true }).waitFor();
        assert.equal(await page.getByRole('group', { name: 'Filesystem roots' }).getByRole('button').count(), 1);
        await search.fill('File 1');
        await rows.locator('small[title="/File 1"]').waitFor();
        await selector.focus();
        await page.keyboard.press('ArrowDown');
        await page.keyboard.press('Enter');
        await page.waitForFunction(() => document.querySelector('[aria-label="Image"]')?.value === 'source-2');
        await page.getByRole('button', { name: 'Files', exact: true }).click();
        await rows.getByText('File 2', { exact: true }).waitFor();
        assert.equal(await page.getByRole('button', { name: 'Partition 1', exact: true }).count(), 0);
        assert.equal(await search.inputValue(), '');
        await select('source-1');
        await rows.locator('small[title="/File 1"]').waitFor();
        assert.equal(await search.inputValue(), 'File 1');
        await page.getByRole('button', { name: 'Refresh fixture' }).click();
        await rows.locator('small[title="/File 1"]').waitFor();
        await selector.focus();
        const geometry = await selector.evaluate((control) => {
            const box = control.getBoundingClientRect(), header = control.closest('.app-header').getBoundingClientRect();
            const style = getComputedStyle(control);
            return { top: box.top, bottom: box.bottom, left: box.left, right: box.right, headerTop: header.top,
                headerBottom: header.bottom, headerLeft: header.left, headerRight: header.right,
                colorScheme: style.colorScheme, outline: style.outlineWidth };
        });
        assert(geometry.headerBottom - geometry.bottom >= 6, 'Selector must clear the horizontal separator');
        assert(geometry.top - geometry.headerTop >= 6, 'Selector focus ring must fit above the control');
        assert(geometry.left >= 0 && geometry.right <= logicalWidth + 1);
        assert(geometry.headerLeft >= 0 && geometry.headerRight <= logicalWidth + 1, 'The complete header must fit the viewport');
        assert.equal(geometry.colorScheme, 'dark');
        assert.notEqual(geometry.outline, '0px');
        assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > window.innerWidth), false);
        await page.screenshot({ path: resolve(output, `files-${width}-${scale}.png`) });
        await select('source-3');
        await page.getByRole('button', { name: 'Waves', exact: true }).click();
        await page.getByRole('button', { name: 'Wave 20', exact: true }).click();
        await page.getByRole('searchbox', { name: 'Search Waves' }).fill('Wave 2');
        await select('source-4');
        assert.equal(await page.getByRole('searchbox', { name: 'Search Waves' }).inputValue(), 'Wave 2');
        await select('source-3');
        if (width / scale > 850) {
            await page.getByRole('button', { name: 'To Files', exact: true }).click();
            await rows.locator('[data-file-entry="source-4:file"][aria-selected="true"]').waitFor();
            assert.equal(await selector.inputValue(), 'source-4');
        }
        await select('source-5');
        await rows.getByText('File 5', { exact: true }).waitFor();
        assert.equal(await page.getByRole('button', { name: 'Device', exact: true }).isDisabled(), true);
        for (const label of ['Decline switch', 'Fail switch']) {
            await page.getByRole('checkbox', { name: label }).check();
            await selector.selectOption('source-2');
            await page.waitForFunction(() => {
                const control = document.querySelector('[aria-label="Image"]');
                return control?.value === 'source-5' && !control.disabled;
            });
            await rows.getByText('File 5', { exact: true }).waitFor();
            await page.getByRole('checkbox', { name: label }).uncheck();
        }
        await page.getByRole('checkbox', { name: 'Block switching' }).check();
        assert.equal(await selector.isDisabled(), true);
        await page.getByRole('checkbox', { name: 'Block switching' }).uncheck();
        await page.getByRole('button', { name: 'Eject all images' }).click();
        await selector.waitFor({ state: 'detached' });
        assert.deepEqual(errors, []);
        results.push({ width, scale, logicalWidth, passed: true, geometry });
        await page.close();
    }
} catch (error) {
    if (currentPage && !currentPage.isClosed()) {
        await currentPage.screenshot({ path: resolve(output, 'failure.png') });
        await writeFile(resolve(output, 'failure.html'), await currentPage.content());
    }
    throw error;
} finally {
    clearTimeout(deadline);
    process.removeListener('SIGTERM', signal);
    process.removeListener('SIGINT', signal);
    await stop();
    if (port) await new Promise((resolve, reject) => {
        const socket = socketServer(); socket.once('error', reject);
        socket.listen(port, '127.0.0.1', () => socket.close((error) => error ? reject(error) : resolve()));
    });
    await writeFile(resolve(output, 'results.json'), `${JSON.stringify({ serverStopped: true, browserStopped: true, portReleased: !!port, results }, null, 2)}\n`);
}
