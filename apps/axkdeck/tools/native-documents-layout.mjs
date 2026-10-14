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
    for (const [width, scale] of [[1366, 1], [915, 1], [640, 1], [390, 1], [1366, 1.25], [1366, 1.5]]) {
        const logicalWidth = Math.ceil(width / scale);
        const page = await browser.newPage({ viewport: { width: logicalWidth, height: Math.ceil(844 / scale) }, deviceScaleFactor: scale });
        page.setDefaultTimeout(15000);
        const errors = [];
        page.on('pageerror', (error) => errors.push(error.message));
        await page.goto(`http://127.0.0.1:${port}/tools/layout-fixtures/native-documents.html`);
        const dialog = page.getByRole('dialog', { name: 'Open', exact: true });
        await dialog.waitFor();
        const before = await dialog.boundingBox();
        await dialog.getByRole('option', { name: /Yamaha images/ }).click();
        await dialog.getByRole('option', { name: /^Disk1\.hds/ }).click();
        await dialog.getByRole('option', { name: /^Disk2\.hds/ }).click();
        assert.equal(await dialog.getByRole('region', { name: 'Source set' }).getByRole('listitem').count(), 2);
        await dialog.getByRole('button', { name: /Remove .*Disk1.hds/ }).click();
        assert.equal(await dialog.getByRole('option', { name: /^Disk1\.hds/ }).getAttribute('aria-selected'), 'false');
        await dialog.getByRole('option', { name: /^Disk1\.hds/ }).click();
        await dialog.getByRole('button', { name: 'Load more', exact: true }).click();
        await dialog.getByRole('option', { name: /^Disk90\.hds/ }).click();
        assert.equal(await dialog.getByRole('region', { name: 'Source set' }).getByRole('listitem').count(), 3);
        await dialog.getByRole('button', { name: 'Add current folder' }).click();
        await dialog.getByRole('button', { name: 'Remove current folder' }).click();
        assert.equal(await dialog.locator('.open-source-panel:not([hidden]) .dialog-footer').count(), 1);
        assert.equal(await dialog.getByRole('button', { name: /^Add \d/ }).count(), 0);
        assert.deepEqual(await dialog.boundingBox(), before, 'Selection must not resize the dialog');
        const footer = dialog.locator('.open-source-panel:not([hidden]) > footer');
        const geometry = await footer.locator('button').evaluateAll((buttons) => buttons.map((button) => ({
            height: button.getBoundingClientRect().height, top: button.getBoundingClientRect().top,
            marginTop: getComputedStyle(button).marginTop, marginBottom: getComputedStyle(button).marginBottom,
        })));
        assert(geometry.length >= 2);
        geometry.forEach((item) => assert.deepEqual(item, geometry[0]));
        assert.equal(geometry[0].marginTop, '0px');
        assert.equal(geometry[0].marginBottom, '0px');
        await page.screenshot({ path: resolve(output, `sources-${width}-${scale}.png`) });
        await dialog.getByRole('button', { name: 'Open', exact: true }).click();
        await dialog.waitFor({ state: 'detached' });
        const selector = page.getByRole('combobox', { name: 'Image', exact: true });
        await selector.waitFor({ state: 'visible' });
        assert.equal(await selector.evaluate((node) => getComputedStyle(node).colorScheme), 'dark');
        const tabs = page.getByRole('navigation', { name: 'Workspace views' });
        assert.deepEqual(await tabs.locator('button').evaluateAll((nodes) => nodes.map((node) => node.getAttribute('aria-label'))), ['Performances', 'Voices', 'Timbres', 'Waves', 'Setups', 'Native Files']);
        assert.equal(await tabs.getByRole('button', { name: 'Performances', exact: true }).getAttribute('aria-pressed'), 'true');
        const root = page.getByRole('navigation', { name: 'TX16W structure' }).getByRole('button', { name: '/', exact: true });
        assert.equal(await page.getByRole('button', { name: /All objects/i }).count(), 0);
        if (await root.isVisible()) {
            assert.notEqual(await root.evaluate((node) => getComputedStyle(node).backgroundImage), 'none', 'Structural selection must remain visible');
            await root.click({ button: 'right' });
            await page.getByRole('menuitem', { name: 'Convert...', exact: true }).click();
            assert.equal(await page.getByLabel('Conversion requests').textContent(), '1');
            await root.focus();
            await page.keyboard.press('Shift+F10');
            await page.getByRole('menuitem', { name: 'Convert...', exact: true }).waitFor();
            await page.keyboard.press('Escape');
            assert(await root.evaluate((node) => node === document.activeElement));
        }
        await tabs.getByRole('button', { name: 'Waves', exact: true }).click();
        await page.getByRole('button', { name: 'Wave 20', exact: true }).click();
        assert.notEqual(await page.getByRole('region', { name: 'TX16W native objects' }).locator('tr.selected').evaluate((node) => getComputedStyle(node).backgroundImage), 'none', 'Selected native object must remain visible');
        const inspector = page.locator('[aria-label="TX16W inspector"]');
        const inspectorState = logicalWidth > 850 ? 'visible' : 'attached';
        await inspector.getByText('33333', { exact: true }).waitFor({ state: inspectorState });
        await inspector.getByText('AMBIGUOUS', { exact: true }).waitFor({ state: inspectorState });
        assert.equal(await inspector.isVisible(), logicalWidth > 850, 'Preserve the shared narrow-screen inspector layout');
        await page.getByRole('searchbox', { name: 'Search Waves' }).fill('Wave 2');
        await tabs.getByRole('button', { name: 'Voices', exact: true }).click();
        await page.getByRole('searchbox', { name: 'Search Voices' }).fill('Piano');
        await tabs.getByRole('button', { name: 'Waves', exact: true }).click();
        assert.equal(await page.getByRole('searchbox', { name: 'Search Waves' }).inputValue(), 'Wave 2');
        if (logicalWidth > 850) {
            await inspector.getByRole('button', { name: 'Candidate: Wave 21' }).click();
            assert(await page.getByRole('region', { name: 'TX16W native objects' }).getByRole('button', { name: 'Wave 21', exact: true }).evaluate((node) => node === document.activeElement));
        }
        await page.getByRole('button', { name: 'Files', exact: true }).click();
        await page.getByText('Files workspace').first().waitFor();
        const fileInspector = page.getByRole('complementary', { name: 'Filesystem inspector', includeHidden: true });
        assert.equal(await fileInspector.locator('.inspector-mode-footer button').count(), 1);
        if (logicalWidth > 850) {
            const action = fileInspector.getByRole('button', { name: 'Show in TX16W disk set (60 objects)' });
            const actionBounds = await action.boundingBox();
            const bodyBounds = await fileInspector.locator('.inspector-body').boundingBox();
            const inspectorBounds = await fileInspector.boundingBox();
            assert(actionBounds.y >= bodyBounds.y + bodyBounds.height - 1);
            assert(actionBounds.y + actionBounds.height <= inspectorBounds.y + inspectorBounds.height);
            await page.screenshot({ path: resolve(output, `files-${width}-${scale}.png`) });
            await action.click();
        } else {
            await page.getByRole('button', { name: 'Device', exact: true }).click();
        }
        await page.waitForFunction(() => document.querySelector('[aria-label="Search Waves"]')?.value === 'Wave 2');
        await inspector.getByText('33333', { exact: true }).waitFor({ state: inspectorState });
        await page.getByText('Discovery notices (1)').click();
        await page.getByText('Missing wave companion', { exact: true }).waitFor();
        await page.getByText('Discovery notices (1)').focus();
        await page.keyboard.press('Escape');
        assert.equal(await page.locator('details[open]').count(), 0);
        const bounds = await selector.boundingBox();
        assert(bounds.x >= 0 && bounds.x + bounds.width <= logicalWidth + 1);
        const header = await page.locator('.app-header').boundingBox();
        const content = await page.getByRole('region', { name: 'TX16W native objects' }).boundingBox();
        assert(content.y >= header.y + header.height - 1, 'Document controls must not overlap workspace content');
        assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > window.innerWidth), false);
        await page.screenshot({ path: resolve(output, `native-${width}-${scale}.png`) });
        assert.deepEqual(errors, []);
        results.push({ width, scale, passed: true, geometry });
        await page.close();
    }
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
