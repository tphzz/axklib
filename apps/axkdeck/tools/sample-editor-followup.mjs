import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
const { chromium } = await import(pathToFileURL(process.env.PLAYWRIGHT_MODULE).href);
const base = process.argv[2] ?? 'http://127.0.0.1:5191';
const output = resolve(process.argv[3] ?? '../../../build/logs/sample-editor/00010');
await mkdir(output, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const zoom of [1, 1.5]) {
        for (const scenario of ['pitch-gap', 'dirty-tabs', 'envelope-edges', 'eq-response', 'settings-metadata', 'popup-filtering']) {
            const page = await browser.newPage({ viewport: { width: 2400, height: 800 } });
            const name = `${scenario}-${zoom}`;
            const errors = [];
            page.on('pageerror', error => errors.push(error.message));
            try {
                await page.goto(`${base}/tools/layout-fixtures/sample-editor.html?short-stereo`);
                await page.getByRole('button', { name: 'Save', exact: true }).waitFor();
                await page.evaluate(zoom => {
                    document.documentElement.style.zoom = String(zoom);
                    document.querySelector('main').style.height = `${innerHeight / zoom - 40}px`;
                }, zoom);
                const navigate = async (tab, sub) => {
                    await page.getByRole('tab', { name: tab, exact: true }).click();
                    await page.getByLabel('Sample subpages', { exact: true }).getByRole('button', { name: sub, exact: true }).click();
                };
                const set = async (name, value) => {
                    const input = page.getByRole('spinbutton', { name, exact: true });
                    await input.fill(String(value)); await input.press('Tab');
                };
                if (scenario === 'pitch-gap') {
                    await navigate('Map/Out', 'Pitch');
                    const groups = page.locator('.parameter-groups section');
                    const portamento = await groups.nth(1).boundingBox();
                    const tuning = await groups.nth(0).boundingBox();
                    assert.equal(await groups.count(), 2, 'Pitch contains Tuning and Portamento only');
                    assert(portamento.x - tuning.x - tuning.width <= 13 * zoom, 'Portamento follows Tuning without a gap');
                } else if (scenario === 'dirty-tabs') {
                    await navigate('Map/Out', 'Pitch');
                    const tabs = async () => page.locator('.navigation [role=tab]').evaluateAll(nodes => nodes.map(node => node.getBoundingClientRect().x));
                    const before = await tabs();
                    await set('Coarse tune', 3);
                    assert.deepEqual(await tabs(), before, 'dirty marker must not move the tabs');
                    await page.getByRole('button', { name: 'Undo Sample edit' }).click();
                    assert.deepEqual(await tabs(), before);
                } else if (scenario === 'envelope-edges') {
                    for (const kind of ['Amplitude', 'Filter', 'Pitch']) {
                        await navigate('EG', kind);
                        if (kind === 'Amplitude') await set('Sustain level', 127);
                        if (kind === 'Pitch') {
                            await set('Attack level', 39);
                            await set('Sustain level', -54);
                        }
                        for (const name of ['Attack rate', 'Decay rate', 'Release rate']) await set(name, 127);
                        await page.getByRole('button', { name: 'Fit envelope to width' }).click();
                        const attack = page.locator('[data-handle="1"]');
                        assert(await attack.evaluate(node => parseFloat(node.style.left) < 1), 'maximum attack should be near vertical');
                        if (kind === 'Amplitude') {
                            await page.screenshot({ path: resolve(output, `maximum-attack-${zoom}.png`) });
                            assert.equal(await page.locator('[data-handle="release-rate"]').count(), 0, 'no artificial midpoint');
                            const release = await page.locator('[data-handle="4"]').boundingBox();
                            await page.mouse.move(release.x + release.width / 2, release.y + release.height / 2);
                            await page.mouse.down();
                            await page.mouse.move(release.x + release.width / 2 + 40, release.y + release.height / 2, { steps: 5 });
                            await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
                            const firstRate = Number(await page.getByRole('spinbutton', { name: 'Release rate', exact: true }).inputValue());
                            await page.mouse.move(release.x + release.width / 2 + 100, release.y + release.height / 2, { steps: 5 });
                            await page.mouse.up();
                            assert(Number(await page.getByRole('spinbutton', { name: 'Release rate', exact: true }).inputValue()) < firstRate, 'release drag continues after its endpoint leaves the viewport');
                            await page.getByRole('button', { name: 'Undo Sample edit' }).click();
                        }
                        await set('Attack rate', 126);
                        if (kind !== 'Amplitude') {
                            const end = page.locator('[data-handle="4"]');
                            assert(!(await end.isVisible()), 'offscreen release handle must not leak past plot border');
                            if (kind === 'Pitch') await page.screenshot({ path: resolve(output, `pitch-endpoint-clipped-${zoom}.png`) });
                            await page.getByRole('button', { name: 'Fit envelope to width' }).click();
                            assert(await end.isVisible(), 'Fit restores access to endpoint');
                        }
                    }
                } else if (scenario === 'settings-metadata') {
                    await navigate('Trim/Loop', 'Sample Info');
                    const metadata = page.locator('.source-metadata');
                    const header = await page.locator('.sample-info .editor-toolbar').boundingBox();
                    const box = await metadata.boundingBox();
                    assert(box.y >= header.y && box.y + box.height <= header.y + header.height, 'metadata uses the existing header row');
                    assert.equal(await page.locator('.settings-summary').count(), 0);
                    const text = await metadata.textContent();
                    await set('Loop Tempo', 110);
                    assert.equal(await metadata.textContent(), text, 'source duration is informational');
                } else if (scenario === 'popup-filtering') {
                    await navigate('MIDI/CTRL', 'Control');
                    for (const direction of ['above', 'below']) {
                        const input = page.getByRole('combobox', { name: 'Control 1 Controller', exact: true });
                        // Keep the fixture geometry deterministic without changing application styles.
                        await page.evaluate(({ direction, zoom }) => {
                            const main = document.querySelector('main');
                            main.style.marginTop = direction === 'above' ? `${420 / zoom}px` : '0';
                            main.style.height = `${(innerHeight - (direction === 'above' ? 420 : 0)) / zoom - 40}px`;
                        }, { direction, zoom });
                        await input.click();
                        const check = async () => {
                            await page.waitForTimeout(80);
                            const anchor = await input.boundingBox(), popup = await page.getByRole('listbox').boundingBox();
                            const gap = direction === 'above' ? anchor.y - popup.y - popup.height : popup.y - anchor.y - anchor.height;
                            assert(Math.abs(gap - 4) <= 1, `${direction} popup remains attached: gap ${gap}`);
                            assert(popup.y >= 0 && popup.y + popup.height <= 801, 'popup stays in viewport');
                        };
                        await check();
                        for (const query of ['071', 'no matches here', '']) { await input.fill(query); await check(); }
                        await input.fill('071'); await check();
                        await page.screenshot({ path: resolve(output, `popup-${direction}-${zoom}.png`) });
                        await input.press('Escape');
                        assert.equal(await page.getByRole('listbox').count(), 0);
                    }
                } else {
                    await navigate('Filter', 'Sample EQ');
                    await set('EQ frequency selection', 10);
                    await set('EQ gain (dB)', -4);
                    await set('EQ width', 1);
                    const trace = page.locator('[data-trace="eq"]');
                    const d = await trace.getAttribute('d');
                    const values = [...d.matchAll(/[ML]([^,]+),([^ ]+)/g)].map(match => Number(match[2]));
                    assert(Math.abs(Math.max(...values) - (22 / 36) * 200) < 0.1, '63Hz peak dip remains at nominal -4dB');
                    const overlay = page.getByRole('button', { name: 'Show coefficient response' });
                    assert.equal(await page.locator('[data-trace="coefficients"]').count(), 0);
                    await overlay.click();
                    assert.equal(await page.locator('[data-trace="coefficients"]').count(), 1);
                    assert.equal(await trace.getAttribute('d'), d, 'coefficient overlay must not move the editing curve');
                    await overlay.click();
                }
                assert.deepEqual(errors, []);
                await page.screenshot({ path: resolve(output, `${name}.png`) });
                results.push({ name, status: 'passed' }); console.log(`PASS ${name}`);
            } catch (error) {
                results.push({ name, status: 'failed', error: error.message }); console.error(`FAIL ${name}: ${error.message}`);
                await page.screenshot({ path: resolve(output, `${name}-failure.png`) }); process.exitCode = 1;
            } finally { await page.close(); }
        }
    }
} finally {
    await writeFile(resolve(output, 'followup-results.json'), JSON.stringify(results, null, 2) + '\n');
    await browser.close();
}
