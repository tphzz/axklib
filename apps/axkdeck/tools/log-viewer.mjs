import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createServer } from 'vite';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const output = resolve(process.argv[2] ?? '../../../build/logs/log-viewer/00001/browser');
await mkdir(output, { recursive: true });
const server = await createServer({ server: { host: '127.0.0.1', port: 0, strictPort: true } });
let browser;
let port;
const results = [];
const stop = async () => { await browser?.close(); await server.close(); };
const deadline = setTimeout(() => void stop(), 180000);
const onSignal = () => void stop();
process.once('SIGINT', onSignal); process.once('SIGTERM', onSignal);
try {
    await server.listen(); port = server.httpServer.address().port;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    for (const scenario of ['populated', 'empty', 'read-error', 'preference-error', 'scale-error', 'browser', 'module-error']) {
        const page = await browser.newPage({ viewport: { width: 1100, height: 700 } });
        const name = `startup-${scenario}`;
        const errors = [];
        page.on('pageerror', (error) => errors.push(error.message));
        page.setDefaultTimeout(10000);
        try {
            await page.addInitScript({ path: 'tools/log-startup-mock.js' });
            if (scenario === 'module-error') await page.route('**/src/features/diagnostics/bootstrap.ts', (route) => route.abort());
            await page.goto(`http://127.0.0.1:${port}/index.html?view=logs&logs-test=${scenario}`);
            if (scenario === 'module-error') {
                await page.getByText('The Logs window could not be loaded:', { exact: false }).waitFor();
                assert(await page.evaluate(() => !document.documentElement.hasAttribute('data-interface-scale-pending')));
                assert.equal(await page.evaluate(() => window.logStartup.showCalls), 1);
            } else {
                await page.addScriptTag({ path: 'tools/log-startup-checks.js' });
                const result = await page.evaluate(() => window.runLogStartupChecks());
                assert.deepEqual(result.failures, []);
            }
            assert.deepEqual(errors, []);
            results.push({ name, passed: true });
            console.log(`PASS ${name}`);
        } catch (error) {
            results.push({ name, passed: false, error: error.message, errors });
            console.error(`FAIL ${name}: ${error.message}`); process.exitCode = 1;
        } finally {
            if (!page.isClosed()) { await page.screenshot({ path: resolve(output, `${name}.png`) }); await page.close(); }
        }
    }
    for (const width of [390, 800, 1100, 1600]) for (const dpr of [1, 1.5, 2]) {
        const page = await browser.newPage({ viewport: { width, height: 700 }, deviceScaleFactor: dpr });
        const name = `${width}-dpr-${dpr}`;
        const errors = [];
        page.on('pageerror', (error) => errors.push(error.message));
        page.setDefaultTimeout(10000);
        try {
            await page.goto(`http://127.0.0.1:${port}/tools/layout-fixtures/log-viewer.html`);
            await page.getByText('Entry 250:', { exact: false }).waitFor();
            await page.getByRole('switch', { name: 'Word wrap' }).click();
            assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'Viewport overflow');
            const pane = page.getByRole('textbox', { name: 'Log entries' });
            await pane.focus();
            await pane.press('Control+a');
            const selected = await page.evaluate(() => getSelection().toString());
            assert(selected.includes('Entry 250:'), 'Log selection failed');
            assert.equal(await page.getByRole('switch', { name: 'Follow latest' }).getAttribute('aria-checked'), 'false');
            await page.evaluate(() => window.logFixture.append());
            await page.getByRole('button', { name: '1 new entries' }).waitFor();
            assert.equal(await page.evaluate(() => getSelection().toString()), selected, 'New entries disturbed selection');
            await page.getByRole('button', { name: '1 new entries' }).click();
            await page.getByText('Entry 251:', { exact: false }).waitFor();
            await page.getByRole('button', { name: 'Clear view', exact: true }).click();
            await page.getByText('No matching log entries').waitFor();
            await page.evaluate(() => window.logFixture.append(3));
            await page.getByText('Entry 254:', { exact: false }).waitFor();
            assert.equal(await page.locator('.log-record').count(), 3);
            await page.getByRole('combobox', { name: 'Minimum level' }).selectOption('warning');
            await page.waitForFunction(() => document.querySelectorAll('.log-record').length === 1);
            await page.getByRole('button', { name: 'Save logs' }).click();
            await page.getByRole('menuitem', { name: 'Save view...' }).press('Enter');
            await page.waitForFunction(() => window.logFixture.saves.length === 1);
            assert.equal(await page.evaluate(() => window.logFixture.saves[0].since), 251);
            await page.getByRole('button', { name: 'Save logs' }).click();
            await page.getByRole('menuitem', { name: 'Save view...' }).press('ArrowDown');
            await page.getByRole('menuitem', { name: 'Save all logs...' }).press('Enter');
            await page.waitForFunction(() => window.logFixture.saves.length === 2);
            assert.equal(await page.evaluate(() => window.logFixture.saves[1]), null);
            await page.getByRole('button', { name: 'Show retained history' }).click();
            await page.waitForFunction(() => document.querySelectorAll('.log-record').length > 3);
            await page.evaluate(() => window.logFixture.visibility(false));
            const count = await page.evaluate(() => window.logFixture.reads.length);
            await page.waitForTimeout(1100);
            assert.equal(await page.evaluate(() => window.logFixture.reads.length), count, 'Hidden viewer kept polling');
            await page.evaluate(() => window.logFixture.visibility(true));
            await page.waitForFunction((previous) => window.logFixture.reads.length > previous, count);
            const overlapping = await page.locator('.log-toolbar').evaluate((toolbar) => {
                const nodes = [...toolbar.querySelectorAll('input, select, button')].filter((el) => el.getBoundingClientRect().width);
                return nodes.some((a, i) => nodes.slice(i + 1).some((b) => {
                    const x = a.getBoundingClientRect(), y = b.getBoundingClientRect();
                    return Math.min(x.right, y.right) - Math.max(x.left, y.left) > 1 && Math.min(x.bottom, y.bottom) - Math.max(x.top, y.top) > 1;
                }));
            });
            assert(!overlapping, 'Toolbar controls overlap');
            assert.deepEqual(errors, []);
            results.push({ name, passed: true });
            console.log(`PASS ${name}`);
        } catch (error) {
            results.push({ name, passed: false, error: error.message, errors });
            console.error(`FAIL ${name}: ${error.message}`); process.exitCode = 1;
        } finally {
            if (!page.isClosed()) { await page.screenshot({ path: resolve(output, `${name}.png`) }); await page.close(); }
        }
    }
    if (process.env.LOG_VIEWER_WEBKIT === '1') {
        const result = await promisify(execFile)('/usr/bin/python3', [
            'tools/sample-editor-webkit.py', `http://127.0.0.1:${port}`, resolve(output, 'webkit'),
            '--width', '1100', '--height', '700', '--zoom', '1', '--zoom', '1.5',
            '--fixture', '/tools/layout-fixtures/log-viewer.html', '--checks', 'tools/log-viewer-checks.js',
            '--checks-function', 'runLogViewerChecks',
        ], { timeout: 120000, env: process.env });
        console.log(result.stdout);
        await writeFile(resolve(output, 'webkit.log'), result.stdout + result.stderr);
        const startup = await promisify(execFile)('/usr/bin/python3', [
            'tools/sample-editor-webkit.py', `http://127.0.0.1:${port}`, resolve(output, 'webkit-startup'),
            '--width', '1100', '--height', '700', '--zoom', '1', '--zoom', '1.5',
            '--fixture', '/index.html?view=logs', '--init-script', 'tools/log-startup-mock.js',
            '--checks', 'tools/log-startup-checks.js', '--checks-function', 'runLogStartupChecks',
        ], { timeout: 120000, env: process.env });
        console.log(startup.stdout);
        await writeFile(resolve(output, 'webkit-startup.log'), startup.stdout + startup.stderr);
    }
} finally {
    clearTimeout(deadline); process.removeListener('SIGINT', onSignal); process.removeListener('SIGTERM', onSignal);
    await stop();
    await writeFile(resolve(output, 'results.json'), JSON.stringify({ port, serverStopped: true, results }, null, 2) + '\n');
}
