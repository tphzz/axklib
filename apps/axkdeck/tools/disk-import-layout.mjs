import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { spawn } from 'node:child_process';
import { createServer as socketServer } from 'node:net';
import { createServer } from 'vite';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const checks = await readFile(new URL('./disk-import-layout-checks.js', import.meta.url), 'utf8');
const freePort = await new Promise((done, reject) => { const socket = socketServer(); socket.once('error', reject); socket.listen(0, '127.0.0.1', () => { const port = socket.address().port; socket.close(error => error ? reject(error) : done(port)); }); });
const server = await createServer({ server: { host: '127.0.0.1', port: freePort, strictPort: true } });
let browser, child, childExit, port, stopping, interrupted = false;
const results = [];
function stop() {
    return stopping ??= (async () => {
        if (child?.pid) {
            try { process.kill(-child.pid, 'SIGTERM'); } catch (error) { if (error.code !== 'ESRCH') throw error; }
            const force = setTimeout(() => { try { process.kill(-child.pid, 'SIGKILL'); } catch {} }, 3000);
            try { await childExit; } finally { clearTimeout(force); }
        }
        try { await browser?.close(); } finally { await server.close(); }
    })();
}
const interrupt = () => { interrupted = true; process.exitCode = 1; void stop(); };
process.once('SIGTERM', interrupt); process.once('SIGINT', interrupt);
const deadline = setTimeout(interrupt, 300000);
const cases = [];
for (const kind of ['floppy', 'tx16w']) {
    for (const width of [1152, 1040, 732, 610, 390]) for (const zoom of [1, 1.25, 1.5]) cases.push({ kind, width, height: 820, zoom, scenario: 'normal' });
    for (const scenario of kind === 'floppy' ? ['recovery', 'capacity', 'empty', 'planning', 'importing', 'unconfirmed', 'refresh-failed', 'warnings', 'a3k', 'conflicts'] : ['empty', 'planning', 'importing', 'unconfirmed', 'refresh-failed', 'warnings', 'missing', 'strict', 'native', 'expired']) cases.push({ kind, width: 1152, height: 820, zoom: 1, scenario });
    for (const scenario of ['warnings', 'refresh-failed', 'unconfirmed']) cases.push({ kind, width: 732, height: 680, zoom: 1, scenario });
}
cases.push({ kind: 'tx16w', width: 732, height: 680, zoom: 1.5, scenario: 'native-long' });
for (const scenario of ['companion-found', 'searching', 'search-error', 'mixed-losses', 'all-losses', 'long-chips']) {
    for (const width of [1152, 732]) cases.push({ kind: 'tx16w', width, height: 820, zoom: 1, scenario });
}
cases.push({ kind: 'tx16w', width: 390, height: 820, zoom: 1.5, scenario: 'long-chips' });
if (process.env.DISK_IMPORT_QUICK === '1') cases.splice(0, cases.length, ...cases.filter(c => [1152, 732].includes(c.width) && [1, 1.5].includes(c.zoom) && c.scenario === 'normal'));
if (process.env.DISK_IMPORT_SCENARIOS) cases.splice(0, cases.length, ...cases.filter(c => process.env.DISK_IMPORT_SCENARIOS.split(',').includes(c.scenario)));
try {
    await server.listen(); port = server.httpServer.address().port;
    const base = `http://127.0.0.1:${port}`;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    for (const entry of cases) {
        if (interrupted) break;
        const page = await browser.newPage({ viewport: { width: entry.width, height: entry.height } });
        page.setDefaultTimeout(10000);
        const name = `${entry.kind}-${entry.scenario}-${entry.width}-${entry.zoom}`;
        const errors = []; page.on('pageerror', error => errors.push(error.message));
        try {
            const fixture = entry.kind === 'floppy' ? 'floppy-import' : 'conversion';
            await page.goto(`${base}/tools/layout-fixtures/${fixture}.html?layout&direct&scenario=${entry.scenario}&${entry.scenario}`);
            const dialog = page.getByRole('dialog'); await dialog.waitFor();
            await page.evaluate(zoom => { document.documentElement.style.zoom = String(zoom); }, entry.zoom);
            await page.addScriptTag({ content: checks });
            await page.waitForTimeout(100);
            const geometry = await page.evaluate(() => window.runDiskImportLayoutChecks());
            await page.screenshot({ path: resolve(output, `${name}.png`), animations: 'disabled' });
            assert.deepEqual(geometry.failures, []);
            const footer = dialog.locator('.dialog-footer');
            const footerBounds = await footer.boundingBox();
            const contents = dialog.locator('[data-pane="contents"] .import-pane-scroll');
            if (entry.scenario === 'normal') {
                await contents.focus(); await page.keyboard.press('PageDown');
                await page.waitForFunction(() => document.querySelector('[data-pane="contents"] .import-pane-scroll').scrollTop > 0);
                await page.waitForTimeout(300);
                const scrollTop = await contents.evaluate(node => node.scrollTop);
                assert(scrollTop > 0, 'Contents must scroll by keyboard');
                if (geometry.narrow) {
                    await dialog.getByRole('tab', { name: 'Contents', exact: true }).focus();
                    await page.keyboard.press('ArrowRight');
                    await dialog.getByRole('tabpanel', { name: /^Review/ }).waitFor();
                    assert.equal(await contents.evaluate(node => node.closest('[data-pane]').inert), true);
                    await page.screenshot({ path: resolve(output, `${name}-review.png`) });
                    await page.keyboard.press('ArrowLeft');
                    assert.equal(await contents.evaluate(node => node.scrollTop), scrollTop, 'Pane switch must preserve scroll position');
                }
                if (entry.kind === 'tx16w') {
                    await dialog.getByRole('button', { name: 'Next objects page' }).click();
                    await contents.getByText('Concert-Piano-100', { exact: true }).waitFor();
                    assert.equal(await contents.evaluate(node => node.scrollTop), 0, 'New objects page must start at top');
                    if (geometry.narrow) await dialog.getByRole('tab', { name: /^Review/ }).click();
                    const ack = dialog.locator('[data-pane="review"] input[type=checkbox]');
                    const allLosses = dialog.getByRole('checkbox', { name: 'Accept all losses', exact: true });
                    await allLosses.focus(); await page.keyboard.press('Space');
                    assert(await dialog.getByRole('button', { name: 'Import', exact: true }).isDisabled(), 'Bulk losses must leave uncertainty consent unchanged');
                    const uncertainty = dialog.getByRole('checkbox', { name: /Uncertainty.*Destination controls/ });
                    await uncertainty.focus(); await page.keyboard.press('Space');
                    assert(await dialog.getByRole('button', { name: 'Import', exact: true }).isEnabled());
                    await dialog.getByRole('button', { name: 'Strict', exact: true }).click();
                    assert(await dialog.getByRole('button', { name: 'Import', exact: true }).isDisabled());
                    assert(await ack.first().isDisabled());
                    const help = dialog.getByRole('button', { name: 'Destination availability', exact: true });
                    await help.focus();
                    await page.getByRole('tooltip').filter({ hasText: 'TX16W conversion requires an existing volume' }).waitFor();
                    await page.keyboard.press('Escape');
                    assert.equal(await page.getByRole('dialog').count(), 1, 'Tooltip Escape must not close dialog');
                }
                assert.deepEqual(await footer.boundingBox(), footerBounds, 'Footer must remain stationary');
                if (entry.width === 1152 && entry.zoom === 1) {
                    await page.emulateMedia({ forcedColors: 'active' });
                    const target = dialog.getByRole('group', { name: 'Sampler load target', exact: true }).locator('[aria-pressed="true"]');
                    await page.keyboard.press('Tab'); await target.focus();
                    assert.notEqual(await target.evaluate(node => getComputedStyle(node).outlineStyle), 'none');
                    await page.screenshot({ path: resolve(output, `${entry.kind}-forced-colors.png`) });
                    await page.emulateMedia({ forcedColors: 'none' });
                    const fileName = dialog.locator('.member-name button').first();
                    await fileName.focus();
                    const expected = await fileName.textContent();
                    await page.getByRole('tooltip').filter({ hasText: expected }).waitFor();
                    await page.keyboard.press('Escape');
                }
                if (entry.kind === 'tx16w' && entry.width === 1152 && entry.zoom === 1) {
                    await dialog.getByRole('button', { name: 'Best', exact: true }).click();
                    await dialog.locator('[data-pane="review"] input[type=checkbox]').first().focus();
                    await page.setViewportSize({ width: 732, height: 820 });
                    await page.waitForFunction(() => document.activeElement?.getAttribute('data-tab') === 'contents');
                }
                if (entry.width === 732 && entry.zoom === 1) {
                    await dialog.getByRole('tab', { name: /^Review/ }).click();
                    const review = dialog.locator('[data-pane="review"] .import-pane-scroll');
                    await review.evaluate(node => { node.scrollTop = node.scrollHeight; });
                    await dialog.getByRole('tab', { name: 'Contents', exact: true }).click();
                    await page.evaluate(() => window.diskImportLayout.showWarnings());
                    await dialog.getByRole('button', { name: 'Done', exact: true }).waitFor();
                    assert.equal(await dialog.getByRole('tab', { name: /^Review/ }).getAttribute('aria-selected'), 'true');
                    assert.equal(await review.evaluate(node => node.scrollTop), 0);
                    const warning = dialog.getByText('Import saved; journal cleanup is pending. Retain recovery data.', { exact: true });
                    const warningBounds = await warning.boundingBox(), reviewBounds = await review.boundingBox();
                    assert(warningBounds.y >= reviewBounds.y && warningBounds.y + warningBounds.height <= reviewBounds.y + reviewBounds.height, 'Retained warning must be visible without scrolling');
                    await page.screenshot({ path: resolve(output, `${entry.kind}-warning-transition.png`) });
                }
            }
            if (['empty', 'planning', 'importing', 'capacity', 'missing', 'strict', 'conflicts', 'recovery', 'expired', 'searching', 'search-error'].includes(entry.scenario)) assert(await dialog.getByRole('button', { name: 'Import', exact: true }).isDisabled());
            if (entry.scenario === 'searching') assert(await dialog.getByRole('button', { name: 'Cancel', exact: true }).isEnabled());
            if (entry.scenario === 'companion-found') {
                assert.equal(await dialog.locator('.source-member').count(), 2);
                assert.equal(await dialog.getByText('Auto-added', { exact: true }).count(), 1);
            }
            if (entry.scenario === 'search-error') {
                await dialog.getByRole('button', { name: 'Retry', exact: true }).click();
                assert.equal(await dialog.getByRole('button', { name: 'Retry', exact: true }).count(), 0);
            }
            if (['mixed-losses', 'all-losses'].includes(entry.scenario)) {
                if (geometry.narrow) {
                    await dialog.getByRole('tab', { name: /^Review/ }).focus();
                    await page.keyboard.press('Enter');
                    await page.screenshot({ path: resolve(output, `${name}-review.png`) });
                }
                const all = dialog.getByRole('checkbox', { name: 'Accept all losses', exact: true });
                assert.equal(await all.evaluate(node => node.indeterminate), entry.scenario === 'mixed-losses');
                if (entry.scenario === 'mixed-losses') await all.click();
                assert(await all.isChecked());
                await dialog.getByRole('button', { name: 'Next mapping details page' }).click();
                const loss = dialog.getByRole('checkbox', { name: /Loss.*Wave Data 100 resampled/ });
                assert(await loss.isChecked(), 'Bulk loss acceptance must include subsequent pages');
                assert(await dialog.getByRole('button', { name: 'Import', exact: true }).isDisabled(), 'Uncertainty must remain individually unaccepted');
                await all.click();
                assert.equal(await loss.isChecked(), false);
            }
            if (entry.scenario === 'long-chips') {
                const fileName = dialog.locator('.member-name button').first();
                await fileName.focus();
                await page.getByRole('tooltip').filter({ hasText: await fileName.textContent() }).waitFor();
                await page.screenshot({ path: resolve(output, `${name}-name-help.png`) });
                await page.keyboard.press('Escape');
                assert.equal(await page.getByRole('dialog').count(), 1);
            }
            if (['unconfirmed', 'refresh-failed', 'warnings'].includes(entry.scenario)) {
                assert.equal(await dialog.getByRole('button', { name: 'Import', exact: true }).count(), 0);
                const action = entry.scenario === 'unconfirmed' ? 'Check status' : entry.scenario === 'refresh-failed' ? 'Refresh' : 'Done';
                assert.equal(await dialog.getByRole('button', { name: action, exact: true }).count(), 1);
                if (geometry.narrow) assert.equal(await dialog.getByRole('tab', { name: /^Review/ }).getAttribute('aria-selected'), 'true');
                if (entry.scenario === 'warnings') await dialog.getByText('Import saved; journal cleanup is pending. Retain recovery data.', { exact: true }).waitFor();
            }
            if (entry.scenario === 'native') assert.equal(await dialog.getByRole('region', { name: 'Source disks' }).count(), 0);
            assert.deepEqual(errors, []);
            results.push({ ...entry, pass: true, geometry }); console.log(`PASS ${name}`);
        } catch (error) {
            process.exitCode = 1; results.push({ ...entry, pass: false, error: error.message });
            console.error(`FAIL ${name}: ${error.message}`);
            await page.screenshot({ path: resolve(output, `${name}-failed.png`) }).catch(() => {});
        } finally { await page.close(); }
    }
    if (process.env.DISK_IMPORT_WEBKIT === '1' && !process.exitCode) {
        for (const fixture of ['floppy-import', 'conversion']) {
            child = spawn('/usr/bin/python3', ['tools/sample-editor-webkit.py', base, resolve(output, `webkit-${fixture}`), '--fixture', `/tools/layout-fixtures/${fixture}.html?layout&direct`, '--checks', 'tools/disk-import-layout-checks.js', '--checks-function', 'runDiskImportLayoutChecks', '--zoom', '1', '--zoom', '1.25', '--zoom', '1.5', '--width', '1152', '--height', '820', '--focused'], { stdio: 'inherit', detached: true });
            childExit = new Promise((done, reject) => { child.once('error', reject); child.once('close', code => code === 0 ? done() : reject(new Error(`WebKitGTK exited ${code}`))); });
            await childExit; child = undefined;
        }
    }
} finally {
    clearTimeout(deadline); await stop();
    process.removeListener('SIGTERM', interrupt); process.removeListener('SIGINT', interrupt);
    if (port) await new Promise((done, reject) => { const socket = socketServer(); socket.once('error', reject); socket.listen(port, '127.0.0.1', () => socket.close(error => error ? reject(error) : done())); });
    await writeFile(resolve(output, 'results.json'), JSON.stringify({ serverStopped: true, browserStopped: true, portReleased: !!port, results }, null, 2) + '\n');
}
