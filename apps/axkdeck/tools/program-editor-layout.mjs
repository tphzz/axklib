import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { createConnection, createServer as createSocketServer } from 'node:net';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createServer } from 'vite';

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
const deadline = setTimeout(signal, 240000);
const state = async page => JSON.parse(await page.locator('[data-program-state]').textContent());
const near = (actual, expected, message) => assert.ok(Math.abs(actual - expected) < 1, `${message}: ${actual} != ${expected}`);
const painted = page => page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
async function graphPanelGeometry(page, zoom) {
    const result = await page.locator('.graph-panel').evaluate(panel => {
        const rect = node => {
            const box = node.getBoundingClientRect();
            return { x: box.x, y: box.y, width: box.width, height: box.height, right: box.right, bottom: box.bottom };
        };
        const graph = panel.querySelector('.graph-region');
        const controls = panel.querySelector('.graph-controls');
        return { panel: rect(panel), cssWidth: panel.clientWidth, graph: rect(graph), controls: rect(controls), graphOverflow: graph.scrollWidth > graph.clientWidth, splitters: panel.querySelectorAll('[role="separator"]').length, overflowY: getComputedStyle(panel).overflowY, scrollHeight: panel.scrollHeight, clientHeight: panel.clientHeight };
    });
    assert.equal(result.graphOverflow, false, 'Graph must not need horizontal scrolling');
    if (result.cssWidth >= 740) {
        assert.equal(result.splitters, 1, 'Wide Program graphs must retain their resize control');
        assert.ok(result.graph.right <= result.controls.x + 1, 'Program graph must sit left of controls');
        near(result.graph.y, result.controls.y, 'Graph and controls share the same top edge');
        assert.ok(result.graph.width >= 360 * zoom - 1, 'Program graph retains usable width');
        assert.ok(result.controls.width >= 300 * zoom - 1, 'Program controls retain usable width');
    } else {
        assert.equal(result.splitters, 0, 'Narrow Program graphs must stack without a resize control');
        assert.ok(result.graph.bottom <= result.controls.y + 1, 'Narrow Program controls stack below the graph');
    }
    assert.ok(result.graph.x >= result.panel.x - 1 && result.controls.right <= result.panel.right + 1, 'Graph and controls stay framed');
    if (result.graph.bottom > result.panel.bottom + 1 || result.controls.bottom > result.panel.bottom + 1) {
        assert.ok(['auto', 'scroll'].includes(result.overflowY) && result.scrollHeight > result.clientHeight, 'Stacked graphs extending below the pane must have a scrollable outer panel');
    }
    return result;
}
async function envelopeControlReachability(page, zoom) {
    const before = await geometry(page, zoom);
    const controls = page.locator('.graph-controls');
    const reached = [];
    for (const label of ['Attack rate offset', 'Decay rate offset', 'Release rate offset']) {
        const input = controls.getByRole('spinbutton', { name: label, exact: true });
        await input.scrollIntoViewIfNeeded();
        await input.focus();
        const bounds = await input.boundingBox();
        const panel = await page.locator('.graph-panel').boundingBox();
        const controlBounds = await controls.boundingBox();
        assert.ok(bounds.y >= Math.max(panel.y, controlBounds.y) - 1 && bounds.y + bounds.height <= Math.min(panel.y + panel.height, controlBounds.y + controlBounds.height) + 1, `${label} must be fully reachable within its scrollable editor pane`);
        reached.push({ label, ...bounds });
    }
    const after = await geometry(page, zoom);
    assert.deepEqual(after.headers, before.headers, 'Scrolling to envelope offsets must not move editor headers');
    assert.deepEqual(after.footer, before.footer, 'Scrolling to envelope offsets must not move the status footer');
    return reached;
}
async function geometry(page, zoom) {
    const result = await page.locator('.device-editor').evaluate(editor => {
        const rect = node => {
            const box = node.getBoundingClientRect();
            return { x: box.x, y: box.y, width: box.width, height: box.height, right: box.right, bottom: box.bottom };
        };
        const headers = [...editor.querySelectorAll('.editor-header')].map(header => ({
            ...rect(header),
            tabs: rect(header.querySelector('nav')),
            activeTab: rect(header.querySelector('[aria-selected="true"], [aria-pressed="true"]')),
            tools: rect(header.querySelector('.header-tools')),
            buttons: [...header.querySelectorAll('.header-tools button')].map(rect),
        }));
        return { editor: rect(editor), headers, panel: rect(editor.querySelector('.program-panel')), footer: rect(editor.querySelector('footer')), pageOverflow: document.documentElement.scrollWidth > innerWidth };
    });
    assert.equal(result.pageOverflow, false, 'Page must not overflow horizontally');
    near(result.headers[0].height, 32 * zoom, 'Primary header height');
    near(result.headers[1].height, 28 * zoom, 'Secondary header height');
    assert.ok(result.panel.y >= result.headers[1].bottom - 1, 'Content must not overlap the secondary header');
    assert.ok(result.panel.bottom <= result.footer.y + 1, 'Content must not overlap the status footer');
    near(result.footer.bottom, result.editor.bottom, 'Footer must stay inside lower editor pane');
    for (const header of result.headers) {
        assert.ok(header.tabs.width >= header.activeTab.width - 1, `Active tab must fit visible strip: ${JSON.stringify(header)}`);
        assert.ok(header.activeTab.x >= header.tabs.x - 1 && header.activeTab.right <= header.tabs.right + 1, 'Active tab must be revealed within the strip');
        assert.ok(header.tabs.right <= header.tools.x + 1, 'Tabs and actions must not overlap');
        assert.ok(header.tools.right <= header.right + 1, 'Actions must remain inside header');
        for (const button of header.buttons) {
            assert.ok(button.y >= header.y - 1 && button.bottom <= header.bottom + 1, 'Action must fit header height');
        }
    }
    return result;
}
async function graphGeometry(page, count) {
    return page.locator('.routing').evaluate((graph, count) => {
        const bounds = graph.getBoundingClientRect();
        const nodes = [...graph.querySelectorAll('button')].map(node => {
            const box = node.getBoundingClientRect();
            const labels = [...node.children].map(label => ({ text: label.textContent, height: label.clientHeight, textHeight: label.scrollHeight }));
            return { label: node.getAttribute('aria-label'), x: box.left - bounds.left, y: box.top - bounds.top, width: box.width, height: box.height, labels, fits: box.left >= bounds.left && box.right <= bounds.right && box.top >= bounds.top && box.bottom <= bounds.bottom };
        });
        const routes = [...graph.querySelectorAll('.route')].map(path => {
            const box = path.getBBox(), svg = path.ownerSVGElement;
            return { path: path.getAttribute('d'), marker: path.getAttribute('marker-end'), stroke: getComputedStyle(path).stroke, length: path.getTotalLength(), fits: box.x >= 0 && box.y >= 0 && box.x + box.width <= svg.width.baseVal.value && box.y + box.height <= svg.height.baseVal.value };
        });
        return { count, width: bounds.width, height: bounds.height, nodes, routes };
    }, count);
}
try {
    await server.listen();
    port = server.httpServer.address().port;
    const base = `http://127.0.0.1:${port}/tools/layout-fixtures/program-editor.html`;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    const matrix = [1366, 1920, 390].flatMap(width => [1, 1.5].flatMap(zoom => [false, true].map(native => ({ width, zoom, native, compact: false }))));
    matrix.push(...[false, true].map(native => ({ width: 1366, zoom: 1, native, compact: true })));
    matrix.push(...[false, true].flatMap(native => [
        { width: 2049, zoom: 1.5, native, compact: false },
        { width: 1250, zoom: 1, native, compact: false },
        { width: 1875, zoom: 1.5, native, compact: false },
    ]));
    for (const { width, zoom, native, compact } of matrix) {
        if (stopping) break;
        const name = `${width}-${zoom === 1 ? '100' : '150'}-${native ? 'native' : 'current'}${compact ? '-lower320' : ''}`;
        const page = await browser.newPage({ viewport: { width, height: 900 } });
        page.setDefaultTimeout(8000);
        const errors = [];
        let catalogRequests = 0;
        await page.route('**/api/v1/program-editor-catalog', async route => {
            catalogRequests += 1;
            assert.equal(route.request().headers().authorization, 'Bearer program-editor-browser-fixture');
            const catalog = JSON.parse(await page.locator('[data-program-catalog]').textContent());
            await route.fulfill({ json: { data: catalog, meta: { requestId: 'browser-catalog' } } });
        });
        page.on('pageerror', error => errors.push(error.message));
        const result = { name, width, zoom, native, compact, errors };
        results.push(result);
        try {
            const query = new URLSearchParams();
            if (native) query.set('native', '');
            if (compact) query.set('lower', '320');
            await page.goto(`${base}?${query}`);
            await page.getByRole('tab', { name: 'Effects', exact: true }).waitFor();
            assert.equal(catalogRequests, 1, 'Editor must load its catalog through the real HTTP client');
            assert.equal(await page.getByText('Program parameter catalog is unavailable.', { exact: true }).count(), 0);
            await page.evaluate(zoom => {
                document.body.style.zoom = String(zoom);
                document.documentElement.style.setProperty('--fixture-zoom', String(zoom));
            }, zoom);
            result.routingGeometry = await geometry(page, zoom);
            result.routingPanel = await graphPanelGeometry(page, zoom);
            if (width === 1366 && zoom === 1) near(result.routingGeometry.editor.width, 850, 'Realistic lower-zone width');
            if (width === 2049) near(result.routingGeometry.editor.width / zoom, 850, 'Realistic lower-zone CSS width at 150 percent');
            if (compact) near(result.routingGeometry.editor.height, 320, 'Realistic lower-zone height');
            const count = native ? 3 : 6;
            assert.equal(await page.getByRole('button', { name: /^Select Ef\d$/ }).count(), count);
            result.graph = await graphGeometry(page, count);
            assert.equal(result.graph.nodes.length, count);
            assert.ok(result.graph.nodes.every(node => node.fits), 'Graph nodes must fit the graph');
            assert.ok(result.graph.nodes.every(node => node.labels.every(label => label.textHeight <= label.height + 1)), `Graph node labels must not clip vertically: ${JSON.stringify(result.graph.nodes)}`);
            assert.equal(result.graph.routes.length, count);
            assert.ok(result.graph.routes.every(route => route.fits && route.length > 0 && route.marker && route.stroke !== 'none'), 'Graph arrows must be framed and painted');
            await page.screenshot({ path: resolve(output, `${name}-routing.png`) });
            if (compact) {
                const controls = page.locator('.graph-controls');
                assert.ok(await controls.evaluate(node => node.scrollHeight > node.clientHeight), 'Compact routing controls must scroll inside their own pane');
                await controls.evaluate(node => { node.scrollTop = node.scrollHeight; });
                const scrolled = await geometry(page, zoom);
                assert.deepEqual(scrolled.headers, result.routingGeometry.headers);
                assert.deepEqual(scrolled.footer, result.routingGeometry.footer);
                await page.screenshot({ path: resolve(output, `${name}-routing-scrolled.png`) });
                await controls.evaluate(node => { node.scrollTop = 0; });
            }

            const undo = page.getByRole('button', { name: 'Undo Program edit', exact: true });
            await page.getByRole('button', { name: 'Ef1-3 connection', exact: true }).click();
            await page.getByRole('option', { name: '1 > 2 > 3', exact: true }).click();
            assert.equal((await state(page)).values['effect_connections.1'], 2);
            result.serialGraph = await graphGeometry(page, count);
            assert.equal(result.serialGraph.routes.filter(route => route.path.includes(' H ')).length, 2);
            assert.ok(result.serialGraph.routes.every(route => route.fits && route.length > 0));
            await page.screenshot({ path: resolve(output, `${name}-serial-routing.png`) });
            await undo.click();
            if (!native) {
                await page.getByRole('button', { name: 'Output', exact: true }).click();
                const destination = page.getByRole('option', { name: 'Ef4', exact: true });
                assert.equal(await destination.locator('.extended-parameter').count(), 1, 'A5000-only destinations retain their shared marker');
                await destination.click();
                assert.equal((await state(page)).values['effects.1.destination'], 6);
                result.crossGroupGraph = await graphGeometry(page, count);
                assert.equal(result.crossGroupGraph.routes.filter(route => route.path.includes(' H ')).length, 1);
                assert.ok(result.crossGroupGraph.routes.every(route => route.fits && route.length > 0));
                await page.screenshot({ path: resolve(output, `${name}-cross-routing.png`) });
                await undo.click();
            }

            const effect = page.getByRole('combobox', { name: 'Effect type', exact: true });
            await effect.fill('AutoSyn');
            const popup = page.getByRole('listbox', { name: 'Effect type', exact: true });
            await popup.waitFor();
            result.popup = await popup.evaluate(node => {
                const box = node.getBoundingClientRect(), style = getComputedStyle(node);
                return { x: box.x, y: box.y, right: box.right, bottom: box.bottom, background: style.backgroundColor, color: style.color, border: style.borderColor, viewportWidth: innerWidth, viewportHeight: innerHeight };
            });
            assert.ok(result.popup.x >= 0 && result.popup.right <= width && result.popup.y >= 0 && result.popup.bottom <= 900, 'Dropdown must fit viewport');
            assert.notEqual(result.popup.background, 'rgba(0, 0, 0, 0)', 'Dropdown must have a theme background');
            assert.notEqual(result.popup.color, 'rgb(0, 0, 0)', 'Dropdown must inherit editor text color');
            await page.screenshot({ path: resolve(output, `${name}-dropdown.png`) });
            await effect.press('Enter');
            let current = await state(page);
            assert.equal(current.values['effects.1.type'], 2);
            for (let word = 0; word < 16; ++word) assert.equal(current.values[`effects.1.words.${word}`], 40 + word);
            await effect.fill('Scratch');
            await effect.press('Enter');
            current = await state(page);
            assert.equal(current.values['effects.1.type'], 1);
            assert.equal(current.values['effects.1.reset'], true);
            for (let word = 0; word < 16; ++word) assert.equal(current.values[`effects.1.words.${word}`], 10 + word);
            await undo.click();
            assert.equal((await state(page)).values['effects.1.type'], 2);
            await undo.click();
            current = await state(page);
            assert.equal(current.dirty, false);
            for (let word = 0; word < 16; ++word) assert.equal(current.values[`effects.1.words.${word}`], 200 + word);

            await page.getByRole('button', { name: 'Routing', exact: true }).focus();
            await page.keyboard.press('ArrowRight');
            assert.equal((await state(page)).page, 'parameters');
            assert.equal(await effect.inputValue(), '1: Scratch');
            await page.getByRole('spinbutton', { name: 'Parameter 1', exact: true }).fill('99');
            assert.deepEqual((await state(page)).changes, { 'effects.1.words.0': 99 });
            await undo.click();
            result.parameterGeometry = await geometry(page, zoom);
            await page.screenshot({ path: resolve(output, `${name}-parameters.png`) });

            await page.getByRole('tab', { name: 'Effects', exact: true }).focus();
            await page.keyboard.press('ArrowRight');
            assert.equal((await state(page)).tab, 'setup');
            assert.equal(await page.getByRole('spinbutton', { name: 'Level', exact: true }).inputValue(), '100');
            result.setupGeometry = await geometry(page, zoom);
            await page.screenshot({ path: resolve(output, `${name}-setup.png`) });
            await page.keyboard.press('End');
            assert.equal((await state(page)).tab, 'control');
            assert.equal(await page.getByRole('button', { name: 'StepWave', exact: true }).count(), native ? 0 : 1);
            if (!native) {
                await page.getByRole('button', { name: 'StepWave', exact: true }).click();
                assert.equal(await page.locator('.plot-handle').count(), 8);
                const handle = page.getByRole('button', { name: /^Step 1: / });
                await handle.focus();
                await handle.press('ArrowUp');
                assert.equal((await state(page)).values['step_wave.values.1'], 65);
                await handle.press('Shift+ArrowUp');
                assert.equal((await state(page)).values['step_wave.values.1'], 73);
                const step = page.getByRole('spinbutton', { name: 'Step 1', exact: true });
                assert.equal(await step.inputValue(), '73');
                assert.equal(await page.getByRole('spinbutton', { name: 'Step 9', exact: true }).isDisabled(), true);
                await undo.click();
                await undo.click();
                assert.equal((await state(page)).dirty, false);
                await step.fill('96');
                assert.equal((await state(page)).values['step_wave.values.1'], 96);
                assert.equal(await handle.getAttribute('aria-label'), 'Step 1: 96');
                result.stepWaveGeometry = await geometry(page, zoom);
                result.stepWaveTrace = await page.locator('.response-trace').evaluate(trace => ({ length: trace.getTotalLength(), stroke: getComputedStyle(trace).stroke }));
                assert.ok(result.stepWaveTrace.length > 0 && result.stepWaveTrace.stroke !== 'none');
                await page.screenshot({ path: resolve(output, `${name}-stepwave.png`) });
                await undo.click();
                assert.equal((await state(page)).dirty, false);
                await page.getByRole('tab', { name: 'Control', exact: true }).focus();
            }
            await page.keyboard.press('Home');
            assert.equal((await state(page)).tab, 'sample-select');
            await page.keyboard.press('ArrowRight');
            assert.equal((await state(page)).tab, 'easy-edit');
            assert.equal(await page.getByRole('combobox', { name: 'Sample/Bank', exact: true }).count(), 0, 'Sample/Bank must be a normal EditorChoice, not an autocomplete');
            const assignment = page.getByRole('button', { name: 'Sample/Bank', exact: true });
            await assignment.click();
            assert.equal(await page.getByRole('option', { name: /Duplicate \(Sample\)/ }).count(), 2);
            await page.getByRole('option', { name: 'Duplicate (Sample) · 2', exact: true }).click();
            const level = page.getByRole('spinbutton', { name: 'Level offset', exact: true });
            assert.equal(await level.inputValue(), '22');
            await level.fill('55');
            current = await state(page);
            assert.equal(current.assignment, 1);
            assert.deepEqual(current.changes, { 'assignments.1.level_offset': 55 });
            assert.equal(current.values['assignments.0.level_offset'], 11);
            await undo.click();
            assert.equal((await state(page)).dirty, false);
            result.assignmentGeometry = await geometry(page, zoom);
            await page.screenshot({ path: resolve(output, `${name}-assignment.png`) });
            await page.getByRole('button', { name: 'Amp EG', exact: true }).click();
            result.envelopePanel = await graphPanelGeometry(page, zoom);
            result.envelopeLegend = await page.locator('.envelope-graph .legend').evaluate(legend => {
                const bounds = legend.getBoundingClientRect();
                const clips = [];
                for (let parent = legend.parentElement; parent; parent = parent.parentElement) {
                    const style = getComputedStyle(parent);
                    if (!['hidden', 'clip', 'auto', 'scroll'].includes(style.overflowX)) continue;
                    const box = parent.getBoundingClientRect();
                    if (bounds.left < box.left - 1 || bounds.right > box.right + 1) clips.push(parent.className);
                }
                return { text: legend.textContent, clips };
            });
            assert.deepEqual(result.envelopeLegend.clips, [], 'Base/Easy Edit envelope legend must not be clipped by the graph toolbar');
            result.envelopeControlReachability = await envelopeControlReachability(page, zoom);
            if (result.envelopePanel.cssWidth < 740) await page.screenshot({ path: resolve(output, `${name}-envelope-controls.png`) });
            await page.locator('.graph-controls, .graph-panel').evaluateAll(nodes => nodes.forEach(node => { node.scrollTop = 0; }));
            const attack = page.getByRole('button', { name: /^Peak:.*offset 0, base 64/ });
            await attack.press('ArrowLeft');
            assert.deepEqual((await state(page)).changes, { 'assignments.1.amp_attack_offset': 1 });
            assert.equal(await page.locator('[data-trace="base-envelope"]').count(), 1);
            assert.ok((await page.locator('.envelope-graph .graph-surface').boundingBox()).height >= 150 * zoom, 'Envelope plot must retain useful vertical resolution');
            await page.screenshot({ path: resolve(output, `${name}-envelope.png`) });
            await undo.click();
            await page.getByRole('button', { name: 'Key/Velocity', exact: true }).click();
            await page.getByRole('button', { name: 'Low key limit', exact: true }).press('ArrowRight');
            assert.equal((await state(page)).values['assignments.1.key_low'], 1);
            await page.screenshot({ path: resolve(output, `${name}-ranges.png`) });
            await undo.click();
            assert.equal(await page.getByRole('button', { name: /^Keyboard view:/ }).count(), 0, 'Inline keyboard must not expose Ranges/Mapping modes');
            assert.equal(await page.getByRole('button', { name: 'Mapping Editor', exact: true }).isDisabled(), true, 'Separate mapping window is unavailable without the desktop bridge');
            assert.ok(await page.locator('.keyboard-mapping .toolbar').evaluate(toolbar => {
                const bounds = toolbar.getBoundingClientRect();
                return [...toolbar.querySelectorAll('button')].every(button => {
                    const box = button.getBoundingClientRect();
                    return box.left >= bounds.left - 1 && box.right <= bounds.right + 1;
                });
            }), 'Mapping toolbar buttons must remain visible at every scale');
            assert.equal(await page.getByRole('button', { name: 'High velocity limit', exact: true }).count(), 0, 'Inline keyboard remains a compact range view');
            assert.ok((await page.locator('.keyboard-mapping .plot').boundingBox()).height <= 64 * zoom, 'Inline range graph must remain compact');
            assert.equal((await state(page)).values['assignments.1.key_shift'], 0);
            await page.screenshot({ path: resolve(output, `${name}-compact-keyboard.png`) });
            assert.equal((await state(page)).dirty, false);
            await page.getByRole('tab', { name: 'Sample Select', exact: true }).click();
            await page.getByRole('checkbox', { name: 'Show only assigned' }).uncheck();
            await page.getByRole('button', { name: 'Receive channel assign: Available', exact: true }).click();
            await page.screenshot({ path: resolve(output, `${name}-receive-menu.png`) });
            await page.getByRole('option', { name: '=Sample', exact: true }).click();
            assert.equal((await state(page)).assignment, 2);
            await page.screenshot({ path: resolve(output, `${name}-table.png`) });
            await undo.click();
            assert.equal((await state(page)).dirty, false);
            assert.deepEqual(errors, []);
            result.passed = true;
            console.log(`${name}: geometry, graph, dropdown, keyboard, type reset/undo and duplicate ordinals passed`);
        } catch (error) {
            result.error = error.message;
            process.exitCode = 1;
            console.error(`${name}: ${error.message}`);
            await page.screenshot({ path: resolve(output, `${name}-failure.png`) }).catch(() => {});
        } finally { await page.close(); }
    }
    for (const targetCount of [200, 1000, 2048]) {
        for (const zoom of [1, 1.5]) {
            if (stopping) break;
            const name = `assignments-${targetCount}-${zoom === 1 ? '100' : '150'}`;
            const result = { name, targetCount, zoom, historicalShowAllMs: { 200: 541, 1000: 2587, 2048: 5232 }[targetCount], errors: [] };
            results.push(result);
            const page = await browser.newPage({ viewport: { width: 1366 * zoom, height: 900 } });
            page.setDefaultTimeout(8000);
            page.on('pageerror', error => result.errors.push(error.message));
            await page.route('**/api/v1/program-editor-catalog', async route => {
                const catalog = JSON.parse(await page.locator('[data-program-catalog]').textContent());
                await route.fulfill({ json: { data: catalog, meta: { requestId: 'browser-catalog' } } });
            });
            try {
                await page.goto(`${base}?targets=${targetCount}&tab=sample-select`);
                const table = page.getByRole('table', { name: 'Program assignments' });
                await table.waitFor();
                await page.evaluate(zoom => {
                    document.body.style.zoom = String(zoom);
                    document.documentElement.style.setProperty('--fixture-zoom', String(zoom));
                }, zoom);
                const start = await page.evaluate(() => performance.now());
                await page.getByRole('checkbox', { name: 'Show only assigned' }).uncheck();
                await page.waitForFunction(count => document.querySelector('[aria-label="Program assignments"]')?.getAttribute('aria-rowcount') === String(count + 2), targetCount);
                await painted(page);
                result.showAllMs = Math.round(await page.evaluate(() => performance.now()) - start);
                const metrics = () => table.evaluate(node => {
                    const rows = [...node.querySelectorAll('tbody tr[data-row-index]')];
                    const declaredRows = Number(node.getAttribute('aria-rowcount'));
                    const spacerHeight = [...node.querySelectorAll('tbody .spacer')].reduce((sum, row) => sum + row.getBoundingClientRect().height, 0);
                    return { rows: rows.length, elements: node.querySelectorAll('*').length, declaredRows, heights: rows.map(row => row.getBoundingClientRect().height), virtualRowHeight: spacerHeight / (declaredRows - 1 - rows.length) };
                });
                result.initial = await metrics();
                assert.ok(result.initial.rows <= 80, 'Large assignment lists must keep a bounded number of mounted rows');
                assert.ok(result.initial.elements <= 10000, 'Large assignment lists must keep a bounded DOM');
                assert.ok(result.initial.heights.every(height => Math.abs(height - result.initial.virtualRowHeight) < 1), `Mounted row heights must match virtual spacer geometry: ${JSON.stringify(result.initial)}`);
                const first = table.locator('[data-row-index="0"] .target');
                const lastIndex = targetCount;
                await first.focus();
                await first.press('End');
                await page.waitForFunction(index => document.activeElement?.closest('[data-row-index]')?.getAttribute('data-row-index') === String(index), lastIndex);
                const last = table.locator(`[data-row-index="${lastIndex}"] .target`);
                assert.equal(await last.getAttribute('aria-label'), `Target ${String(targetCount).padStart(4, '0')} Sample`);
                await last.press('Home');
                await page.waitForFunction(() => document.activeElement?.closest('[data-row-index]')?.getAttribute('data-row-index') === '0');
                await first.press('Tab');
                assert.equal(await page.locator(':focus').getAttribute('aria-label'), 'Receive channel assign: Duplicate (1)');
                await page.keyboard.press('Tab');
                await page.waitForFunction(() => document.activeElement?.closest('[data-row-index]')?.getAttribute('data-row-index') === '1' && document.activeElement?.classList.contains('target'));
                await page.keyboard.press('Shift+Tab');
                assert.equal(await page.locator(':focus').getAttribute('aria-label'), 'Receive channel assign: Duplicate (1)');
                const mountedBoundary = await table.locator('tbody tr[data-row-index]').evaluateAll(rows => Math.max(...rows.map(row => Number(row.getAttribute('data-row-index')))));
                assert.ok(mountedBoundary < lastIndex, 'Initial viewport leaves unmounted rows for the Tab boundary check');
                await table.locator(`[data-row-index="${mountedBoundary}"] button`).last().evaluate(node => node.focus({ preventScroll: true }));
                await page.keyboard.press('Tab');
                await page.waitForFunction(index => document.activeElement?.closest('[data-row-index]')?.getAttribute('data-row-index') === String(index) && document.activeElement?.classList.contains('target'), mountedBoundary + 1);
                await page.keyboard.press('Home');
                await page.waitForFunction(() => document.activeElement?.closest('[data-row-index]')?.getAttribute('data-row-index') === '0');
                await first.focus();
                await page.locator('.assignment-scroll').evaluate(node => { node.scrollTop = node.scrollHeight; });
                await painted(page);
                assert.equal(await first.evaluate(node => document.activeElement === node), true, 'Scrolling must not unmount the focused row');
                result.scrolled = await metrics();
                assert.ok(result.scrolled.rows <= 80, 'Focused offscreen rows must not disable virtualization');
                assert.ok(result.scrolled.heights.every(height => Math.abs(height - result.scrolled.virtualRowHeight) < 1), 'Scrolling must preserve fixed virtual row geometry');
                await page.screenshot({ path: resolve(output, `${name}-scrolled.png`) });
                await first.press('Home');
                await first.press('PageDown');
                await page.waitForFunction(() => Number(document.activeElement?.closest('[data-row-index]')?.getAttribute('data-row-index')) > 0);
                const search = page.getByRole('searchbox', { name: 'Search Assignments' });
                const searchStart = await page.evaluate(() => performance.now());
                await search.fill(`Target ${String(targetCount).padStart(4, '0')}`);
                await page.waitForFunction(() => document.querySelector('[aria-label="Program assignments"]')?.getAttribute('aria-rowcount') === '2');
                await painted(page);
                result.searchMs = Math.round(await page.evaluate(() => performance.now()) - searchStart);
                assert.equal(await table.locator('tbody tr[data-row-index]').count(), 1);
                await page.screenshot({ path: resolve(output, `${name}-search.png`) });
                await search.fill('No matching assignment');
                await page.getByText('No matching Samples or Sample Banks', { exact: true }).waitFor();
                await search.fill('');
                await page.waitForFunction(count => document.querySelector('[aria-label="Program assignments"]')?.getAttribute('aria-rowcount') === String(count + 2), targetCount);
                assert.equal(await page.locator('.assignment-scroll').evaluate(node => node.scrollTop), 0, 'Clearing search returns to the first rows');
                await page.screenshot({ path: resolve(output, `${name}-table.png`) });
                assert.deepEqual(result.errors, []);
                result.passed = true;
                console.log(`${name}: show-all ${result.showAllMs} ms, search ${result.searchMs} ms, ${result.initial.rows} mounted rows`);
            } catch (error) {
                result.error = error.message;
                process.exitCode = 1;
                console.error(`${name}: ${error.message}`);
                await page.screenshot({ path: resolve(output, `${name}-failure.png`) }).catch(() => {});
            } finally { await page.close(); }
        }
    }
} finally {
    clearTimeout(deadline);
    process.removeListener('SIGTERM', signal);
    process.removeListener('SIGINT', signal);
    await stop();
    const portReleased = await new Promise(resolve => {
        const socket = createConnection({ host: '127.0.0.1', port });
        socket.once('connect', () => { socket.destroy(); resolve(false); });
        socket.once('error', () => resolve(true));
    });
    await writeFile(resolve(output, 'results.json'), JSON.stringify({ port, serverStopped: true, browserStopped: !browser?.isConnected(), portReleased, results }, null, 2) + '\n');
    assert.equal(portReleased, true, 'Owned Vite port must be released');
}
