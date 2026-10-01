import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const base = process.argv[2] ?? 'http://127.0.0.1:5191';
const output = resolve(process.argv[3] ?? '../../../build/logs/sample-editor/00009');
await mkdir(output, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const width of [390, 800, 1000, 1600]) {
        const page = await browser.newPage({ viewport: { width, height: 700 } });
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        const name = `compact-${width}`;
        try {
            await page.goto(`${base}/tools/layout-fixtures/sample-editor.html?short-stereo`);
            await page.getByRole('button', { name: 'Save', exact: true }).waitFor();
            await page.locator('main').evaluate(node => { node.style.height = '360px'; });
            const subpage = async (tab, sub) => {
                await page.getByRole('tab', { name: tab, exact: true }).click();
                await page.getByLabel('Sample subpages', { exact: true }).getByRole('button', { name: sub, exact: true }).click();
                await page.waitForTimeout(100);
            };
            for (const sub of ['Mix & Key', 'Pitch']) {
                await subpage('Map/Out', sub);
                const geometry = await page.locator('.parameter-groups').evaluate(node => ({
                    columns: Number(node.dataset.columns),
                    sections: [...node.children].map(child => { const r = child.getBoundingClientRect(); return { x: r.x, y: r.y, w: r.width }; }),
                    overflow: node.scrollWidth > node.clientWidth,
                }));
                assert(!geometry.overflow);
                assert(geometry.sections.every(r => r.w <= 380));
                const [a, b, c] = geometry.sections;
                if (geometry.columns === 3) { assert(a.x < b.x && b.x < c.x); assert.equal(a.y, c.y); }
                if (geometry.columns === 2 && c) {
                    assert(a.x < b.x);
                    assert.equal(c.x, sub === 'Pitch' ? b.x : a.x);
                    assert(c.y > a.y);
                }
                await page.screenshot({ path: resolve(output, `${name}-${sub.replaceAll(' ', '-')}.png`) });
            }
            const conversion = page.locator('.attribute-help-label[aria-label="Portamento type"]');
            await conversion.focus();
            assert.match(await page.getByRole('tooltip').textContent(), /full parameter layout/);
            await page.keyboard.press('Escape');
            await subpage('Map/Out', 'Level scaling');
            const header = await page.locator('.graph-readout').boundingBox();
            const plot = await page.locator('.graph-surface').boundingBox();
            assert.equal(header.height, 26);
            assert(plot.y - header.y < 36, 'only one header row precedes the graph');
            if (width >= 1000) {
                const divider = page.getByRole('separator', { name: 'Resize graph and controls' });
                assert.equal((await divider.boundingBox()).width, 8);
                const line = await divider.evaluate(node => {
                    const style = getComputedStyle(node, '::before');
                    return { width: style.width, border: getComputedStyle(node).borderLeftWidth };
                });
                assert.deepEqual(line, { width: '1px', border: '0px' });
                await divider.press('ArrowLeft');
                assert(Number(await divider.getAttribute('aria-valuenow')) < 50);
            }
            await subpage('Filter', 'Sample EQ');
            const handle = page.locator('[data-handle="frequency-gain"]');
            const eqWidth = page.getByRole('spinbutton', { name: 'EQ width', exact: true });
            const frequency = page.getByRole('spinbutton', { name: 'EQ frequency selection', exact: true });
            const gain = page.getByRole('spinbutton', { name: 'EQ gain (dB)', exact: true });
            const before = [await frequency.inputValue(), await gain.inputValue()];
            const drag = async (modifier, dx, dy) => {
                const box = await handle.boundingBox();
                await page.keyboard.down(modifier);
                await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
                await page.mouse.down();
                await page.mouse.move(box.x + box.width / 2 + dx, box.y + box.height / 2 + dy, { steps: 15 });
                await page.mouse.up();
                await page.keyboard.up(modifier);
                await page.waitForTimeout(50);
            };
            await drag('Alt', 40, 0);
            assert(Number(await eqWidth.inputValue()) > 1);
            assert.deepEqual([await frequency.inputValue(), await gain.inputValue()], before);
            await page.getByRole('button', { name: 'Undo Sample edit' }).click();
            assert.equal(await eqWidth.inputValue(), '1');
            await drag('Shift', 60, 0);
            const fine = Number(await frequency.inputValue());
            assert(fine >= Number(before[0]));
            await page.getByRole('button', { name: 'Undo Sample edit' }).click();
            await subpage('MIDI/CTRL', 'Control');
            if (width >= 1000) {
                assert(await page.locator('.sample-panel').evaluate(node => node.scrollHeight <= node.clientHeight + 1), 'six rows fit a 360px editor');
            }
            await page.screenshot({ path: resolve(output, `${name}-Control.png`) });
            assert.deepEqual(errors, []);
            results.push({ name, status: 'passed' });
            console.log(`PASS ${name}`);
        } catch (error) {
            results.push({ name, status: 'failed', error: error.message, errors });
            console.error(`FAIL ${name}: ${error.message}`);
            await page.screenshot({ path: resolve(output, `${name}-failure.png`) });
            process.exitCode = 1;
        } finally { await page.close(); }
    }
} finally {
    await writeFile(resolve(output, 'compact-results.json'), JSON.stringify(results, null, 2) + '\n');
    await browser.close();
}
