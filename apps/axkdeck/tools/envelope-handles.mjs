import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { spawn } from 'node:child_process';
import { createServer } from 'vite';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const checks = await readFile(new URL('./envelope-handles-checks.js', import.meta.url), 'utf8');
const server = await createServer({ server: { host: '127.0.0.1', strictPort: false } });
let browser, child, childExit, port;
let interrupted = false;
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
const frame = page => page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));

async function pointerChecks(page, name) {
    const fixture = page.locator('[data-envelope-fixture]');
    const controls = page.getByRole('navigation', { name: 'Envelope fixture controls' });
    const state = () => page.locator('[data-envelope-state]').evaluate(node => JSON.parse(node.textContent));
    const selected = fixture.getByRole('combobox', { name: 'Envelope stage' });
    const records = [];
    for (const mode of ['Sample', 'Bank']) for (const kind of ['aeg', 'feg', 'peg']) {
        for (const id of kind === 'aeg' ? ['1', '2'] : ['0', '1']) {
            await controls.getByRole('button', { name: mode, exact: true }).click();
            await controls.getByRole('button', { name: kind, exact: true }).click();
            await selected.selectOption(id);
            await frame(page);
            const before = await state();
            const handle = fixture.locator(`[data-handle="${id}"]`);
            const box = await handle.boundingBox();
            const center = { x: box.x + box.width / 2, y: box.y + box.height / 2 };
            assert(await handle.evaluate(node => {
                const box = node.getBoundingClientRect();
                return document.elementFromPoint(box.x + box.width / 2, box.y + box.height / 2) === node;
            }), `${mode} ${kind} ${id}: selected overlap not hit-testable`);
            const geometry = await fixture.locator('.graph-surface').boundingBox();
            const horizontal = id === '0' ? 80 : geometry.width * 0.08;
            await page.mouse.move(center.x, center.y);
            await frame(page);
            assert(await handle.evaluate(node => {
                const style = getComputedStyle(node);
                return node.matches(':hover') && style.outlineStyle === 'solid' && parseFloat(style.outlineWidth) >= 2;
            }), `${mode} ${kind} ${id}: real hover halo missing`);
            assert.deepEqual(await handle.boundingBox(), box, 'Hover resized or moved handle');
            await page.mouse.down();
            // Initial level ignores horizontal movement, so the pointer leaves its handle.
            await page.mouse.move(center.x + horizontal, center.y + (kind === 'aeg' ? 22 : -22), { steps: 4 });
            await frame(page);
            assert(await handle.evaluate(node => {
                const style = getComputedStyle(node);
                return node.classList.contains('dragging') && style.outlineStyle === 'solid' && parseFloat(style.outlineWidth) >= 2;
            }), `${mode} ${kind} ${id}: captured drag halo missing`);
            if (id === '0') {
                const current = await handle.boundingBox();
                assert(center.x + 80 > current.x + current.width + 10, 'Initial drag must move pointer outside the handle');
            }
            await page.mouse.up();
            await selected.focus();
            await page.mouse.move(4, 4);
            await frame(page);
            assert(await handle.evaluate(node => node.classList.contains('selected') && !node.classList.contains('dragging') && getComputedStyle(node).outlineStyle === 'none'), `${mode} ${kind} ${id}: hover halo persisted after release/blur or selection was lost`);
            const after = await state();
            const allowed = id === '0' ? [`${kind}.init_level`] : id === '1'
                ? [`${kind}.attack_level`, `${kind}.attack_rate`]
                : [`${kind}.sustain_level`, `${kind}.decay_rate`];
            const changed = Object.keys(after.values).filter(key => before.values[key] !== after.values[key]);
            assert(changed.length > 0 && changed.every(key => allowed.includes(key)), `${mode} ${kind} ${id}: drag changed wrong stage: ${changed}`);
            assert(after.dirty && after.canUndo, `${mode} ${kind} ${id}: missing undoable change`);
            assert.deepEqual(await fixture.locator('.graph-surface').boundingBox(), geometry, 'Drag moved graph surface');
            await controls.getByRole('button', { name: 'Undo', exact: true }).click();
            await frame(page);
            const undone = await state();
            assert.deepEqual(undone.values, before.values, 'One undo did not restore gesture values');
            assert(!undone.dirty && !undone.canUndo, 'One undo did not restore bank overrides/undo stack');
            records.push({ mode, kind, id, changed });
        }
    }
    const exact = page.locator('[data-overlap-fixture]');
    for (const id of ['first', 'second', 'edge']) {
        await exact.getByRole('combobox', { name: 'Exact overlap stage' }).selectOption(id);
        await frame(page);
        const handle = exact.locator(`[data-handle="${id}"]`), box = await handle.boundingBox();
        await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
        await page.mouse.down();
        await page.mouse.move(box.x + box.width / 2 + (id === 'edge' ? -20 : 20), box.y + box.height / 2 - 20, { steps: 3 });
        await page.mouse.up(); await frame(page);
        const state = await page.locator('[data-overlap-state]').evaluate(node => JSON.parse(node.textContent));
        assert.equal(state.selected, id, `Exact overlap ${id}: wrong selected handle`);
        assert(state.changes[id], `Exact overlap ${id}: pointer drag did not edit selected handle`);
    }
    await controls.getByRole('button', { name: 'feg', exact: true }).click();
    await selected.selectOption('1');
    const first = fixture.locator('[data-handle="1"]');
    await first.hover(); await frame(page);
    await page.screenshot({ path: resolve(output, `${name}-hover.png`) });
    return records;
}

