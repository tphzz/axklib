import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createServer as socketServer } from 'node:net';
import { createServer } from 'vite';

const { chromium } = await import(
    process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright'
);
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const server = await createServer({ server: { host: '127.0.0.1', port: 0 } });
let browser, port;
async function stop() { await browser?.close(); await server.close(); }
const signal = () => void stop();
process.once('SIGTERM', signal); process.once('SIGINT', signal);
const deadline = setTimeout(signal, 120000);
const results = [];
try {
    await server.listen(); port = server.httpServer.address().port;
    const base = `http://127.0.0.1:${port}`;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    for (const viewport of [
        { width: 1366, height: 768 },
        { width: 640, height: 700 },
        { width: 390, height: 844 },
    ]) {
        const page = await browser.newPage({ viewport, deviceScaleFactor: 1 });
        const errors = [];
        page.on('pageerror', (error) => errors.push(error.message));
        await page.goto(`${base}/tools/layout-fixtures/floppy-import.html`);
        await page.getByRole('dialog', { name: 'Import floppy' }).waitFor();
        const geometry = () =>
            page.evaluate(() => {
                const rect = (element) => {
                    const r = element.getBoundingClientRect();
                    return { x: r.x, y: r.y, width: r.width, height: r.height, right: r.right, bottom: r.bottom };
                };
                const shell = document.querySelector('[role="dialog"]');
                const footer = document.querySelector('.dialog-footer');
                const rows = document.querySelector('[data-pane="contents"] .import-pane-scroll');
                return {
                    shell: rect(shell),
                    footer: rect(footer),
                    header: rect(document.querySelector('.floppy-table-heading')),
                    rows: { ...rect(rows), scrollHeight: rows.scrollHeight, clientHeight: rows.clientHeight },
                    actions: [...footer.querySelectorAll('button')].map((button) => ({
                        text: button.textContent.trim(),
                        ...rect(button),
                        marginTop: getComputedStyle(button).marginTop,
                        marginBottom: getComputedStyle(button).marginBottom,
                    })),
                    headings: [...shell.querySelectorAll('h3')].map((h) => ({
                        actual: getComputedStyle(h).fontSize,
                        expected: h.closest('.floppy-rows') ? '10px' : '11px',
                    })),
                    sourceLabel: {
                        ...rect(shell.querySelector('.source-summary strong')),
                        clientWidth: shell.querySelector('.source-summary strong').clientWidth,
                        scrollWidth: shell.querySelector('.source-summary strong').scrollWidth,
                    },
                };
            });
        const before = await geometry();
        assert(before.shell.x >= 0 && before.shell.right <= viewport.width + 1);
        assert(before.shell.y >= 0 && before.shell.bottom <= viewport.height + 1);
        assert(before.rows.scrollHeight > before.rows.clientHeight);
        assert(before.header.height >= 24, 'The fixed table header must not shrink or clip');
        assert(before.sourceLabel.clientWidth >= before.sourceLabel.scrollWidth,
            'The source identity must remain fully visible beside the utility buttons');
        assert(before.headings.every(({ actual, expected }) => actual === expected),
            'Pane headings and table category labels must retain their respective compact typography');
        const controlStyles = () =>
            page.evaluate(() => {
                const style = (selector) => {
                    const element = document.querySelector(selector);
                    const css = getComputedStyle(element);
                    return {
                        height: element.getBoundingClientRect().height,
                        font: css.fontSize,
                        marginTop: css.marginTop,
                        marginBottom: css.marginBottom,
                    };
                };
                return {
                    primary: style('.dialog-footer .primary-button'),
                    secondary: style('.dialog-footer .secondary-button'),
                    title: style('.dialog-header h2'),
                };
            });
        const floppyStyles = await controlStyles();
        await page.screenshot({ path: resolve(output, `initial-${viewport.width}x${viewport.height}.png`) });
        assert.deepEqual(
            before.actions.map((b) => b.text),
            ['Cancel', 'Review', 'Import'],
        );
        for (const button of before.actions) {
            assert.equal(button.marginTop, '0px');
            assert.equal(button.marginBottom, '0px');
            assert(Math.abs(button.height - before.actions[0].height) < 1);
            assert(Math.abs(button.y - before.actions[0].y) < 1);
        }
        await page.locator('[data-pane="contents"] .import-pane-scroll').evaluate((element) => {
            element.scrollTop = element.scrollHeight;
        });
        assert.deepEqual((await geometry()).footer, before.footer);
        assert.deepEqual((await geometry()).header, before.header);
        await page.getByRole('checkbox', { name: 'Select all objects' }).uncheck();
        assert.deepEqual((await geometry()).footer, before.footer);
        assert(await page.getByRole('button', { name: 'Import', exact: true }).isDisabled());
        await page.screenshot({ path: resolve(output, `${viewport.width}x${viewport.height}.png`) });
        await page.keyboard.press('Escape');
        await page.getByText('Closed', { exact: true }).waitFor();
        assert.deepEqual(errors, []);
        await page.goto(`${base}/tools/layout-fixtures/floppy-import.html?packages`);
        await page.getByRole('dialog', { name: 'Import packages', exact: true }).waitFor();
        assert.deepEqual(await controlStyles(), floppyStyles, 'Package and floppy dialog control geometry must match');
        assert.deepEqual(
            (await page.locator('.dialog-footer-actions button').allTextContents()).map((text) => text.trim()),
            ['Cancel', 'Review', 'Import'],
        );
        await page.screenshot({ path: resolve(output, `packages-${viewport.width}x${viewport.height}.png`) });
        assert.deepEqual(errors, []);
        await page.goto(`${base}/tools/layout-fixtures/floppy-import.html?direct`);
        await page.getByRole('button', { name: 'Add disks...', exact: true }).waitFor();
        assert.equal(await page.getByRole('button', { name: 'Computer', exact: true }).count(), 0);
        assert.equal(await page.locator('.import-source-choice').count(), 0);
        assert.deepEqual((await geometry()).footer, before.footer);
        await page.screenshot({ path: resolve(output, `direct-${viewport.width}x${viewport.height}.png`) });
        assert.deepEqual(errors, []);
        for (const folder of [
            'axk/floppy/unpacked/Drum Kits/norddrms',
            'axk/floppy/unpacked/Drum Kits/norddrms/disk1',
        ]) {
            await page.goto(
                `${base}/tools/layout-fixtures/floppy-import.html?direct&folder=${encodeURIComponent(folder)}`,
            );
            const input = page.getByRole('textbox', { name: 'New volume name' });
            const expected = folder.split('/').pop();
            await page.waitForFunction(
                (value) => document.querySelector('[aria-label="New volume name"]')?.value === value,
                expected,
            );
            assert.equal(await input.inputValue(), expected);
            await page.locator('.source-members').getByText(`Yamaha/${folder}`, { exact: true }).waitFor();
            for (const draft of ['Custom name', '']) {
                await input.fill(draft);
                await page.getByRole('button', { name: 'Existing', exact: true }).click();
                await page.getByRole('button', { name: 'New', exact: true }).click();
                assert.equal(await input.inputValue(), draft);
            }
            await input.fill(expected);
            await page.screenshot({ path: resolve(output, `folder-${expected}-${viewport.width}.png`) });
            assert.deepEqual(errors, []);
        }
        results.push({ viewport, ...before });
        await page.close();
    }
    await writeFile(resolve(output, 'geometry.json'), JSON.stringify(results, null, 2) + '\n');
    console.log(`Verified ${results.length} viewports; ${output}`);
} finally {
    clearTimeout(deadline);
    process.removeListener('SIGTERM', signal); process.removeListener('SIGINT', signal);
    await stop();
    if (port) await new Promise((done, reject) => {
        const socket = socketServer(); socket.once('error', reject);
        socket.listen(port, '127.0.0.1', () => socket.close(error => error ? reject(error) : done()));
    });
    await writeFile(resolve(output, 'cleanup.json'), JSON.stringify({ serverStopped: true, browserStopped: true, portReleased: !!port }) + '\n');
}
