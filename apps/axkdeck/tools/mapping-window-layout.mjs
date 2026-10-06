import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { createConnection, createServer as createSocketServer } from 'node:net';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createServer } from 'vite';
import { bankMappingCases } from './bank-mapping-cases.mjs';
import { mappingPresentationCases } from './mapping-presentation-cases.mjs';
import { mappingAuditionCases } from './mapping-audition-cases.mjs';
import { mappingNavigationCases } from './mapping-navigation-cases.mjs';

assert.ok(process.argv[2], 'Provide a build/reports output directory');
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const availablePort = await new Promise((resolve, reject) => {
    const socket = createSocketServer();
    socket.once('error', reject);
    socket.listen(0, '127.0.0.1', () => {
        const port = socket.address().port;
        socket.close(error => error ? reject(error) : resolve(port));
    });
});
const server = await createServer({ server: { host: '127.0.0.1', port: availablePort, strictPort: true } });
let browser, port, stopping;
const results = [];
function stop() {
    return stopping ??= (async () => {
        try { await browser?.close(); }
        finally { await server.close(); }
    })();
}
const signal = () => { process.exitCode = 1; void stop(); };
process.once('SIGTERM', signal);
process.once('SIGINT', signal);
const deadline = setTimeout(signal, 180000);
const state = async page => JSON.parse(await page.locator('[data-mapping-main]').textContent());
const snapshot = async page => JSON.parse(await page.locator('[data-mapping-snapshot]').textContent()).state;
async function waitState(page, key, value) {
    await page.waitForFunction(({ key, value }) => JSON.parse(document.querySelector('[data-mapping-main]').textContent)[key] === value, { key, value });
}
async function waitValue(page, key, value) {
    await page.waitForFunction(({ key, value }) => JSON.parse(document.querySelector('[data-mapping-main]').textContent).values[key] === value, { key, value });
}
async function waitLimit(page, key, value) {
    await page.waitForFunction(({ key, value }) => JSON.parse(document.querySelector('[data-mapping-snapshot]').textContent)?.state.limits?.[key] === value, { key, value });
}
async function pressHandle(page, label, key) {
    await page.waitForFunction(label => [...document.querySelectorAll('button')].some(button => button.getAttribute('aria-label') === label && !button.disabled), label);
    await page.getByRole('button', { name: label, exact: true }).press(key);
}
async function geometry(page) {
    return page.locator('.mapping-window').evaluate(root => {
        const rect = node => {
            const box = node.getBoundingClientRect();
            return { x: box.x, y: box.y, right: box.right, bottom: box.bottom, width: box.width, height: box.height };
        };
        const header = root.querySelector('header');
        const canvas = root.querySelector('.canvas');
        const plot = root.querySelector('.plot');
        const footer = root.querySelector('footer');
        return {
            viewport: { width: innerWidth, height: innerHeight, scale: devicePixelRatio },
            root: rect(root), header: rect(header), canvas: rect(canvas), plot: rect(plot), footer: rect(footer),
            keyboard: rect(root.querySelector('.keyboard')), legend: rect(root.querySelector('.legend')),
            heading: rect(root.querySelector('h1')), actions: rect(root.querySelector('.actions')),
            handles: [...root.querySelectorAll('.limit-handle')].map(node => ({ ...rect(node), label: node.getAttribute('aria-label') })),
            zones: [...root.querySelectorAll('.zone')].map(node => ({ ...rect(node), background: getComputedStyle(node).backgroundColor })),
            gridPaths: plot.querySelectorAll('.grid-overlay path').length,
            pageOverflow: document.documentElement.scrollWidth > innerWidth,
        };
    });
}
async function openMapping(main, context, viewport, errors) {
    const promise = context.waitForEvent('page');
    await main.getByRole('button', { name: 'Mapping Editor', exact: true }).click();
    const page = await promise;
    page.on('pageerror', error => errors.push(`mapping: ${error.message}`));
    page.setDefaultTimeout(10000);
    await page.setViewportSize(viewport);
    await page.getByRole('button', { name: 'Low key limit', exact: true }).waitFor();
    return page;
}

