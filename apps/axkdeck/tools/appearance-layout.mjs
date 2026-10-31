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
const server = await createServer({ server: { host: '127.0.0.1', port: 0, strictPort: false } });
const checks = await readFile(new URL('./appearance-checks.js', import.meta.url), 'utf8');
let browser, child, childExit, port, stopping;
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
const interrupted = () => { process.exitCode = 1; void stop(); };
process.once('SIGTERM', interrupted); process.once('SIGINT', interrupted);
const deadline = setTimeout(interrupted, 300000);
try {
    await server.listen();
    port = server.httpServer.address().port;
    const base = `http://127.0.0.1:${port}`;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    for (const [width, zoom] of [[1440, 1], [1440, 1.25], [1440, 1.5], [800, 1]]) {
        for (const stage of ['workspace', 'starting', 'unavailable', 'tx16w-idle', 'tx16w-busy']) {
            const name = `${stage}-${width}-${zoom}`;
            const page = await browser.newPage({ viewport: { width, height: 1000 } });
            page.setDefaultTimeout(10000);
            try {
                await page.goto(`${base}/tools/layout-fixtures/appearance.html?stage=${stage}${width < 1000 ? '&narrow=1' : ''}`);
                await page.waitForSelector('.brand, .startup-brand');
                await page.evaluate(value => { document.documentElement.style.zoom = String(value); }, zoom);
                await page.addScriptTag({ content: checks });
                const result = await page.evaluate(() => window.runAppearanceChecks());
                assert.deepEqual(result.failures, []);
                if (stage.startsWith('tx16w-')) {
                    const label = page.locator('.tx16w-import-dialog label.secondary-button');
                    await label.hover({ force: true }); await page.waitForTimeout(200);
                    assert.equal(await label.evaluate(node => getComputedStyle(node).backgroundColor), stage === 'tx16w-busy' ? 'rgb(11, 13, 15)' : 'rgb(27, 30, 32)');
                }
                if (stage === 'workspace') {
                    const primary = page.locator('[data-action-state="Hover"] .primary-button');
                    const secondary = page.locator('[data-action-state="Hover"] .secondary-button');
                    await primary.hover(); await page.waitForTimeout(200);
                    const hover = await primary.evaluate(node => ({ image: getComputedStyle(node).backgroundImage, color: getComputedStyle(node).color, filter: getComputedStyle(node).filter }));
                    assert.match(hover.image, /rgb\(184, 92, 26\)/);
                    assert.match(hover.image, /rgb\(163, 78, 22\)/);
                    assert.equal(hover.color, 'rgb(255, 255, 255)'); assert.equal(hover.filter, 'none');
                    await secondary.hover(); await page.waitForTimeout(200);
                    assert.equal(await secondary.evaluate(node => getComputedStyle(node).backgroundColor), 'rgb(27, 30, 32)');
                    const disabled = page.locator('[data-action-state="Disabled"] .secondary-button');
                    await disabled.hover({ force: true }); await page.waitForTimeout(200);
                    assert.equal(await disabled.evaluate(node => getComputedStyle(node).backgroundColor), 'rgb(11, 13, 15)');
                    const focus = page.locator('[data-action-state="Focus"] .primary-button');
                    await page.keyboard.press('Tab'); await focus.focus();
                    await page.waitForTimeout(200);
                    assert.equal(await focus.evaluate(node => getComputedStyle(node).outlineColor), 'rgb(104, 151, 187)');
                    assert.notEqual(await focus.evaluate(node => getComputedStyle(node).outlineStyle), 'none');
                    const brand = page.getByRole('button', { name: 'About axkdeck', exact: true });
                    for (const key of ['Enter', 'Space']) {
                        await brand.focus(); await page.keyboard.press(key);
                        await page.getByRole('dialog', { name: 'About axkdeck' }).waitFor();
                        await page.keyboard.press('Escape');
                        await page.waitForFunction(() => document.activeElement?.matches('.brand'));
                    }
                    if (width === 1440 && zoom === 1) {
                        await page.mouse.move(0, 0);
                        await page.evaluate(() => document.documentElement.style.setProperty('--color-primary-start', '#ffffff'));
                        const broken = await page.evaluate(() => window.runAppearanceChecks());
                        assert.ok(broken.failures.some(value => value.includes('contrast')), 'Bright-stop regression must fail despite the dark ancestor');
                        await page.evaluate(() => document.documentElement.style.removeProperty('--color-primary-start'));
                    }
                    await page.emulateMedia({ forcedColors: 'active' });
                    await focus.focus();
                    assert.notEqual(await focus.evaluate(node => getComputedStyle(node).outlineStyle), 'none');
                    for (const name of ['Device', 'Files']) {
                        await page.getByRole('button', { name, exact: true }).click();
                        await focus.focus(); await page.waitForTimeout(200);
                        const selection = await page.locator('[aria-label="Workspace mode"]').evaluate(group => {
                            const selected = group.querySelector('[aria-pressed="true"]');
                            const other = group.querySelector('[aria-pressed="false"]');
                            const value = node => { const cue = getComputedStyle(node, '::after'); return { color: getComputedStyle(node).color, background: getComputedStyle(node).backgroundColor, selectionCue: !['none', 'normal'].includes(cue.content) && parseFloat(cue.borderTopWidth) > 0 }; };
                            return { selected: value(selected), other: value(other) };
                        });
                        assert.notDeepEqual(selection.selected, selection.other, `${name}: selection cue must survive blur in forced colors`);
                        assert.equal(selection.selected.selectionCue, true, `${name}: selected mode retains an independent geometric cue`);
                        const selected = page.getByRole('button', { name, exact: true });
                        await page.keyboard.press('Tab'); await selected.focus(); await page.waitForTimeout(200);
                        assert.ok(await selected.evaluate(node => { const cue = getComputedStyle(node, '::after'); return node.matches(':focus-visible') && parseFloat(getComputedStyle(node).outlineOffset) > 0 && !['none', 'normal'].includes(cue.content) && parseFloat(cue.borderTopWidth) > 0; }), `${name}: selection and focus cues coexist`);
                    }
                    for (const label of ['Blue access choice', 'Editor value choice']) {
                        const selection = await page.getByRole('group', { name: label, exact: true }).evaluate(group => {
                            const value = state => { const node = group.querySelector(`[aria-pressed="${state}"]`); const css = getComputedStyle(node), cue = getComputedStyle(node, '::after'); return { color: css.color, background: css.backgroundColor, selectionCue: !['none', 'normal'].includes(cue.content) && parseFloat(cue.borderTopWidth) > 0 }; };
                            return { selected: value(true), other: value(false) };
                        });
                        assert.notDeepEqual(selection.selected, selection.other, `${label}: selected value must survive forced colors`);
                        assert.equal(selection.selected.selectionCue, true, `${label}: selected value retains an independent geometric cue`);
                        const selected = page.getByRole('group', { name: label, exact: true }).locator('[aria-pressed="true"]');
                        await page.keyboard.press('Tab'); await selected.focus(); await page.waitForTimeout(200);
                        assert.ok(await selected.evaluate(node => { const cue = getComputedStyle(node, '::after'); return node.matches(':focus-visible') && parseFloat(getComputedStyle(node).outlineOffset) > 0 && !['none', 'normal'].includes(cue.content) && parseFloat(cue.borderTopWidth) > 0; }), `${label}: selection and focus cues coexist`);
                        await page.screenshot({ path: resolve(output, `forced-focus-${label.startsWith('Blue') ? 'blue' : 'editor'}-${name}.png`) });
                        const clippingOwner = await selected.evaluate(node => {
                            const css = getComputedStyle(node), bounds = node.getBoundingClientRect();
                            const zoom = parseFloat(getComputedStyle(document.documentElement).zoom) || 1;
                            const extent = (parseFloat(css.outlineWidth) + parseFloat(css.outlineOffset)) * zoom;
                            for (let parent = node.parentElement; parent; parent = parent.parentElement) {
                                const value = getComputedStyle(parent), box = parent.getBoundingClientRect();
                                const clippedX = value.overflowX !== 'visible' && (bounds.left - extent < box.left - 1 || bounds.right + extent > box.right + 1);
                                const clippedY = value.overflowY !== 'visible' && (bounds.top - extent < box.top - 1 || bounds.bottom + extent > box.bottom + 1);
                                if (clippedX || clippedY) return parent.className;
                            }
                            return null;
                        });
                        assert.equal(clippingOwner, null, `${label}: outward focus ring must not be clipped`);
                        await selected.hover(); await page.waitForTimeout(200);
                        assert.equal(await selected.evaluate(node => getComputedStyle(node).color), selection.selected.color, `${label}: hover retains system selection foreground`);
                    }
                    await page.screenshot({ path: resolve(output, `forced-${name}.png`) });
                    await page.emulateMedia({ forcedColors: 'none' });
                    await page.getByRole('button', { name: 'Device', exact: true }).click();
                    await page.mouse.move(0, 0);
                }
                await page.screenshot({ path: resolve(output, `${name}.png`) });
                results.push({ name, ...result });
                console.log(`PASS ${name}`);
            } catch (error) {
                process.exitCode = 1;
                results.push({ name, failures: [error.stack] });
                await page.screenshot({ path: resolve(output, `failure-${name}.png`) });
                console.error(`FAIL ${name}: ${error.message}`);
            } finally { await page.close(); }
        }
    }
    if (process.env.APPEARANCE_WEBKIT === '1') {
        for (const stage of ['workspace', 'starting', 'unavailable', 'tx16w-idle', 'tx16w-busy']) {
            await new Promise((done, reject) => {
                child = spawn('/usr/bin/python3', ['tools/sample-editor-webkit.py', base, resolve(output, `webkit-${stage}`), '--fixture', `/tools/layout-fixtures/appearance.html?stage=${stage}`, '--checks', 'tools/appearance-checks.js', '--checks-function', 'runAppearanceChecks', '--zoom', '1', '--zoom', '1.25', '--zoom', '1.5', '--focused'], { stdio: 'inherit', detached: true });
                childExit = new Promise(resolveExit => child.once('close', resolveExit));
                child.once('error', reject);
                child.once('close', code => { child = undefined; code === 0 ? done() : reject(Error(`WebKitGTK ${stage} exited ${code}`)); });
            });
        }
    }
} finally {
    clearTimeout(deadline);
    await stop();
    process.removeListener('SIGTERM', interrupted); process.removeListener('SIGINT', interrupted);
    if (port) await new Promise((done, reject) => {
        const socket = createSocketServer(); socket.once('error', reject);
        socket.listen(port, '127.0.0.1', () => socket.close(error => error ? reject(error) : done()));
    });
    await writeFile(resolve(output, 'results.json'), `${JSON.stringify({ serverStopped: true, browserStopped: true, portReleased: !!port, results }, null, 2)}\n`);
}
process.exit(process.exitCode ?? 0);
