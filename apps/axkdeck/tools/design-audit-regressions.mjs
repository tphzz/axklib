import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createServer as socketServer } from 'node:net';
import { spawn } from 'node:child_process';
import { createServer } from 'vite';

const { chromium } = await import(
    process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright'
);
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const checks = await readFile(new URL('./design-audit-checks.js', import.meta.url), 'utf8');
const server = await createServer({ server: { host: '127.0.0.1', port: 0 } });
let browser, port, child, childExit, stopping;
const results = [];
const stop = () =>
    (stopping ??= (async () => {
        if (child?.pid) {
            try {
                process.kill(-child.pid, 'SIGTERM');
            } catch (e) {
                if (e.code !== 'ESRCH') throw e;
            }
            const force = setTimeout(() => {
                try {
                    process.kill(-child.pid, 'SIGKILL');
                } catch {}
            }, 3000);
            try {
                await childExit;
            } finally {
                clearTimeout(force);
            }
        }
        try {
            await browser?.close();
        } finally {
            await server.close();
        }
    })());
const interrupted = () => {
    process.exitCode = 1;
    void stop();
};
process.once('SIGTERM', interrupted);
process.once('SIGINT', interrupted);
const deadline = setTimeout(interrupted, 540000);
const contrast = async (locator) =>
    locator.evaluate((node) => {
        const s = getComputedStyle(node);
        let bg = s.backgroundColor;
        for (let p = node.parentElement; p && bg === 'rgba(0, 0, 0, 0)'; p = p.parentElement)
            bg = getComputedStyle(p).backgroundColor;
        const lum = (c) =>
            c
                .match(/[\d.]+/g)
                .slice(0, 3)
                .map(Number)
                .map((v) => {
                    v /= 255;
                    return v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4;
                })
                .reduce((n, v, i) => n + v * [0.2126, 0.7152, 0.0722][i], 0);
        const a = lum(s.color),
            b = lum(bg);
        return { foreground: s.color, background: bg, ratio: (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05) };
    });
