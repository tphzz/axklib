import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const { chromium } = await import(
    process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright'
);
const base = process.argv[2] ?? 'http://127.0.0.1:5191';
const output = resolve(process.argv[3] ?? '../../../build/logs/floppy-directory/00001/layout');
await mkdir(output, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];
try {
    for (const viewport of [
        { width: 1366, height: 768 },
        { width: 640, height: 700 },
        { width: 390, height: 844 },
    ]) {
        const page = await browser.newPage({ viewport });
        const errors = [];
        page.on('pageerror', (error) => errors.push(error.message));
        await page.goto(`${base}/tools/layout-fixtures/companion-disks.html`);
        await page.getByRole('dialog', { name: 'Add companion disks' }).waitFor();
        const geometry = () =>
            page.evaluate(() => {
                const rect = (element) => {
                    const value = element.getBoundingClientRect();
                    return {
                        x: value.x,
                        y: value.y,
                        width: value.width,
                        height: value.height,
                        right: value.right,
                        bottom: value.bottom,
                    };
                };
                const rows = document.querySelector('.companion-disk-list');
                return {
                    shell: rect(document.querySelector('[role="dialog"]')),
                    footer: rect(document.querySelector('.dialog-footer')),
                    rows: { scrollHeight: rows.scrollHeight, clientHeight: rows.clientHeight },
                    actions: [...document.querySelectorAll('.dialog-footer button')].map((button) => ({
                        ...rect(button),
                        marginTop: getComputedStyle(button).marginTop,
                        marginBottom: getComputedStyle(button).marginBottom,
                    })),
                };
            });
        const before = await geometry();
        assert(before.shell.x >= 0 && before.shell.right <= viewport.width + 1);
        assert(before.shell.y >= 0 && before.shell.bottom <= viewport.height + 1);
        assert(before.rows.scrollHeight > before.rows.clientHeight);
        for (const button of before.actions) {
            assert.equal(button.marginTop, '0px');
            assert.equal(button.marginBottom, '0px');
            assert(Math.abs(button.height - before.actions[0].height) < 1);
            assert(Math.abs(button.y - before.actions[0].y) < 1);
        }
        await page.locator('.companion-disk-list').evaluate((element) => {
            element.scrollTop = element.scrollHeight;
        });
        assert.deepEqual((await geometry()).footer, before.footer);
        await page.getByRole('button', { name: 'Add and retry', exact: true }).click();
        assert.deepEqual((await geometry()).footer, before.footer);
        assert(await page.getByRole('button', { name: 'Cancel', exact: true }).isDisabled());
        await page.keyboard.press('Escape');
        assert(await page.getByRole('dialog').isVisible());
        await page.screenshot({ path: resolve(output, `${viewport.width}.png`) });
        await page.reload();
        await page.getByRole('dialog').waitFor();
        await page.keyboard.press('Escape');
        await page.getByText('Closed', { exact: true }).waitFor();
        assert.deepEqual(errors, []);
        results.push({ viewport, ...before });
        await page.close();
    }
    await writeFile(resolve(output, 'geometry.json'), JSON.stringify(results, null, 2) + '\n');
    console.log(`Verified ${results.length} companion dialog viewports`);
} finally {
    await browser.close();
}
