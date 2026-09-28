import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const output = resolve(process.argv[2] ?? '../../../build/logs/sample-editor/00027/waveform-transition');
await mkdir(output, { recursive: true });
const server = await createServer({ server: { host: '127.0.0.1', port: 0, strictPort: true } });
let browser;
let port;
const results = [];
let interrupted = false;
const stop = async () => {
    interrupted = true;
    await browser?.close();
    await server.close();
};
const deadline = setTimeout(() => void stop(), 240000);
const onSignal = () => void stop();
process.once('SIGINT', onSignal);
process.once('SIGTERM', onSignal);

async function frame(page) {
    await page.evaluate(() => new Promise((done) => requestAnimationFrame(() => requestAnimationFrame(done))));
}

async function pixels(page, testId) {
    return page.getByTestId(testId).locator('canvas').evaluate((canvas) => {
        const data = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
        let top = canvas.height;
        let bottom = -1;
        let count = 0;
        for (let y = 0; y < canvas.height; y++) for (let x = 0; x < canvas.width; x++) {
            if (data[(y * canvas.width + x) * 4 + 3] <= 128) continue;
            top = Math.min(top, y);
            bottom = Math.max(bottom, y);
            count++;
        }
        const box = canvas.getBoundingClientRect();
        return { top, bottom, count, width: canvas.width, height: canvas.height, cssWidth: box.width, cssHeight: box.height };
    });
}

async function geometry(page) {
    return page.locator('.device-editor').evaluate((host) => {
        const box = (node) => {
            const rect = node.getBoundingClientRect();
            return [rect.x, rect.y, rect.width, rect.height];
        };
        return {
            host: box(host),
            header: box(host.querySelector('header')),
            panel: box(host.querySelector('[role="tabpanel"]')),
            ...Object.fromEntries([...host.querySelectorAll('[role="tab"]')].map((tab) => [`tab-${tab.textContent}`, box(tab)])),
            badgeSlot: box(host.querySelector('.format-slot')),
        };
    });
}

function sameGeometry(before, after, label) {
    for (const key of Object.keys(before)) before[key].forEach((value, index) => {
        assert(Math.abs(value - after[key][index]) <= 1, `${label}: ${key}[${index}] moved from ${value} to ${after[key][index]}`);
    });
}

