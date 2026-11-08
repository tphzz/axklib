import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { spawn } from 'node:child_process';
import { createServer as createSocketServer } from 'node:net';
import { createServer } from 'vite';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const checks = await readFile(new URL('./audio-import-layout-checks.js', import.meta.url), 'utf8');
const server = await createServer({ server: { host: '127.0.0.1', port: 0, strictPort: false } });
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
try {
    await server.listen();
    port = server.httpServer.address().port;
    const base = `http://127.0.0.1:${port}`;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    const cases = [];
    for (const width of [1320, 880, 640, 390]) for (const zoom of [1, 1.25, 1.5]) cases.push({ width, zoom, scenario: 'normal' });
    for (const scenario of ['mono', 'loop', 'long', 'conflict', 'invalid', 'empty', 'inspecting', 'busy', 'warnings', 'refresh-failed', 'preview-failure', 'many']) cases.push({ width: scenario === 'long' ? 880 : 1320, zoom: 1, scenario });
    if (process.env.AUDIO_IMPORT_SCENARIO) cases.splice(0, cases.length, ...cases.filter(entry => entry.scenario === process.env.AUDIO_IMPORT_SCENARIO));
    if (process.env.AUDIO_IMPORT_QUICK === '1') cases.splice(1);
    for (const entry of cases) {
        if (interrupted) break;
        const name = `${entry.scenario}-${entry.width}-${entry.zoom}`;
        const page = await browser.newPage({ viewport: { width: entry.width, height: 900 } });
        page.setDefaultTimeout(10000);
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        try {
            await page.goto(`${base}/tools/layout-fixtures/audio-import-layout.html?scenario=${entry.scenario}`);
            await page.getByRole('dialog', { name: 'Import audio' }).waitFor();
            if (!['empty', 'inspecting', 'invalid'].includes(entry.scenario)) await page.locator('.audio-import-card.editable').first().waitFor();
            if (entry.scenario === 'conflict') {
                await page.getByLabel(/^Sample name for/).first().fill('DNBS_drum_loop_1');
                await page.getByRole('alert').filter({ hasText: 'Sample name already exists' }).waitFor();
            }
            if (entry.scenario === 'long') {
                await page.getByLabel(/^Wave data \(mono\/left\)/).fill('MMMMMMMMMMMMMMMM');
                await page.getByLabel(/^Wave data \(right\)/).fill('WWWWWWWWWWWWWWWW');
                const fit = await page.getByLabel(/^Wave data \(right\)/).evaluate(input => {
                    const css = getComputedStyle(input), canvas = document.createElement('canvas');
                    const context = canvas.getContext('2d'); context.font = css.font;
                    return { text: context.measureText(input.value).width, available: input.clientWidth - parseFloat(css.paddingLeft) - parseFloat(css.paddingRight) };
                });
                assert.ok(fit.text <= fit.available - 1, `Maximum right-wave name/caret must fit: ${JSON.stringify(fit)}`);
            }
            await page.evaluate(zoom => { document.documentElement.style.zoom = String(zoom); }, entry.zoom);
            await page.addScriptTag({ content: checks });
            let result = await page.evaluate(() => window.runAudioImportLayoutChecks());
            await page.screenshot({ path: resolve(output, `${name}.png`), animations: 'disabled' });
            assert.deepEqual(result.failures, []);
            if (entry.scenario === 'many') {
                const firstSample = page.getByLabel(/^Sample name for/).first();
                await page.getByRole('combobox', { name: /^Playback mode for/ }).last().focus();
                let reachedFirst = false;
                for (let step = 0; step < 300; step++) {
                    await page.keyboard.press('Shift+Tab');
                    const focus = await page.evaluate(() => {
                        const node = document.activeElement;
                        if (!node.closest('.audio-import-card')) return null;
                        const bounds = node.getBoundingClientRect();
                        const scroller = document.querySelector('.audio-import-rows').getBoundingClientRect();
                        const heading = document.querySelector('.audio-import-column-headings').getBoundingClientRect();
                        return { label: node.getAttribute('aria-label'), top: bounds.top, bottom: bounds.bottom, clearTop: Math.max(scroller.top, heading.bottom), clearBottom: scroller.bottom };
                    });
                    if (focus) assert.ok(focus.top >= focus.clearTop - 1 && focus.bottom <= focus.clearBottom + 1, `Keyboard focus obscured: ${focus.label}`);
                    reachedFirst = await firstSample.evaluate(node => node === document.activeElement);
                    if (reachedFirst) break;
                }
                assert.ok(reachedFirst, 'Reverse navigation must reach the first Sample');
                await page.screenshot({ path: resolve(output, 'reverse-keyboard.png'), animations: 'disabled' });
                result = { ...result, reverseKeyboardPassed: true };
            }
            if (entry.scenario === 'normal' && entry.width === 1320 && entry.zoom === 1) {
                assert.equal(result.wide, true, 'Primary desktop reference must use the shared wide grid');
                const format = page.getByRole('group', { name: 'Sample format', exact: true });
                await format.getByRole('button', { name: 'A3K', exact: true }).focus();
                await page.keyboard.press('Space');
                assert.equal(await format.getByRole('button', { name: 'A3K', exact: true }).getAttribute('aria-pressed'), 'true');
                await page.getByRole('button', { name: 'Clear volume', exact: true }).click();
                assert.equal(await page.getByRole('combobox', { name: 'Destination volume', exact: true }).inputValue(), '');
                await page.getByRole('option', { name: 'A3K_FLOPPY_03', exact: true }).click();
                const first = page.locator('.audio-import-card').first();
                await first.getByRole('combobox', { name: /^Playback mode/ }).click();
                await page.keyboard.press('Escape');
                assert.equal(await page.getByRole('dialog', { name: 'Import audio' }).count(), 1);
                await page.getByRole('combobox', { name: 'Import mode', exact: true }).click();
                await page.keyboard.press('End'); await page.keyboard.press('Enter');
                await page.getByRole('textbox', { name: 'Sample Bank name', exact: true }).fill('Drum loops');
                result = await page.evaluate(() => window.runAudioImportLayoutChecks());
                assert.deepEqual(result.failures, []);
                await page.screenshot({ path: resolve(output, 'bank-name.png'), animations: 'disabled' });
                await page.emulateMedia({ forcedColors: 'active' });
                const selected = format.locator('[aria-pressed=true]');
                await page.keyboard.press('Tab'); await selected.focus();
                assert.notEqual(await selected.evaluate(node => getComputedStyle(node).outlineStyle), 'none');
                await page.screenshot({ path: resolve(output, 'forced-colors.png'), animations: 'disabled' });
                await page.emulateMedia({ forcedColors: 'none' });
            }
            if (entry.scenario === 'loop') assert.equal(await page.getByLabel(/^Loop length for/).count(), 3);
            if (entry.scenario === 'mono') assert.equal(await page.getByLabel(/^Wave data \(right\)/).count(), 0);
            if (entry.scenario === 'conflict') {
                assert.equal(await page.getByRole('button', { name: 'Import', exact: true }).isDisabled(), true);
                const name = page.getByLabel(/^Sample name for/).first();
                assert.equal(await name.getAttribute('aria-invalid'), 'true');
                assert.equal(await name.getAttribute('aria-describedby'), await page.getByRole('alert').getAttribute('id'));
            }
            if (entry.scenario === 'busy') {
                await page.getByRole('button', { name: 'Import', exact: true }).click();
                assert.equal(await page.getByRole('button', { name: 'Cancel', exact: true }).isDisabled(), true);
                assert.equal(await page.locator('.audio-import-card input:not(:disabled)').count(), 0);
                await page.screenshot({ path: resolve(output, 'busy.png'), animations: 'disabled' });
                await page.evaluate(() => window.audioImportLayoutFixture.finishCommit(false));
            }
            if (entry.scenario === 'warnings' || entry.scenario === 'refresh-failed') {
                await page.getByRole('button', { name: 'Import', exact: true }).click();
                await page.getByRole('button', { name: entry.scenario === 'warnings' ? 'Done' : 'Refresh', exact: true }).waitFor();
                assert.equal(await page.getByRole('button', { name: 'Import', exact: true }).count(), 0);
                await page.screenshot({ path: resolve(output, `${entry.scenario}-result.png`), animations: 'disabled' });
            }
            if (entry.scenario === 'preview-failure') {
                await page.getByRole('button', { name: /^Play / }).first().click();
                await page.getByText(/^Preview unavailable:/).waitFor();
                await page.screenshot({ path: resolve(output, 'preview-failure-result.png'), animations: 'disabled' });
            }
            assert.deepEqual(errors, []);
            results.push({ ...entry, pass: true, result });
            console.log(`PASS ${name}`);
        } catch (error) {
            process.exitCode = 1;
            results.push({ ...entry, pass: false, error: error.message });
            await page.screenshot({ path: resolve(output, `${name}-failed.png`) }).catch(() => {});
            console.error(`FAIL ${name}: ${error.message}`);
        } finally { await page.close(); }
    }
    if (process.env.AUDIO_IMPORT_WEBKIT === '1' && !process.exitCode) {
        child = spawn('/usr/bin/python3', ['tools/sample-editor-webkit.py', base, resolve(output, 'webkit'), '--fixture', '/tools/layout-fixtures/audio-import-layout.html?scenario=normal&native-cases=1', '--checks', 'tools/audio-import-layout-checks.js', '--checks-function', 'runAudioImportLayoutChecks', '--zoom', '1', '--zoom', '1.25', '--zoom', '1.5', '--width', '1320', '--height', '900', '--focused'], { stdio: 'inherit', detached: true });
        childExit = new Promise((done, reject) => { child.once('error', reject); child.once('close', code => code === 0 ? done() : reject(new Error(`WebKitGTK exited${code}`))); });
        await childExit;
        child = undefined;
    }
} finally {
    clearTimeout(deadline);
    await stop();
    process.removeListener('SIGTERM', interrupt); process.removeListener('SIGINT', interrupt);
    if (port) await new Promise((done, reject) => {
        const socket = createSocketServer(); socket.once('error', reject);
        socket.listen(port, '127.0.0.1', () => socket.close(error => error ? reject(error) : done()));
    });
    await writeFile(resolve(output, 'results.json'), JSON.stringify({ serverStopped: true, browserStopped: true, portReleased: !!port, results }, null, 2) + '\n');
}
