import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const base = process.argv[2] ?? 'http://127.0.0.1:5191';
const output = resolve(process.argv[3] ?? '../../../build/logs/sample-editor/00006');
await mkdir(output, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const [width, height, zoom, dpr, workspace] of [[1600,900,1,1,true], [1920,1080,1.25,1,true], [1920,1080,1.5,2,true], [1100,720,1,1,true], [800,480,1,1,false], [390,640,1,2,false]]) {
        console.log(`Graph layout: ${width}x${height}, scale ${zoom}, DPR ${dpr}, workspace ${workspace}`);
        const page = await browser.newPage({ viewport: { width, height }, deviceScaleFactor: dpr });
        page.setDefaultTimeout(10000);
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        await page.goto(`${base}/tools/layout-fixtures/sample-editor.html${workspace ? '?workspace' : ''}`);
        await page.evaluate(({ zoom, workspace }) => {
            document.documentElement.style.zoom = String(zoom);
            document.querySelector(workspace ? '.workspace-fixture' : 'main').style.height = `${innerHeight / zoom - 40}px`;
        }, { zoom, workspace });
        if (workspace) await page.getByRole('button', { name: 'Editor panel', exact: true }).click();
        const save = page.getByRole('button', { name: 'Save', exact: true });
        await save.waitFor();
        const dock = await page.locator('.device-editor').boundingBox();
        if (workspace) {
            const stage = await page.locator('.main-stage').boundingBox();
            const available = stage.height / zoom - 4;
            const expected = Math.max(Math.min(180,available/2), Math.min(available - Math.min(180,available/2), Math.max(360,available/3)));
            assert(Math.abs(dock.height / zoom - expected) < 2, 'workspace uses usable default editor height');
        }
        const snap = async name => page.screenshot({ path: resolve(output, `${name}-${width}-${zoom}-${dpr}.png`) });
        await snap('workspace-default');
        await page.getByRole('tab', { name: 'Map/Out', exact: true }).click();
        await page.getByRole('button', { name: 'Level scaling', exact: true }).click();
        const panel = page.locator('.graph-panel');
        const panelWidth = await panel.evaluate(node => node.clientWidth);
        const divider = page.getByRole('separator', { name: 'Resize graph and controls' });
        assert.equal(await divider.isVisible(), panelWidth >= 900, 'split follows editor width, not window width');
        const keyGeometry = await page.locator('.keyboard').evaluate(node => {
            const svg = node.querySelector('svg').getBoundingClientRect();
            const surface = node.closest('.graph-frame').querySelector('.breakpoint-graph').getBoundingClientRect();
            const scale = surface.width / node.closest('.graph-frame').querySelector('.breakpoint-graph').clientWidth;
            return { left: svg.left, right: svg.right, expectedLeft: surface.left + 10 * scale, expectedRight: surface.right - 10 * scale, blackHeights: [...node.querySelectorAll('rect.black')].map(key => key.getBoundingClientRect().height) };
        });
        assert(Math.abs(keyGeometry.left - keyGeometry.expectedLeft) < 1);
        assert(Math.abs(keyGeometry.right - keyGeometry.expectedRight) < 1);
        assert(keyGeometry.blackHeights.every(h => Math.abs(h - keyGeometry.blackHeights[0]) < 0.1));
        await snap('keyboard');
        let resized = null;
        if (await divider.isVisible()) {
            await divider.focus(); await divider.press('ArrowRight');
            resized = await divider.getAttribute('aria-valuenow');
            assert(Number(resized) > 50);
            assert(await save.isDisabled(), 'layout changes do not dirty the Sample');
            const grip = await divider.boundingBox();
            await page.mouse.move(grip.x + grip.width / 2, grip.y + grip.height / 2);
            await page.mouse.down();
            await page.mouse.move(grip.x + grip.width / 2 + 25, grip.y + grip.height / 2, { steps: 4 });
            await page.mouse.up();
            resized = await divider.getAttribute('aria-valuenow');
            await page.setViewportSize({ width: 1100, height });
            await divider.waitFor({ state: 'hidden' });
            await page.setViewportSize({ width, height });
            await divider.waitFor({ state: 'visible' });
            assert.equal(await divider.getAttribute('aria-valuenow'), resized, 'responsive stacking preserves the chosen ratio');
        }
        await page.getByRole('tab', { name: 'Filter', exact: true }).click();
        await page.getByRole('button', { name: 'Filter scaling', exact: true }).click();
        if (resized) assert.equal(await divider.getAttribute('aria-valuenow'), resized);
        if (panelWidth < 900) await page.getByRole('button', { name: 'Velocity', exact: true }).click();
        const mode = page.getByRole('button', { name: 'Velocity to cutoff mode', exact: true });
        await mode.click();
        const popup = await page.getByRole('listbox', { name: 'Velocity to cutoff mode' }).boundingBox();
        assert(popup.x >= 0 && popup.y >= 0 && popup.x + popup.width <= width + 1 && popup.y + popup.height <= height + 1);
        await page.keyboard.press('Escape');
        await snap('filter-scaling');
        await page.getByRole('tab', { name: 'EG', exact: true }).click();
        await snap('amplitude');
        await page.getByRole('button', { name: 'Attack mode: Hold', exact: true }).click();
        assert(await page.getByRole('button', { name: /^Hold:/ }).isVisible());
        assert((await page.locator('[data-trace="envelope"]').getAttribute('d')).startsWith('M0,0'));
        await page.getByRole('button', { name: 'Undo Sample edit' }).click();
        for (const name of ['Filter', 'Pitch']) {
            await page.getByRole('button', { name, exact: true }).click();
            const first = page.getByRole('button', { name: /^Initial:/ });
            const last = page.getByRole('button', { name: /^Release:/ });
            const bounds = await page.locator('.parameter-plot').boundingBox();
            const firstBox = await first.boundingBox(), lastBox = await last.boundingBox();
            assert(Math.abs(firstBox.x + firstBox.width / 2 - bounds.x) < 1);
            assert(Math.abs(lastBox.x + lastBox.width / 2 - bounds.x - bounds.width) < 1);
            await first.focus(); await first.press('ArrowUp');
            assert.equal(await page.getByRole('spinbutton', { name: 'Init level', exact: true }).inputValue(), '1');
            await page.getByRole('button', { name: 'Undo Sample edit' }).click();
            assert(await save.isDisabled());
            const before = await page.locator('.graph-region').boundingBox();
            await page.locator('.graph-controls').evaluate(node => node.scrollTop = node.scrollHeight);
            assert.deepEqual(await page.locator('.graph-region').boundingBox(), before, 'controls scroll independently');
            await snap(`${name.toLowerCase()}-envelope`);
        }
        await page.getByRole('tab', { name: 'LFO', exact: true }).click();
        assert.equal(await page.locator('.graph-controls').evaluate(node => node.scrollTop), 0, 'new pages open at the first controls');
        await snap('lfo');
        const initial = await page.locator('.reference').getAttribute('d');
        await page.getByRole('button', { name: 'Wave: Square' }).click();
        assert.notEqual(await page.locator('.reference').getAttribute('d'), initial);
        await page.getByRole('button', { name: 'Undo Sample edit' }).click();
        if (panelWidth < 900) await page.getByRole('button', { name: 'Depth', exact: true }).click();
        await page.getByRole('spinbutton', { name: 'Pitch depth', exact: true }).fill('90');
        assert(!(await save.isDisabled()));
        assert.equal(await page.getByLabel('Write count').textContent(), '0');
        await page.getByRole('button', { name: 'Undo Sample edit' }).click();
        await page.getByRole('button', { name: 'Show Amplitude modulation' }).click();
        assert(await save.isDisabled());
        if (workspace) {
            const lowerDivider = page.getByRole('separator', { name: 'Resize editor panel' });
            await lowerDivider.focus(); await lowerDivider.press('ArrowUp');
            const resizedDock = await page.locator('.device-editor').boundingBox();
            assert(resizedDock.height > dock.height);
            await page.getByRole('button', { name: 'Editor panel', exact: true }).click();
            await page.getByRole('button', { name: 'Editor panel', exact: true }).click();
            await save.waitFor();
            assert(Math.abs((await page.locator('.device-editor').boundingBox()).height - resizedDock.height) < 1);
            await page.getByRole('button', { name: 'Select B', exact: true }).click();
            await page.waitForFunction(() => document.querySelector('.device-editor header strong')?.textContent === 'Sample B');
            assert(Math.abs((await page.locator('.device-editor').boundingBox()).height - resizedDock.height) < 1, 'selection changes preserve the manual pane height');
        }
        const footer = await page.locator('.sample-transport').boundingBox();
        assert(footer.y + footer.height <= height + 1, 'transport remains inside the viewport');
        assert(await page.evaluate(() => document.documentElement.scrollWidth <= document.documentElement.clientWidth), 'no horizontal document overflow');
        assert.deepEqual(errors, []);
        results.push({ width, height, zoom, dpr, workspace, dock, panelWidth, keyGeometry, errors });
        await page.close();
    }
    await writeFile(resolve(output, 'graph-results.json'), JSON.stringify(results, null, 2) + '\n');
} finally { await browser.close(); }
