import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const base = process.argv[2] ?? 'http://127.0.0.1:5191';
const output = resolve(process.argv[3] ?? '../../../build/logs/files-image-import/00001');
await mkdir(output, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const width of [480, 800, 1280]) {
        const page = await browser.newPage({ viewport: { width, height: 900 } });
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        await page.goto(`${base}/tools/layout-fixtures/files-refresh.html?floppy=1`);
        const folder = page.locator('[data-file-entry="folder"]');
        await page.getByRole('button', { name: 'Expand SAMPLES', exact: true }).click();
        await page.locator('[data-file-entry="file-79"]').waitFor({ state: 'attached' });
        const tree = page.getByRole('treegrid');
        await tree.evaluate(node => { node.scrollTop = 300; node.dispatchEvent(new Event('scroll')); });
        const before = await tree.evaluate(node => node.scrollTop);
        const transfer = await page.evaluateHandle(() => {
            const data = new DataTransfer();
            data.items.add(new File(['test'], 'disk.ima', { type: 'application/octet-stream' }));
            return data;
        });
        // Dropping on a file must resolve to the same parent directory.
        await page.locator('[data-file-entry="file-0"]').dispatchEvent('drop', { dataTransfer: transfer });
        const dialog = page.getByRole('dialog', { name: 'Import floppy files' });
        await dialog.waitFor();
        const mode = page.getByRole('button', { name: 'Contents', exact: true });
        await mode.waitFor();
        assert.equal(await mode.getAttribute('aria-pressed'), 'true');
        await page.getByRole('button', { name: 'Review', exact: true }).waitFor();
        await page.waitForFunction(() => [...document.querySelectorAll('button')].some(node => node.textContent.trim() === 'Review' && !node.disabled));
        assert.equal(await dialog.getByRole('checkbox', { name: 'Select all entries' }).isChecked(), true);
        const geometry = await dialog.evaluate(node => {
            const rect = node.getBoundingClientRect();
            const footer = node.querySelector('footer').getBoundingClientRect();
            const scroll = node.querySelector('.image-rows');
            const buttons = [...node.querySelectorAll('footer button')].map(button => ({ height: button.getBoundingClientRect().height, marginTop: getComputedStyle(button).marginTop, marginBottom: getComputedStyle(button).marginBottom }));
            return { top: rect.top, bottom: rect.bottom, left: rect.left, right: rect.right, footer: footer.top, overflowing: scroll.scrollHeight > scroll.clientHeight, buttons };
        });
        assert(geometry.left >= 0 && geometry.right <= width && geometry.top >= 0 && geometry.bottom <= 900);
        assert(geometry.overflowing);
        assert(geometry.buttons.every(button => button.height === geometry.buttons[0].height && button.marginTop === '0px' && button.marginBottom === '0px'));
        await page.screenshot({ path: resolve(output, `dialog-${width}.png`) });
        await page.getByRole('button', { name: 'File', exact: true }).click();
        assert.equal(await page.getByRole('textbox', { name: 'Filename 1.1', exact: true }).inputValue(), 'DISK.IMA');
        await mode.click();
        assert.equal(await dialog.evaluate(node => node.querySelector('footer').getBoundingClientRect().top), geometry.footer);
        await dialog.getByRole('checkbox', { name: 'Select all entries' }).uncheck();
        await dialog.getByRole('checkbox', { name: 'Import disk.ima: F000.S1A', exact: true }).check();
        await page.getByRole('button', { name: 'Review', exact: true }).click();
        await page.getByRole('button', { name: 'Import', exact: true }).click();
        await dialog.waitFor({ state: 'detached' });
        await page.waitForFunction(() => document.querySelector('[data-testid="status"]')?.textContent === 'Imported files');
        await page.waitForFunction(top => document.querySelector('[role="treegrid"]')?.scrollTop === top, before);
        assert.equal(await folder.getAttribute('aria-expanded'), 'true');
        assert.equal(await page.locator('[data-testid="writes"]').textContent(), '1');
        assert.deepEqual(errors, []);
        await page.screenshot({ path: resolve(output, `imported-${width}.png`) });
        results.push({ width, geometry, writes: 1, expanded: true, scrollTop: before });
        await page.close();
    }
    await writeFile(resolve(output, 'results.json'), `${JSON.stringify(results, null, 2)}\n`);
} finally { await browser.close(); }
