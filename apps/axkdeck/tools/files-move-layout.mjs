import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const base = process.argv[2] ?? 'http://127.0.0.1:5191';
const output = resolve(process.argv[3] ?? '../../../build/logs/files-move/00001');
await mkdir(output, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const width of [800, 1280]) {
        const page = await browser.newPage({ viewport: { width, height: 900 } });
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        await page.goto(`${base}/tools/layout-fixtures/files-refresh.html`);
        await page.getByRole('button', { name: 'Expand SAMPLES', exact: true }).click();
        const first = page.locator('[data-file-entry="file-0"]');
        const second = page.locator('[data-file-entry="file-1"]');
        const target = page.locator('[data-file-entry="closed"]');
        await first.click();
        await second.click({ modifiers: ['Control'] });
        const start = await first.boundingBox();
        const end = await target.boundingBox();
        await page.mouse.move(start.x + 160, start.y + start.height / 2);
        await page.mouse.down();
        await page.mouse.move(end.x + 160, end.y + end.height / 2, { steps: 12 });
        await page.waitForFunction(() => document.querySelector('[data-file-entry="closed"]')?.classList.contains('drop-target'));
        await page.mouse.up();
        const dialog = page.getByRole('dialog', { name: 'Move entries' });
        await dialog.waitFor();
        const move = dialog.getByRole('button', { name: 'Move', exact: true });
        await move.waitFor();
        await page.waitForFunction(() => [...document.querySelectorAll('[role="dialog"] button')].some(button => button.textContent.trim() === 'Move' && !button.disabled));
        assert.equal(await page.locator('[data-testid="writes"]').textContent(), '0');
        assert.equal(await dialog.locator('li').count(), 2);
        const heights = await dialog.locator('.dialog-footer-actions button').evaluateAll(buttons => buttons.map(button => ({ height: button.getBoundingClientRect().height, margin: getComputedStyle(button).marginBlock })));
        assert.equal(heights[0].height, heights[1].height);
        assert(heights.every(button => button.margin === '0px'));
        await page.screenshot({ path: resolve(output, `move-review-${width}.png`) });
        await move.click();
        await dialog.waitFor({ state: 'detached' });
        await page.waitForFunction(() => document.querySelector('[data-testid="status"]')?.textContent.includes('Moved 2 entries'));
        for (const id of ['moved-file-0', 'moved-file-1']) assert.equal(await page.locator(`[data-file-entry="${id}"]`).getAttribute('aria-selected'), 'true');
        assert.equal(await target.getAttribute('aria-expanded'), 'true');
        assert.equal(await page.locator('[data-file-entry="folder"]').getAttribute('aria-expanded'), 'true');
        assert.equal(await page.locator('[data-testid="writes"]').textContent(), '1');
        // Moving back to the root can be cancelled without a second mutation.
        const moved = await page.locator('[data-file-entry="moved-file-0"]').boundingBox();
        const root = await page.locator('[data-files-root-drop]').boundingBox();
        await page.mouse.move(moved.x + 160, moved.y + moved.height / 2);
        await page.mouse.down();
        await page.mouse.move(root.x + root.width / 2, root.y + root.height / 2, { steps: 10 });
        await page.mouse.up();
        await dialog.waitFor();
        await dialog.getByRole('button', { name: 'Cancel', exact: true }).click();
        await dialog.waitFor({ state: 'detached' });
        assert.equal(await page.locator('[data-testid="writes"]').textContent(), '1');
        assert.deepEqual(errors, []);
        await page.screenshot({ path: resolve(output, `move-complete-${width}.png`) });
        results.push({ width, selected: 2, writes: 1, footerHeights: heights, errors });
        await page.close();
    }
    await writeFile(resolve(output, 'results.json'), `${JSON.stringify(results, null, 2)}\n`);
} finally { await browser.close(); }
