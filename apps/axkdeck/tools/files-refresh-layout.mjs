import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const base = process.argv[2] ?? 'http://127.0.0.1:5191';
const output = resolve(process.argv[3] ?? '../../../build/logs/files-refresh/00001');
await mkdir(output, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const width of [800, 1280]) {
        const page = await browser.newPage({ viewport: { width, height: 900 } });
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        await page.goto(`${base}/tools/layout-fixtures/files-refresh.html`);
        const folder = page.locator('[data-file-entry="folder"]');
        await page.getByRole('button', { name: 'Expand SAMPLES', exact: true }).click();
        await page.locator('[data-file-entry="file-79"]').waitFor({ state: 'attached' });
        await folder.click();
        const tree = page.getByRole('treegrid');
        await tree.evaluate(node => { node.scrollTop = 350; node.dispatchEvent(new Event('scroll')); });
        const before = await tree.evaluate(node => node.scrollTop);
        assert(before > 0, 'The background must genuinely scroll');
        const transfer = await page.evaluateHandle(() => {
            const data = new DataTransfer();
            data.items.add(new File(['test'], 'tone.bin', { type: 'application/octet-stream' }));
            return data;
        });
        await folder.dispatchEvent('drop', { dataTransfer: transfer });
        const dialog = page.getByRole('dialog');
        await dialog.waitFor();
        await page.getByRole('button', { name: 'Import', exact: true }).click();
        await dialog.waitFor({ state: 'detached' });
        await page.waitForFunction(() => document.querySelector('[data-testid="status"]')?.textContent === 'Imported files');
        await page.waitForFunction(top => document.querySelector('[role="treegrid"]')?.scrollTop === top, before);
        assert.equal(await folder.getAttribute('aria-expanded'), 'true');
        assert.equal(await folder.getAttribute('aria-selected'), 'true');
        assert.equal(await page.locator('[data-file-entry="closed"]').getAttribute('aria-expanded'), 'false');
        assert.equal(await page.locator('[data-file-entry="imported"]').textContent().then(text => text.includes('TONE.BIN')), true);
        assert.equal(await page.locator('[data-testid="writes"]').textContent(), '1');
        assert.deepEqual(errors, []);
        await page.screenshot({ path: resolve(output, `import-refresh-${width}.png`) });
        results.push({ width, scrollTop: before, writes: 1, expanded: true, collapsedSibling: true });
        await transfer.dispose();
        await page.close();
    }
    await writeFile(resolve(output, 'results.json'), `${JSON.stringify(results, null, 2)}\n`);
} finally { await browser.close(); }
