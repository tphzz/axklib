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
            const dialog = page.getByRole('dialog', { name: native ? 'Convert' : 'Import TX16W disk set' });
            await dialog.waitFor();
            const bounds = await dialog.boundingBox();
            const footer = dialog.locator('footer');
            const footerBounds = await footer.boundingBox();
            const primary = dialog.getByRole('button', { name: native ? 'Add to volume' : 'Import', exact: true });
            assert(await primary.isEnabled(), 'Best does not require viewing or acknowledging conversion notes');
            const volumeMode = dialog.getByRole('group', {name:'Destination volume type'});
            if (native) assert(await dialog.getByRole('group',{name:'Destination image type'}).getByRole('button',{name:'New'}).isDisabled());
            await volumeMode.getByRole('button',{name:'New'}).click();
            const name = dialog.getByRole('textbox',{name:'New volume name'});
            const create = dialog.getByRole('button',{name:'Create volume and import'});
            assert(await create.isEnabled());
            await page.screenshot({ path: resolve(output, `${native ? 'native' : 'import'}-${width}-create.png`) });
            await name.fill('Samples');
            assert.equal(await name.getAttribute('aria-invalid'),'true');
            assert(await create.isDisabled());
            await dialog.getByText('Name already exists in this partition.').waitFor();
            if (native) {
                const format = await dialog.getByRole('combobox', { name: 'Target format' }).boundingBox();
                const destination = await dialog.locator('.disk-import-settings').first().boundingBox();
                assert(format.y + format.height <= destination.y, 'Wrapped target format must clear the destination row');
            }
            await page.screenshot({ path: resolve(output, `${native ? 'native' : 'import'}-${width}-duplicate.png`) });
            await name.fill('New keys');
            assert(await create.isEnabled());
            const partition = dialog.getByRole('combobox',{name:'Destination partition'});
            await partition.click();
            await dialog.getByRole('option',{name:'PARTITION 2'}).click();
            assert(await volumeMode.getByRole('button',{name:'Existing'}).isDisabled());
            assert.equal(await name.inputValue(),'New keys');
            await page.screenshot({ path: resolve(output, `${native ? 'native' : 'import'}-${width}-empty.png`) });
            await partition.click();
            await dialog.getByRole('option',{name:'PARTITION 1'}).click();
            await volumeMode.getByRole('button',{name:'Existing'}).click();
            assert(await primary.isEnabled());
            const style = await footer.locator('button').evaluateAll((buttons) => buttons.map((button) => ({
                height: button.getBoundingClientRect().height, marginTop: getComputedStyle(button).marginTop,
                marginBottom: getComputedStyle(button).marginBottom,
            })));
            style.forEach((item) => assert.deepEqual(item, style[0]));
            assert.equal(style[0].marginTop, '0px');
            assert.equal(style[0].marginBottom, '0px');
            const report = dialog.locator('[data-pane="contents"] .import-pane-scroll');
            if (native) {
                const browse = dialog.getByRole('button', {name: /^Browse/});
                assert(await browse.isEnabled());
                const overlap = (a, b) => Math.min(a.y + a.height, b.y + b.height) > Math.max(a.y, b.y);
                assert(overlap(await dialog.getByLabel('File', {exact: true}).boundingBox(), await browse.boundingBox()), 'File and Browse stay on one line at every width');
                assert(overlap(await dialog.getByText('Target format', {exact:true}).boundingBox(), await dialog.getByRole('combobox', {name:'Target format'}).boundingBox()), 'Target format and its selector stay together');
                if (width >= 1000) {
                    assert(overlap(await dialog.getByRole('combobox', {name:'Destination medium'}).boundingBox(), await browse.boundingBox()), 'Destination medium, mode and browse share one desktop row');
                }
                const source = dialog.getByRole('tab', { name: 'Source selection' });
                assert.equal(await source.getAttribute('aria-selected'), 'true');
                const layer = dialog.getByRole('checkbox', { name: /EP Layer/ });
                await layer.focus();
                await page.keyboard.press('Space');
                await dialog.getByText('1 of 2 selected').waitFor();
                await dialog.getByText('Required by Stage Piano').waitFor();
                await dialog.getByText('Piano Voice', { exact:true }).waitFor();
                assert.equal(await dialog.getByRole('checkbox', { name: /Piano Voice/ }).count(), 0);
                if (width >= 1000) {
                    await page.screenshot({ path: resolve(output, 'native-source-ready-wide.png') });
                    await dialog.getByRole('region', { name: 'Import review' }).focus();
                    await page.setViewportSize({width:640,height:844});
                    await page.waitForFunction(() => document.activeElement?.getAttribute('role') === 'tab' && document.activeElement?.textContent?.trim() === 'Source selection');
                    await page.setViewportSize({width,height:844});
                    await dialog.getByRole('region', { name: 'Review', exact:true }).waitFor();
                }
                await dialog.getByRole('tab', { name: 'Planned output' }).click();
            }
            assert(await report.evaluate((node) => node.scrollHeight > node.clientHeight));
            await dialog.getByRole('button', { name: 'Next objects page' }).click();
            await dialog.getByText('Concert-Piano-100', { exact: true }).waitFor();
            assert.deepEqual(await footer.boundingBox(), footerBounds);
            await dialog.getByRole('button', { name: 'Previous objects page' }).click();
            if (await dialog.getByRole('tab', { name: /^Review/ }).count()) await dialog.getByRole('tab', { name: /^Review/ }).click();
            const checks = dialog.locator('[data-pane="review"] input[type=checkbox]');
            assert.equal(await checks.count(), 0);
            await dialog.getByRole('heading', { name: 'Conversion notes' }).waitFor();
            if (native) await dialog.locator('[data-pane="review"]').getByText(/Source: PFC2/).first().waitFor();
            assert.deepEqual(await dialog.boundingBox(), bounds);
            await dialog.getByRole('button', { name: 'Strict', exact: true }).click();
            assert(await primary.isDisabled());
            await dialog.getByRole('status').getByText('Conversion blocked', { exact: true }).waitFor();
            await dialog.getByRole('button', { name: 'Best', exact: true }).click();
            assert(await primary.isEnabled());
            assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > window.innerWidth), false);
            const contents = await dialog.locator('[data-pane="contents"]').boundingBox();
            const review = await dialog.locator('[data-pane="review"]').boundingBox();
            if (!await dialog.getByRole('tab', { name: /^Review/ }).count()) assert(contents.x + contents.width <= review.x);
            assert((await dialog.locator('.disk-import-body').boundingBox()).y < footerBounds.y);
            assert.equal(await dialog.getByRole('region', { name: 'Source disks' }).count(), native ? 0 : 1);
            await report.evaluate((node) => { node.scrollTop = 0; });
            await page.screenshot({ path: resolve(output, `${native ? 'native' : 'import'}-${width}.png`) });
            await primary.click();
            assert.equal(await primary.count(), 0);
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