try {
    await server.listen();
    port = server.httpServer.address().port;
    const base = `http://127.0.0.1:${port}`;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    for (const scale of [1, 1.25, 1.5]) {
        for (const mode of ['package', 'workspace', 'allocation', 'wave', 'completion']) {
            const name = `${mode}-${scale}`;
            const page = await browser.newPage({
                viewport: { width: Math.ceil(1280 / scale), height: Math.ceil(900 / scale) },
                deviceScaleFactor: scale,
            });
            page.setDefaultTimeout(10000);
            const errors = [];
            page.on('pageerror', (error) => errors.push(error.message));
            try {
                const query =
                    mode === 'allocation' ? '&imageId=audit&sourceMemberId=source-1&revision=1&partitionIndex=0' : '';
                await page.goto(`${base}/tools/layout-fixtures/design-audit.html?component=${mode}${query}`);
                await page.addScriptTag({ content: checks });
                await page.exposeFunction('captureAuditState', (label) =>
                    page.screenshot({ path: resolve(output, `${name}-${label}.png`) }),
                );
                const result = await page.evaluate(() => window.runDesignAuditChecks());
                assert.deepEqual(result.failures, []);
                if (mode === 'allocation') {
                    const selected = page.getByRole('button', { name: 'Cluster cell size: 7', exact: true });
                    const pair = await contrast(selected);
                    assert(pair.ratio >= 4.5);
                    result.measurements.push(pair);
                    await selected.focus();
                    await page.keyboard.press('ArrowRight');
                    assert.equal(
                        await page
                            .getByRole('button', { name: 'Cluster cell size: 11', exact: true })
                            .getAttribute('aria-pressed'),
                        'true',
                    );
                    const map = page.getByRole('slider', { name: 'Partition cluster', exact: true });
                    await map.focus();
                    await page.keyboard.press('End');
                    assert.equal(await map.getAttribute('aria-valuenow'), '65535');
                    await page.keyboard.press('Enter');
                    const detail = page.getByRole('region', { name: 'Partition cluster details', exact: true });
                    await detail.waitFor();
                    await page.keyboard.press('PageDown');
                    await page.waitForFunction(() => document.querySelector('.allocation-tooltip').scrollTop > 0);
                    await page.screenshot({ path: resolve(output, `${name}-details.png`) });
                    await page.keyboard.press('Escape');
                    assert.equal(await detail.count(), 0);
                }
                if (mode === 'wave') {
                    const range = page.getByRole('slider', { name: 'Seek Audit wave', exact: true });
                    await range.focus();
                    await page.keyboard.press('ArrowRight');
                    assert(Number(await range.inputValue()) > 0.5);
                    await page.keyboard.press('End');
                    assert.equal(Number(await range.inputValue()), 1);
                    await page.keyboard.press('Home');
                    assert.equal(Number(await range.inputValue()), 0);
                    assert.equal(await page.evaluate(() => window.waveAudit.plays), 0);
                    const before = await page.evaluate(() => window.waveAudit.seeks.length);
                    await range.click({ modifiers: ['Control'] });
                    assert.equal(await page.evaluate(() => window.waveAudit.seeks.length), before);
                    assert.equal(await page.evaluate(() => window.waveAudit.selections), 1);
                    await page.emulateMedia({ forcedColors: 'active' });
                    assert.equal(await range.evaluate((n) => getComputedStyle(n).opacity), '1');
                    await page.emulateMedia({ forcedColors: 'none' });
                    await page.goto(base + '/tools/layout-fixtures/design-audit.html?component=wave&idle=1');
                    await page.getByRole('button', { name: 'Select Audit wave waveform', exact: true }).click();
                    assert.equal(
                        await page.getByRole('slider').count(),
                        0,
                        'Idle waveform must not advertise an ignored seek position',
                    );
                    assert.equal(await page.evaluate(() => window.waveAudit.plays), 0);
                }
                await page.screenshot({ path: resolve(output, `${name}.png`) });
                assert.deepEqual(errors, []);
                results.push({ name, ...result });
                console.log('PASS', name);
            } catch (error) {
                await page.screenshot({ path: resolve(output, `${name}-failure.png`) });
                throw error;
            } finally {
                await page.close();
            }
        }
        const page = await browser.newPage({ viewport: { width: 1920, height: 1080 }, deviceScaleFactor: scale });
        try {
            await page.goto(base + '/tools/layout-fixtures/sample-editor.html');
            await page.getByRole('button', { name: 'Save', exact: true }).waitFor();
            await page.getByRole('tab', { name: 'Filter', exact: true }).click();
            await page
                .getByLabel('Sample subpages', { exact: true })
                .getByRole('button', { name: 'Sample EQ', exact: true })
                .click();
            const choice = page.locator('.editor-choices[aria-label="EQ type"] [aria-pressed=true]');
            const pairs = [];
            for (const state of ['rest', 'hover', 'focus']) {
                if (state === 'hover') await choice.hover();
                if (state === 'focus') {
                    await page.mouse.move(0, 0);
                    await choice.focus();
                }
                const pair = await contrast(choice);
                assert(pair.ratio >= 4.5, `${state}: ${pair.ratio}`);
                pairs.push({ state, ...pair });
            }
            for (const node of await page
                .locator(
                    '.sample-transport .autoplay, .sample-transport small, .sample-transport .editor-meta, .sample-transport .transport-status',
                )
                .all()) {
                const pair = await contrast(node);
                assert(pair.ratio >= 4.5);
                pairs.push({ caption: await node.textContent(), ...pair });
            }
            results.push({ name: `editor-${scale}`, measurements: pairs, failures: [] });
            await page.screenshot({ path: resolve(output, `editor-${scale}.png`) });
            console.log('PASS editor', scale);
        } finally {
            await page.close();
        }
    }
    for (const width of [800, 390]) {
        const page = await browser.newPage({ viewport: { width, height: 740 } });
        try {
            await page.goto(base + '/tools/layout-fixtures/design-audit.html?component=package');
            await page.addScriptTag({ content: checks });
            const result = await page.evaluate(() => window.runDesignAuditChecks());
            assert.deepEqual(result.failures, []);
            results.push({ name: `package-${width}`, ...result });
            await page.screenshot({ path: resolve(output, `package-${width}.png`) });
        } finally {
            await page.close();
        }
    }
    for (const zoom of [1, 1.25, 1.5]) {
        const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
        try {
            await page.goto(
                base +
                    '/tools/layout-fixtures/design-audit.html?component=allocation&imageId=audit&sourceMemberId=source-1&revision=1&partitionIndex=0',
            );
            const canvas = page.getByRole('slider', { name: 'Partition cluster', exact: true });
            await canvas.waitFor();
            await page.evaluate((z) => (document.documentElement.style.zoom = String(z)), zoom);
            await page.getByRole('button', { name: 'Cluster cell size: 4', exact: true }).click();
            await canvas.focus();
            await page.keyboard.press('Home');
            await page.keyboard.press('Escape');
            assert(
                await page.locator('.allocation-viewport').evaluate((n) => n.scrollHeight > n.clientHeight),
                'Scroll regression requires overflowing map',
            );
            const scrollPosition = await page.locator('.allocation-viewport').evaluate((n) => {
                n.scrollTop = 30;
                return n.scrollTop;
            });
            await page.waitForTimeout(200);
            assert.equal(
                await page.locator('.allocation-viewport').evaluate((n) => n.scrollTop),
                scrollPosition,
                'Focused map must not snap pointer scrolling back',
            );
            await page.locator('.allocation-viewport').evaluate((n) => (n.scrollTop = 0));
            await page.waitForTimeout(200);
            const geometry = await canvas.evaluate((n) => {
                const b = n.getBoundingClientRect();
                return {
                    left: b.left,
                    top: b.top,
                    scale: b.width / n.clientWidth,
                    columns: Math.floor((n.clientWidth + 1) / 5),
                };
            });
            await page.getByRole('button', { name: 'Cluster cell size: 4', exact: true }).focus();
            await page.mouse.move(geometry.left + geometry.scale * 16, geometry.top + geometry.scale * 11);
            await page.waitForTimeout(100);
            assert.equal(
                Number(await canvas.getAttribute('aria-valuenow')),
                geometry.columns * 2 + 3,
                'Scaled pointer resolves the visible cluster',
            );
            results.push({ name: `allocation-pointer-zoom-${zoom}`, failures: [], geometry });
        } finally {
            await page.close();
        }
    }
    if (process.env.WEBKITGTK_TESTS === '1')
        for (const mode of ['package', 'workspace', 'allocation', 'wave', 'completion', 'transport']) {
            const query =
                mode === 'allocation' ? '&imageId=audit&sourceMemberId=source-1&revision=1&partitionIndex=0' : '';
            const fixture =
                mode === 'transport'
                    ? '/tools/layout-fixtures/sample-editor.html'
                    : `/tools/layout-fixtures/design-audit.html?component=${mode}${query}`;
            child = spawn(
                '/usr/bin/python3',
                [
                    'tools/sample-editor-webkit.py',
                    base,
                    resolve(output, `webkit-${mode}`),
                    '--fixture',
                    fixture,
                    '--checks',
                    'tools/design-audit-checks.js',
                    '--checks-function',
                    'runDesignAuditChecks',
                    '--focused',
                    '--width',
                    '1280',
                    '--height',
                    '900',
                    '--zoom',
                    '1',
                    '--zoom',
                    '1.25',
                    '--zoom',
                    '1.5',
                ],
                { stdio: 'inherit', detached: true },
            );
            childExit = new Promise((resolve, reject) => {
                child.once('error', reject);
                child.once('exit', (code, signal) => resolve({ code, signal }));
            });
            const outcome = await childExit;
            child = undefined;
            assert.equal(outcome.code, 0, `GTK ${mode}`);
            results.push({ name: `webkit-${mode}`, ...outcome });
        }
} catch (error) {
    process.exitCode = 1;
    results.push({ name: 'failure', error: error.stack });
    console.error(error);
} finally {
    clearTimeout(deadline);
    await stop();
    process.removeListener('SIGTERM', interrupted);
    process.removeListener('SIGINT', interrupted);
    if (port)
        await new Promise((done, reject) => {
            const s = socketServer();
            s.once('error', reject);
            s.listen(port, '127.0.0.1', () => s.close((e) => (e ? reject(e) : done())));
        });
    await writeFile(
        resolve(output, 'results.json'),
        JSON.stringify({ results, serverStopped: true, browserStopped: true, portReleased: true }, null, 2) + '\n',
    );
}
process.exit(process.exitCode ?? 0);
