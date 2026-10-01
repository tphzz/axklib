import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { spawn } from 'node:child_process';
import { createServer } from 'vite';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const checks = await readFile(new URL('./editor-navigation-checks.js', import.meta.url), 'utf8');
const server = await createServer({ server: { host: '127.0.0.1', strictPort: false } });
let browser;
let child;
let childExit;
let interrupted = false;
let port;
const results = [];
const stopChild = async () => {
    if (!child?.pid) return;
    try { process.kill(-child.pid, 'SIGTERM'); } catch (error) { if (error.code !== 'ESRCH') throw error; }
    const force = setTimeout(() => {
        try { process.kill(-child.pid, 'SIGKILL'); } catch (error) { if (error.code !== 'ESRCH') throw error; }
    }, 5000);
    try { await childExit; } finally { clearTimeout(force); }
};
const stop = async () => { interrupted = true; await stopChild(); await browser?.close(); await server.close(); };
const deadline = setTimeout(() => void stop(), 240000);
const onSignal = () => void stop();
process.once('SIGINT', onSignal);
process.once('SIGTERM', onSignal);

async function frame(page) {
    await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
}

async function pointerOverflow(page) {
    const exercised = [];
    for (const kind of ['program', 'sample', 'bank']) {
        const host = page.locator(`[data-editor-fixture="${kind}"]`);
        const strips = host.locator('header [role="tablist"], .sample-pages nav');
        for (let index = 0; index < await strips.count(); index++) {
            const strip = strips.nth(index);
            if (!await strip.evaluate(node => node.scrollWidth > node.clientWidth + 1)) continue;
            const geometry = () => host.evaluate(node => {
                const box = element => {
                    const rect = element.getBoundingClientRect();
                    return [rect.x, rect.y, rect.width, rect.height];
                };
                const header = node.querySelector('header');
                const ancestors = [];
                for (let parent = node; parent; parent = parent.parentElement) ancestors.push(parent.scrollTop);
                return {
                    header: box(header),
                    actions: [...header.querySelectorAll('button:not([role="tab"])')].map(box),
                    ancestors,
                    panels: [...node.querySelectorAll('[role="tabpanel"]')].map(panel => panel.scrollTop),
                };
            });
            const before = await geometry();
            const area = await strip.boundingBox();
            await page.mouse.move(area.x + area.width / 2, area.y + area.height / 2);
            await page.mouse.wheel(1000, 0);
            await page.waitForFunction(element => element.scrollLeft > 0, await strip.elementHandle());
            await frame(page);
            assert(await strip.evaluate(node => {
                const area = node.getBoundingClientRect();
                const last = node.querySelector('button:last-child').getBoundingClientRect();
                return last.left >= area.left - 1 && last.right <= area.right + 1;
            }), `${kind}: horizontal mouse wheel did not reveal the last tab`);
            const last = strip.locator('button').last();
            await last.click();
            await frame(page);
            assert.equal(await last.getAttribute(index === 0 ? 'aria-selected' : 'aria-pressed'), 'true', `${kind}: mouse click did not select the revealed tab`);
            assert.deepEqual(await geometry(), before, `${kind}: horizontal wheel/click moved header, actions or vertical scroll`);
            exercised.push({ kind, row: index === 0 ? 'primary' : 'secondary', selected: await last.textContent() });
        }
    }
    return exercised;
}

try {
    await server.listen();
    port = server.httpServer.address().port;
    const base = `http://127.0.0.1:${port}`;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    const widths = process.env.EDITOR_NAVIGATION_RED === '1' ? [700] : [390, 460, 520, 700, 849, 850, 900, 1440];
    for (const width of widths) for (const scale of process.env.EDITOR_NAVIGATION_RED === '1' ? [1] : [1, 1.25, 1.5]) {
        if (interrupted) throw new Error('Editor navigation regression interrupted or exceeded 240 seconds');
        const workspace = width === 1440;
        const page = await browser.newPage({ viewport: { width: workspace ? width : Math.ceil(width * scale), height: workspace ? 900 : 1100 } });
        page.setDefaultTimeout(10000);
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        const name = `${width}-scale-${scale}${workspace ? '-workspace' : ''}`;
        try {
            await page.goto(`${base}/tools/layout-fixtures/editor-navigation.html?scale=${scale}${workspace ? '&workspace' : ''}`);
            await page.addScriptTag({ content: checks });
            const result = await page.evaluate(() => window.runEditorNavigationChecks());
            const entry = { name, ...result, errors };
            results.push(entry);
            assert.equal(result.failures.length, 0, result.failures.slice(0, 15).join('\n'));
            entry.pointerOverflow = await pointerOverflow(page);
            if (width === 390) assert.equal(entry.pointerOverflow.filter(item => item.row === 'primary').length, 2, 'Narrow Sample and Bank tabs must exercise mouse overflow');
            assert.deepEqual(errors, []);
            console.log(`PASS ${name}`);
        } catch (error) {
            if (results.at(-1)?.name !== name) results.push({ name, failures: [error.message], errors });
            else results.at(-1).failures.push(error.message);
            console.error(`FAIL ${name}: ${error.message}`); process.exitCode = 1;
        }
        finally {
            if (!page.isClosed()) { await page.screenshot({ path: resolve(output, `${name}.png`) }); await page.close(); }
        }
    }
    if (process.env.EDITOR_NAVIGATION_WEBKIT === '1' && !interrupted) {
        child = spawn('/usr/bin/python3', ['tools/sample-editor-webkit.py', base, resolve(output, 'webkit'),
            '--width', '1440', '--height', '900', '--zoom', '1', '--zoom', '1.25', '--zoom', '1.5', '--workspace',
            '--fixture', '/tools/layout-fixtures/editor-navigation.html', '--checks', 'tools/editor-navigation-checks.js',
            '--checks-function', 'runEditorNavigationChecks'], { stdio: 'inherit', detached: true });
        childExit = new Promise((resolve, reject) => { child.once('error', reject); child.once('exit', resolve); });
        const code = await childExit;
        await stopChild();
        child = undefined;
        if (code !== 0) process.exitCode = 1;
    }
} finally {
    clearTimeout(deadline);
    process.removeListener('SIGINT', onSignal);
    process.removeListener('SIGTERM', onSignal);
    await stopChild();
    await browser?.close();
    await server.close();
    await writeFile(resolve(output, 'results.json'), JSON.stringify({ port, serverStopped: true, results }, null, 2) + '\n');
}