try {
    await server.listen();
    port = server.httpServer.address().port;
    const base = `http://127.0.0.1:${port}`;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    const matrix = process.env.ENVELOPE_HANDLES_QUICK === '1' ? [[700, 1]] : [[390, 1], [390, 1.5], [700, 1], [700, 1.5], [1000, 1], [1000, 1.5]];
    for (const [width, scale] of matrix) {
        if (interrupted) throw new Error('Envelope regression interrupted or exceeded 240 seconds');
        const page = await browser.newPage({ viewport: { width: Math.ceil(width * scale), height: 1200 }, deviceScaleFactor: scale });
        page.setDefaultTimeout(10000);
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        const name = `${width}-scale-${scale}`;
        const entry = { name, failures: [], errors };
        results.push(entry);
        try {
            await page.goto(`${base}/tools/layout-fixtures/envelope-handles.html?scale=${scale}`);
            await page.addScriptTag({ content: checks });
            Object.assign(entry, await page.evaluate(() => window.runEnvelopeHandlesChecks()));
            assert.equal(entry.failures.length, 0, entry.failures.join('\n'));
            entry.pointer = await pointerChecks(page, name);
            assert.deepEqual(errors, []);
            console.log(`PASS ${name}`);
        } catch (error) {
            entry.failures.push(error.message); console.error(`FAIL ${name}: ${error.message}`); process.exitCode = 1;
        } finally {
            if (!page.isClosed()) { await page.screenshot({ path: resolve(output, `${name}.png`) }); await page.close(); }
        }
    }
    if (process.env.ENVELOPE_HANDLES_WEBKIT === '1' && !interrupted) {
        child = spawn('/usr/bin/python3', ['tools/sample-editor-webkit.py', base, resolve(output, 'webkit'),
            '--width', '800', '--height', '1100', '--zoom', '1', '--zoom', '1.5', '--focused',
            '--fixture', '/tools/layout-fixtures/envelope-handles.html', '--checks', 'tools/envelope-handles-checks.js',
            '--checks-function', 'runEnvelopeHandlesChecks'], { stdio: 'inherit', detached: true });
        childExit = new Promise((resolve, reject) => { child.once('error', reject); child.once('exit', resolve); });
        const code = await childExit;
        await stopChild(); child = undefined;
        if (code !== 0) process.exitCode = 1;
    }
} finally {
    clearTimeout(deadline);
    process.removeListener('SIGINT', onSignal);
    process.removeListener('SIGTERM', onSignal);
    await stopChild(); await browser?.close(); await server.close();
    await writeFile(resolve(output, 'results.json'), JSON.stringify({ port, serverStopped: true, results }, null, 2) + '\n');
}