try {
    await server.listen();
    port = server.httpServer.address().port;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    for (const zoom of [1, 1.25, 1.5]) {
        if (stopping) break;
        const name = `mapping-${Math.round(zoom * 100)}`;
        const viewport = zoom === 1 ? { width: 1040, height: 680 } : { width: 800, height: 600 };
        const context = await browser.newContext({ viewport, deviceScaleFactor: zoom });
        const main = await context.newPage();
        main.setDefaultTimeout(10000);
        const result = { name, zoom, viewport, errors: [], requests: [] };
        results.push(result);
        main.on('pageerror', error => result.errors.push(`main: ${error.message}`));
        context.on('request', request => {
            if (request.method() !== 'GET') result.requests.push({ method: request.method(), url: request.url() });
        });
        let mapping;
        try {
            await main.goto(`http://127.0.0.1:${port}/tools/layout-fixtures/mapping-window.html?channel=${name}`);
            await waitState(main, 'ready', true);
            mapping = await openMapping(main, context, viewport, result.errors);
            await mapping.getByRole('heading', { name: 'Mapping Editor: Warm pad', exact: true }).waitFor();
            result.geometry = await geometry(mapping);
            const layout = result.geometry;
            assert.equal(layout.pageOverflow, false, 'Mapping must not overflow horizontally');
            assert.ok(layout.heading.right <= layout.actions.x + 1, 'Title and actions must not overlap');
            assert.ok(layout.plot.width > 500 && layout.plot.height > 250, 'Mapping plot must be usable and nonblank');
            assert.ok(layout.gridPaths === 4 && layout.zones.length === 2, 'Mapping paints both assignment zones and four batched grid strokes');
            assert.ok(layout.footer.bottom <= layout.viewport.height + 1, 'Footer must remain inside the window');
            assert.ok(layout.canvas.bottom <= layout.footer.y + 1, 'Canvas and footer must not overlap');
            assert.ok(layout.plot.bottom <= layout.keyboard.y + 1, 'Keyboard axis must follow the mapping plot');
            assert.ok(layout.legend.y >= layout.keyboard.bottom - 1 && layout.legend.y - layout.keyboard.bottom < 20, 'The mapping must not leave a large unused grid row below its keyboard');
            assert.ok(layout.handles.every(handle => (handle.x + handle.right) / 2 >= layout.plot.x - 1 && (handle.x + handle.right) / 2 <= layout.plot.right + 1), 'Handle centers must follow the exact range boundaries');
            assert.ok(layout.zones.every(zone => zone.background !== 'rgba(0, 0, 0, 0)'), 'Effective zones must be painted');
            await mapping.screenshot({ path: resolve(output, `${name}-window.png`) });

            await mapping.getByRole('button', { name: 'Sample/Bank', exact: true }).click();
            const popup = mapping.getByRole('listbox', { name: 'Sample/Bank', exact: true });
            result.dropdown = await popup.evaluate(node => {
                const box = node.getBoundingClientRect(), style = getComputedStyle(node);
                return { x: box.x, y: box.y, right: box.right, bottom: box.bottom, background: style.backgroundColor, color: style.color, border: style.borderColor };
            });
            assert.ok(result.dropdown.x >= 0 && result.dropdown.right <= viewport.width && result.dropdown.bottom <= viewport.height, 'Assignment menu must fit the viewport');
            assert.notEqual(result.dropdown.background, 'rgba(0, 0, 0, 0)', 'Assignment menu needs a theme background');
            assert.notEqual(result.dropdown.color, 'rgb(0, 0, 0)', 'Assignment menu must inherit editor text color');
            assert.equal(await mapping.getByRole('option').count(), 2, 'Duplicate assignment identities remain distinct');
            await mapping.screenshot({ path: resolve(output, `${name}-dropdown.png`) });
            await popup.press('End');
            await popup.press('Enter');
            await waitState(main, 'assignment', 1);
            await waitLimit(mapping, 'low', 48);
            await pressHandle(mapping, 'High velocity limit', 'ArrowDown');
            await waitValue(main, 'assignments.1.velocity_high', 126);
            assert.equal((await state(main)).commands.filter(command => command.action.kind === 'range').length, 1);
            await mapping.getByRole('button', { name: 'Undo', exact: true }).click();
            await waitValue(main, 'assignments.1.velocity_high', 127);
            assert.equal((await state(main)).canUndo, false, 'One keyboard gesture must be one undo entry');

            await main.getByRole('button', { name: 'Sample/Bank', exact: true }).click();
            await main.getByRole('option', { name: 'Duplicate (Sample) · 1', exact: true }).click();
            await waitLimit(mapping, 'low', 24);
            const handle = mapping.getByRole('button', { name: 'Low key limit', exact: true });
            const box = await handle.boundingBox(), plot = await mapping.locator('.plot').boundingBox();
            const beforeDrag = (await state(main)).commands.filter(command => command.action.kind === 'range').length;
            await mapping.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
            await mapping.mouse.down();
            await mapping.mouse.move(box.x + box.width / 2 + plot.width * 20 / 128, box.y + box.height / 2, { steps: 20 });
            assert.equal((await state(main)).values['assignments.0.key_low'], 24, 'An unfinished drag must remain a child preview');
            const guide = await mapping.locator('.range-guide[data-boundary="low"]').getAttribute('x1');
            assert.equal(Number(guide), 36, 'Coverage guide remains on the Sample edge during a local limits preview');
            await mapping.waitForFunction(() => document.querySelector('.limits').style.left === '34.375%');
            assert.equal(await mapping.locator('.limits').evaluate(node => node.style.left), '34.375%', 'Editable bounds follow the local drag before commit');
            await mapping.mouse.up();
            await waitValue(main, 'assignments.0.key_low', 44);
            await waitLimit(mapping, 'low', 44);
            assert.equal(Number(await mapping.locator('.range-guide[data-boundary="low"]').getAttribute('x1')), 44);
            assert.equal((await state(main)).commands.filter(command => command.action.kind === 'range').length, beforeDrag + 1);
            await mapping.screenshot({ path: resolve(output, `${name}-drag.png`) });
            await mapping.getByRole('button', { name: 'Undo', exact: true }).click();
            await waitValue(main, 'assignments.0.key_low', 24);
            assert.equal((await state(main)).canUndo, false, 'The complete pointer drag must be one undo entry');
            const moveBefore = (await state(main)).commands.filter(command => command.action.kind === 'move').length;
            const movePlot = await mapping.locator('.plot').boundingBox();
            const movePoint = { x: movePlot.x + movePlot.width * 40.5 / 128, y: movePlot.y + movePlot.height / 3 };
            await mapping.mouse.move(movePoint.x, movePoint.y); await mapping.mouse.down();
            await mapping.mouse.move(movePoint.x + 2, movePoint.y); await mapping.mouse.up();
            await waitState(main, 'assignment', 0);
            assert.equal((await state(main)).commands.filter(command => command.action.kind === 'move').length, moveBefore, 'A sub-threshold click is not a move');
            await mapping.mouse.move(movePoint.x, movePoint.y); await mapping.mouse.down();
            await mapping.mouse.move(movePoint.x + movePlot.width * 5 / 128, movePoint.y, { steps: 8 });
            assert.equal((await state(main)).values['assignments.0.key_low'], 24, 'Whole-block movement remains local until release');
            await mapping.mouse.up(); await waitValue(main, 'assignments.0.key_low', 29);
            assert.equal((await state(main)).values['assignments.0.key_high'], 105, 'Block movement preserves its width');
            assert.equal((await state(main)).values['assignments.0.key_shift'], 0, 'Moving a block never transposes its root');
            assert.equal((await state(main)).commands.filter(command => command.action.kind === 'move').length, moveBefore + 1);
            await mapping.getByRole('button', { name: 'Undo', exact: true }).click(); await waitValue(main, 'assignments.0.key_low', 24);
            assert.equal((await state(main)).canUndo, false, 'A moved block has exactly one undo step');
            await mapping.mouse.move(movePoint.x, movePoint.y); await mapping.mouse.down();
            await mapping.mouse.move(movePoint.x + movePlot.width * 5 / 128, movePoint.y, { steps: 8 });
            await mapping.keyboard.press('Escape'); await mapping.mouse.up();
            assert.equal((await state(main)).values['assignments.0.key_low'], 24, 'Escape cancels a moved block');
            assert.equal((await state(main)).commands.filter(command => command.action.kind === 'move').length, moveBefore + 1);
            await main.getByRole('spinbutton', { name: 'Low key', exact: true }).fill('28');
            await waitLimit(mapping, 'low', 28);
            await main.getByRole('button', { name: 'Main undo', exact: true }).click();
            await waitLimit(mapping, 'low', 24);

            await pressHandle(mapping, 'High velocity limit', 'ArrowDown');
            await waitValue(main, 'assignments.0.velocity_high', 126);
            await mapping.getByRole('button', { name: 'Save', exact: true }).click();
            await waitState(main, 'saves', 1);
            await waitState(main, 'dirty', false);
            await pressHandle(mapping, 'Low key limit', 'ArrowRight');
            await waitValue(main, 'assignments.0.key_low', 25);
            await mapping.close();
            mapping = await openMapping(main, context, viewport, result.errors);
            await waitLimit(mapping, 'low', 25);
            assert.equal((await state(main)).dirty, true, 'Closing the child must retain accepted main-draft changes');
            await main.getByRole('button', { name: 'Main undo', exact: true }).click();
            await waitLimit(mapping, 'low', 24);
            assert.equal((await state(main)).values['assignments.0.velocity_high'], 126, 'Undo after reopen must retain the saved baseline');

            await main.getByRole('button', { name: 'Select Bright keys', exact: true }).click();
            await mapping.getByRole('heading', { name: 'Mapping Editor: Bright keys', exact: true }).waitFor();
            await waitLimit(mapping, 'low', 12);
            await main.getByRole('button', { name: 'Select Read-only Program', exact: true }).click();
            await mapping.getByRole('heading', { name: 'Mapping Editor: Read-only Program', exact: true }).waitFor();
            assert.equal(await mapping.getByRole('button', { name: 'Low key limit', exact: true }).isDisabled(), true);
            assert.equal(await mapping.getByRole('button', { name: 'Save', exact: true }).isDisabled(), true);
            await mapping.getByRole('button', { name: 'Low key limit', exact: true }).dispatchEvent('keydown', { key: 'ArrowRight' });
            assert.equal((await state(main)).values['assignments.0.key_low'], 24);
            await mapping.screenshot({ path: resolve(output, `${name}-readonly.png`) });
            await main.getByRole('button', { name: 'Close image', exact: true }).click();
            await mapping.getByRole('button', { name: 'Low key limit', exact: true }).waitFor({ state: 'detached' });
            await main.getByRole('button', { name: 'Select Warm pad', exact: true }).click();
            await waitLimit(mapping, 'low', 24);

            const oldOwner = (await snapshot(mapping)).owner;
            await main.reload();
            await waitState(main, 'ready', true);
            const newOwner = (await state(main)).owner;
            assert.notEqual(newOwner, oldOwner, 'Main reload must create an independent owner identity');
            await mapping.waitForFunction(owner => JSON.parse(document.querySelector('[data-mapping-snapshot]').textContent)?.state.owner === owner, newOwner);
            await waitLimit(mapping, 'velocityHigh', 127);
            await pressHandle(mapping, 'High velocity limit', 'ArrowDown');
            await waitValue(main, 'assignments.0.velocity_high', 126);
            result.ownerReplacement = { oldOwner, newOwner };
            assert.deepEqual(result.errors, [], 'Neither main nor mapping page may emit runtime errors');
            assert.deepEqual(result.requests, [], 'Browser fixture must not make write requests');
            result.passed = true;
            console.log(`${name}: cross-window selection, keyboard, pointer, undo, save, read-only, reopen and owner replacement passed`);
        } catch (error) {
            result.error = error.message;
            result.mainState = await state(main).catch(() => null);
            result.mappingState = await mapping?.locator('[data-mapping-snapshot]').textContent().then(value => JSON.parse(value)).catch(() => null);
            process.exitCode = 1;
            console.error(`${name}: ${error.stack ?? error.message}`);
            await mapping?.screenshot({ path: resolve(output, `${name}-failure.png`) }).catch(() => {});
        } finally { await context.close(); }
    }
    await bankMappingCases(browser, `http://127.0.0.1:${port}`, output, results);
    await mappingPresentationCases(browser, `http://127.0.0.1:${port}`, output, results);
    await mappingAuditionCases(browser, `http://127.0.0.1:${port}`, output, results);
    await mappingNavigationCases(browser, `http://127.0.0.1:${port}`, output, results);
} finally {
    clearTimeout(deadline);
    process.removeListener('SIGTERM', signal);
    process.removeListener('SIGINT', signal);
    await stop();
    const portReleased = port === undefined || await new Promise(resolve => {
        const socket = createConnection({ host: '127.0.0.1', port });
        socket.once('connect', () => { socket.destroy(); resolve(false); });
        socket.once('error', () => resolve(true));
    });
    await writeFile(resolve(output, 'results.json'), JSON.stringify({
        port, serverStopped: true, browserStopped: !browser?.isConnected(), portReleased,
        scaleMethod: 'Bank/Sample CSS zoom and Program device-scale emulation at 100/125/150 percent; not native WebView zoom', results,
    }, null, 2) + '\n');
    assert.equal(portReleased, true, 'Owned Vite port must be released');
}