try {
    await server.listen();
    port = server.httpServer.address().port;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    for (const width of [390, 800, 849, 850, 1600]) for (const dpr of [1, 1.25, 1.5, 2]) {
        if (interrupted) throw new Error('Browser regression interrupted or exceeded 240 seconds');
        const page = await browser.newPage({ viewport: { width, height: 800 }, deviceScaleFactor: dpr });
        page.setDefaultTimeout(10000);
        const errors = [];
        page.on('pageerror', (error) => errors.push(error.message));
        const name = `${width}-dpr-${dpr}`;
        try {
            await page.goto(`http://127.0.0.1:${port}/tools/layout-fixtures/waveform-transition.html`);
            await page.getByRole('group', { name: 'Sample: A', exact: true }).waitFor();
            await frame(page);
            const binPixels = {};
            for (const testId of ['amplitude-plot', 'amplitude-overview']) {
                const value = await pixels(page, testId);
                assert.equal(value.width, Math.round(value.cssWidth * dpr));
                assert.equal(value.height, Math.round(value.cssHeight * dpr));
                const inset = testId === 'amplitude-plot' ? 18 * dpr : 0;
                assert(value.count > value.width * 3, `${testId} must not be blank or just a centerline`);
                assert(Math.abs(value.bottom - value.top + 1 - (value.height - inset) * 0.25) <= 2, `${testId} must retain quarter-scale amplitude`);
                binPixels[testId] = value;
            }
            await page.getByRole('button', { name: 'Native PCM', exact: true }).click();
            await frame(page);
            for (const testId of Object.keys(binPixels)) {
                const pcm = await pixels(page, testId);
                assert.deepEqual(pcm, binPixels[testId], `${testId} changed amplitude or dimensions with equivalent native PCM`);
            }
            const before = await geometry(page);
            await page.locator('[role="tabpanel"]').evaluate((panel) => {
                window.retainedPanel = panel;
                window.transitionFrames = [];
                window.observeTransition = true;
                const record = () => {
                    const host = document.querySelector('.device-editor');
                    window.transitionFrames.push({
                        name: host?.querySelector('.editor-content')?.getAttribute('aria-label') ?? '',
                        preview: [...(host?.querySelectorAll('.lane-role') ?? [])].map((node) => node.textContent.trim()),
                        panel: Boolean(host?.querySelector('[role="tabpanel"]')),
                    });
                    if (window.observeTransition) requestAnimationFrame(record);
                };
                requestAnimationFrame(record);
            });
            await page.getByRole('button', { name: 'Select B', exact: true }).click();
            await page.locator('.device-editor[aria-busy="true"]').waitFor();
            await frame(page);
            assert(await page.evaluate(() => window.retainedPanel.isConnected));
            assert(await page.evaluate(() => Boolean(window.retainedPanel.closest('[inert]'))));
            sameGeometry(before, await geometry(page), 'Pending waveform');
            await page.screenshot({ path: resolve(output, `${name}-pending.png`) });
            await page.getByRole('button', { name: 'Complete load', exact: true }).click();
            await page.getByRole('group', { name: 'Sample: Long Sample Name', exact: true }).waitFor();
            await frame(page);
            const frames = await page.evaluate(() => { window.observeTransition = false; return window.transitionFrames; });
            assert(frames.length >= 3);
            for (const value of frames) {
                assert(value.panel, 'A rendered frame lost the editor panel');
                assert(['Sample: A', 'Sample: Long Sample Name'].includes(value.name), 'A rendered frame lost the document name');
                assert.deepEqual(value.preview, value.name === 'Sample: A' ? ['A left', 'A right'] : ['B left', 'B right'], 'A rendered frame mixed document and waveform identities');
            }
            sameGeometry(before, await geometry(page), 'Completed waveform');
            await page.getByRole('tab', { name: 'Map/Out', exact: true }).click();
            await page.getByRole('button', { name: 'Pitch', exact: true }).click();
            await page.getByRole('spinbutton', { name: 'Coarse tune', exact: true }).evaluate((input) => { window.retainedInput = input; });
            const pitchBefore = await geometry(page);
            await page.getByRole('button', { name: 'Select C', exact: true }).click();
            await page.locator('.device-editor[aria-busy="true"]').waitFor();
            assert(await page.evaluate(() => window.retainedInput.isConnected));
            assert(await page.evaluate(() => { window.retainedInput.focus(); return document.activeElement !== window.retainedInput; }), 'Pending controls must not accept focus');
            sameGeometry(pitchBefore, await geometry(page), 'Pending Pitch');
            await page.getByRole('button', { name: 'Complete load', exact: true }).click();
            await page.getByRole('group', { name: 'Sample: C', exact: true }).waitFor();
            assert.equal(await page.getByRole('tab', { name: 'Map/Out', exact: true }).getAttribute('aria-selected'), 'true');
            assert.equal(await page.getByRole('button', { name: 'Pitch', exact: true }).getAttribute('aria-pressed'), 'true');
            sameGeometry(pitchBefore, await geometry(page), 'Completed Pitch');
            await page.getByRole('tab', { name: 'EG', exact: true }).click();
            await frame(page);
            const graphBefore = await page.locator('.graph-panel').evaluate((panel) => {
                const snapshot = () => {
                    const graph = document.querySelector('.graph-panel');
                    return {
                        stacked: graph?.classList.contains('stacked'),
                        columns: graph ? getComputedStyle(graph).gridTemplateColumns : '',
                        fields: [...(graph?.querySelectorAll('.parameter-field') ?? [])].map((field) => {
                            const rect = field.getBoundingClientRect();
                            return [rect.x, rect.y, rect.width, rect.height];
                        }),
                    };
                };
                window.graphFrames = [];
                window.observeGraph = true;
                window.retainedGraph = panel;
                const record = () => {
                    window.graphFrames.push(snapshot());
                    if (window.observeGraph) requestAnimationFrame(record);
                };
                requestAnimationFrame(record);
                return snapshot();
            });
            await page.getByRole('button', { name: 'Select D', exact: true }).click();
            await page.locator('.device-editor[aria-busy="true"]').waitFor();
            await frame(page);
            assert(await page.evaluate(() => window.retainedGraph.isConnected));
            await page.getByRole('button', { name: 'Complete load', exact: true }).click();
            await page.getByRole('group', { name: 'Sample: Envelope Sample', exact: true }).waitFor();
            await frame(page);
            const graphFrames = await page.evaluate(() => { window.observeGraph = false; return window.graphFrames; });
            assert(graphFrames.length >= 3);
            assert(graphBefore.fields.length > 0, 'EG parameter layout was not exercised');
            graphFrames.forEach((value) => assert.deepEqual(value, graphBefore, 'EG graph or parameter layout changed during a Sample transition'));
            assert.equal(await page.getByRole('tab', { name: 'EG', exact: true }).getAttribute('aria-selected'), 'true');
            const headerBefore = await geometry(page);
            const header = page.locator('.device-editor header');
            assert(!await header.locator('strong, .family').count(), 'Redundant header identity is still visible');
            const tabBox = await page.getByRole('tab', { name: 'Trim/Loop', exact: true }).boundingBox();
            assert.equal(tabBox.x, headerBefore.host[0] + 8, 'Tabs must start at the content inset');
            const actionBox = await header.locator('.actions').boundingBox();
            if (width < 850) assert(actionBox.y >= tabBox.y + tabBox.height, 'Narrow tabs must be in the first row');
            else assert(actionBox.y < tabBox.y + tabBox.height, 'Wide header must remain one row');
            await page.getByRole('button', { name: 'Compare', exact: true }).click();
            await frame(page);
            sameGeometry(headerBefore, await geometry(page), 'Comparison status');
            assert.equal(await header.locator('.comparison').getAttribute('title'), 'Editing Envelope Sample only');
            await page.getByRole('button', { name: 'Edit', exact: true }).click();
            await page.getByRole('group', { name: 'Sample: Envelope Sample (unsaved changes)', exact: true }).waitFor();
            sameGeometry(headerBefore, await geometry(page), 'Dirty Sample');
            await page.getByRole('button', { name: 'Discard', exact: true }).click();
            await page.getByRole('button', { name: 'Complete load', exact: true }).click();
            await page.getByRole('group', { name: 'Sample: Envelope Sample', exact: true }).waitFor();
            await page.getByRole('button', { name: 'Compare', exact: true }).click();
            const fixedHeader = Object.fromEntries(Object.entries(headerBefore).filter(([key]) => key !== 'panel'));
            for (const [id, name, format] of [['E', 'B', 'a3k'], ['F', 'Long Bank Name', 'a4k/a5k']]) {
                await page.getByRole('button', { name: `Select ${id}`, exact: true }).click();
                await page.locator('.device-editor[aria-busy="true"]').waitFor();
                sameGeometry(fixedHeader, await geometry(page), 'Pending Bank');
                await page.getByRole('button', { name: 'Complete load', exact: true }).click();
                await page.getByRole('group', { name: `Sample Bank: ${name}`, exact: true }).waitFor();
                await frame(page);
                sameGeometry(fixedHeader, await geometry(page), 'Completed Bank');
                assert.equal((await header.locator('.format-badge').textContent()).trim(), format);
                await page.getByRole('tab', { name: 'EG', exact: true }).click();
            }
            assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'Horizontal viewport overflow');
            assert.deepEqual(errors, []);
            results.push({ name, passed: true, binPixels, transitionFrames: frames.length, graphFrames: graphFrames.length });
            console.log(`PASS ${name}`);
        } catch (error) {
            results.push({ name, passed: false, error: error.message, errors });
            console.error(`FAIL ${name}: ${error.message}`);
            process.exitCode = 1;
        } finally {
            if (!page.isClosed()) {
                await page.screenshot({ path: resolve(output, `${name}-final.png`) });
                await page.close();
            }
        }
    }
} finally {
    clearTimeout(deadline);
    process.removeListener('SIGINT', onSignal);
    process.removeListener('SIGTERM', onSignal);
    await browser?.close();
    await server.close();
    await writeFile(resolve(output, 'results.json'), JSON.stringify({ port, serverStopped: true, results }, null, 2) + '\n');
}
