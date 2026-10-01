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
const deadline = setTimeout(signal, 120000);
try {
    await server.listen();
    port = server.httpServer.address().port;
    const url = `http://127.0.0.1:${port}/tools/layout-fixtures/import-directory-history.html`;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    for (const width of [1366, 640, 390]) {
        const page = await browser.newPage({ viewport: { width, height: 900 } });
        const errors = []; page.on('pageerror', error => errors.push(error.message));
        const entry = { width, errors }; results.push(entry);
        try {
            await page.goto(url);
            const open = async (target, type) => {
                await page.getByRole('button', { name: target, exact: true }).click({ button: 'right' });
                await page.getByRole('menuitem', { name: 'Import', exact: true }).focus();
                await page.keyboard.press('ArrowRight');
                await page.getByRole('menuitem', { name: `Import ${type}`, exact: false }).click();
                return page.getByRole('dialog', { name: type === 'floppy' ? 'Choose floppy source' : 'Choose axklib packages' });
            };
            const folder = (dialog, name) => dialog.getByRole('option', { name: new RegExp(`^${name}(?:\\s|$)`) });
            const cancel = async dialog => { await dialog.getByRole('button', { name: 'Cancel', exact: true }).click(); await dialog.waitFor({ state: 'detached' }); };
            let dialog = await open('Volume', 'floppy');
            await folder(dialog, 'Test sources').click();
            await folder(dialog, 'Disks').click();
            await folder(dialog, 'Kit').click();
            await folder(dialog, 'disk1.img').click();
            await page.screenshot({ path: resolve(output, `${width}-floppy-selected.png`) });
            await dialog.getByRole('button', { name: 'Select 1 file', exact: true }).click();
            const parent = page.getByRole('dialog', { name: 'Import floppy', exact: true });
            await parent.getByRole('button', { name: 'Add disks...' }).click();
            dialog = page.getByRole('dialog', { name: 'Choose floppy source' });
            await folder(dialog, 'disk1.img').waitFor();
            assert.match(await dialog.locator('[aria-current="location"]').textContent(), /Kit/);
            await cancel(dialog);
            assert.match(await parent.getByLabel('Source disks').textContent(), /disk1.img/);
            await parent.getByRole('button', { name: 'Cancel', exact: true }).click();
            await parent.waitFor({ state: 'detached' });
            dialog = await open('Volume', 'packages');
            await folder(dialog, 'Test sources').click();
            await folder(dialog, 'Packages').click();
            await cancel(dialog);
            dialog = await open('Partition', 'floppy');
            await folder(dialog, 'disk1.img').waitFor();
            assert.match(await dialog.locator('[aria-current="location"]').textContent(), /Kit/);
            await page.screenshot({ path: resolve(output, `${width}-partition-restored.png`) });
            await cancel(dialog);
            await parent.waitFor({ state: 'detached' });
            dialog = await open('Partition', 'packages');
            await dialog.locator('[aria-current="location"]').filter({ hasText: 'Packages' }).waitFor();
            await cancel(dialog);
            await page.getByRole('button', { name: 'Remove disk folder' }).click();
            dialog = await open('Volume', 'floppy');
            await dialog.getByText('Source folder no longer exists', { exact: true }).waitFor();
            await folder(dialog, 'Test sources').waitFor();
            await cancel(dialog);
            await parent.waitFor({ state: 'detached' });
            await page.getByRole('button', { name: 'Restore disk folder' }).click();
            dialog = await open('Partition', 'floppy');
            await folder(dialog, 'Test sources').waitFor();
            await cancel(dialog);
            await page.reload();
            dialog = await open('Partition', 'floppy');
            await folder(dialog, 'Test sources').waitFor();
            await page.screenshot({ path: resolve(output, `${width}-fresh-session.png`) });
            await cancel(dialog);
            assert.deepEqual(errors, []);
            entry.passed = true;
            console.log(`${width}: volume/partition history, companion selection, package isolation, missing folder and session reset passed`);
        } catch (error) { entry.error = error.message; throw error; }
        finally { await page.screenshot({ path: resolve(output, `${width}-final.png`) }); await page.close(); }
    }
} finally {
    clearTimeout(deadline); process.removeListener('SIGTERM', signal); process.removeListener('SIGINT', signal);
    await stop();
    await writeFile(resolve(output, 'results.json'), JSON.stringify({ port, serverStopped: true, results }, null, 2) + '\n');
}
