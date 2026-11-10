import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { createServer as createSocketServer } from 'node:net';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
if (!process.argv[2]) throw new Error('Usage: node tools/tab-bar-layout.mjs <output-directory>');
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const port = await new Promise((resolve, reject) => {
    const socket = createSocketServer();
    socket.once('error', reject);
    socket.listen(0, '127.0.0.1', () => {
        const port = socket.address().port;
        socket.close(error => error ? reject(error) : resolve(port));
    });
});
const server = await createServer({ server: { host: '127.0.0.1', port, strictPort: true } });
let browser;
let failure;
let interrupted = false;
const results = [];
const stop = async () => { await browser?.close(); await server.close(); };
const interrupt = () => { interrupted = true; void stop(); };
const deadline = setTimeout(interrupt, 180000);
process.once('SIGTERM', interrupt);
process.once('SIGINT', interrupt);
const near = (actual, expected, label) => assert(Math.abs(actual - expected) < 0.75, `${label}: ${actual} != ${expected}`);

async function checkBar(dialog) {
    const bar = dialog.locator('.source-tabs > .tab-bar');
    const geometry = await bar.evaluate(node => {
        const rect = node.getBoundingClientRect(), shell = node.closest('.dialog-shell').getBoundingClientRect();
        const style = getComputedStyle(node), sourceStyle = getComputedStyle(node.parentElement);
        const active = node.querySelector('[aria-selected="true"]');
        const tab = getComputedStyle(active), line = getComputedStyle(active, '::after');
        return {
            tag: node.tagName, context: node.dataset.context,
            height: rect.height, left: rect.left - shell.left, right: shell.right - rect.right,
            padding: [style.paddingLeft, style.paddingRight], separator: style.boxShadow,
            margins: [sourceStyle.marginTop, sourceStyle.marginBottom],
            tab: { height: active.getBoundingClientRect().height, font: tab.fontSize, radius: tab.borderRadius,
                lineHeight: line.height, lineLeft: line.left, lineRight: line.right },
        };
    });
    assert.equal(geometry.tag, 'DIV');
    assert.equal(geometry.context, 'dialog');
    near(geometry.height, 32, 'Tab-bar height');
    near(geometry.left, 1, 'Separator left edge');
    near(geometry.right, 1, 'Separator right edge');
    assert.deepEqual(geometry.padding, ['12px', '12px']);
    assert.deepEqual(geometry.margins, ['10px', '10px']);
    assert(geometry.separator.includes('inset') && geometry.separator.includes('rgb(61, 68, 72)'), 'Full-width quiet separator is missing');
    near(geometry.tab.height, 32, 'Tab height');
    assert.deepEqual({ ...geometry.tab, height: undefined }, {
        height: undefined, font: '11px', radius: '0px', lineHeight: '2px', lineLeft: '0px', lineRight: '0px',
    });
    return geometry;
}

async function checkCreate(dialog) {
    const name = dialog.getByRole('textbox', { name: 'File name', exact: true });
    const selection = await name.evaluate(node => ({ focused: document.activeElement === node, start: node.selectionStart, end: node.selectionEnd, length: node.value.length }));
    assert(selection.focused && selection.start === 0 && selection.end === selection.length, 'Initial filename must remain focused and selected');
    assert(await dialog.getByRole('button', { name: 'Create', exact: true }).isDisabled(), 'Missing location must disable Create');
    const form = await dialog.locator('.create-image-content').evaluate(node => {
        const rect = element => { const r = element.getBoundingClientRect(); return { x: r.x, y: r.y, right: r.right, bottom: r.bottom, width: r.width, height: r.height }; };
        const fields = [...node.children].slice(0, 5);
        const group = node.querySelector('.partitions');
        const buttons = [...group.querySelectorAll('button')];
        const input = node.querySelector('input');
        return {
            order: fields.map(field => field.querySelector(':scope > span, :scope > legend').textContent.trim()),
            labelsAbove: fields.every(field => {
                const label = field.querySelector(':scope > span, :scope > legend');
                const control = field.querySelector('output, [role="combobox"], input, .partitions');
                return rect(label).bottom <= rect(control).y + 0.5;
            }),
            width: rect(fields[0]).width, group: rect(group),
            type: rect(node.querySelector('[aria-label="Type"]')),
            browse: rect(node.querySelector('.location button')),
            partitions: buttons.map(button => ({ ...rect(button), disabled: button.disabled, selected: button.getAttribute('aria-pressed') })),
            suffix: node.querySelector('.create-hds-name > b').textContent,
            input: rect(input), suffixBounds: rect(node.querySelector('.create-hds-name > b')),
        };
    });
    assert.deepEqual(form.order, ['Location', 'Type', 'File name', 'Capacity', 'Partitions']);
    assert(form.labelsAbove, 'Create labels must remain above their controls');
    near(form.group.width, form.width, 'Full-width partition selector');
    near(form.type.width, form.group.width, 'Full-width Type control');
    near(form.group.height, 29, 'Partition control height');
    assert.equal(form.partitions.length, 8);
    for (const [index, button] of form.partitions.entries()) {
        near(button.width, form.partitions[0].width, 'Equal partition tracks');
        near(button.y, form.partitions[0].y, 'Single partition row');
        assert.equal(button.disabled, index !== 0);
        assert.equal(button.selected, String(index === 0));
    }
    assert(form.browse.width < form.width / 3, 'Browse must stay compact');
    assert.equal(form.suffix, '.hds');
    assert(form.suffixBounds.x > form.input.right, 'Filename suffix must stay outside the input');
    return { selection, form };
}

try {
    await server.listen();
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    for (const width of [1366, 640, 390]) for (const scale of [1, 1.25, 1.5]) for (const kind of ['open', 'create']) {
        if (interrupted) throw new Error('Tab-bar browser check interrupted or timed out');
        // Native WebView zoom changes the layout viewport; CSS zoom does not.
        const page = await browser.newPage({ viewport: { width, height: 900 }, deviceScaleFactor: scale });
        page.setDefaultTimeout(10000);
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        try {
            const mode = kind === 'create' ? 'create-device&missing-location' : 'image';
            await page.goto(`http://127.0.0.1:${port}/tools/layout-fixtures/device-dialog.html?mode=${mode}`);
            const dialog = page.getByRole('dialog', { name: kind === 'create' ? 'Create' : 'Open', exact: true });
            await dialog.waitFor();
            if (kind === 'create') await dialog.getByRole('button', { name: '1 partition', exact: true }).waitFor({ state: 'visible' });
            const bar = await checkBar(dialog);
            const create = kind === 'create' ? await checkCreate(dialog) : undefined;
            const before = await dialog.boundingBox();
            const footer = await dialog.locator('footer:visible').boundingBox();
            const viewport = page.viewportSize();
            assert(before.x >= -0.75 && before.y >= -0.75
                && before.x + before.width <= viewport.width + 0.75
                && before.y + before.height <= viewport.height + 0.75,
            'The entire dialog, including footer actions, must fit inside the viewport');
            assert(await dialog.evaluate(node => node.scrollWidth <= node.clientWidth + 1), 'Dialog must not overflow horizontally');
            await page.screenshot({ path: resolve(output, `${kind}-${width}-${scale}.png`) });
            const tabs = dialog.getByRole('tablist');
            await tabs.getByRole('tab', { name: 'Image', exact: true }).focus();
            await page.keyboard.press('End');
            await page.waitForFunction(id => document.getElementById(id)?.getAttribute('aria-selected') === 'true', `${kind}-tab-SCSI2SD-XML`);
            const last = tabs.getByRole('tab', { name: 'SCSI2SD XML', exact: true });
            assert(await last.evaluate(node => {
                const clip = node.parentElement.getBoundingClientRect(), tab = node.getBoundingClientRect();
                return document.activeElement === node && tab.left >= clip.left - 1 && tab.right <= clip.right + 1;
            }), 'Accepted keyboard selection must reveal and focus the last tab');
            await page.keyboard.press('Home');
            await page.waitForFunction(id => document.getElementById(id)?.getAttribute('aria-selected') === 'true', `${kind}-tab-Image`);
            assert.deepEqual(await dialog.boundingBox(), before, 'Tab selection must preserve the dialog frame');
            assert.deepEqual(await dialog.locator('footer:visible').boundingBox(), footer, 'Tab selection must preserve the footer');
            await dialog.getByRole('button', { name: 'Cancel', exact: true }).click();
            await dialog.waitFor({ state: 'detached' });
            assert.deepEqual(errors, []);
            results.push({ kind, width, scale, viewport, shell: before, footer, bar, create, passed: true });
        } finally { await page.close(); }
    }
} catch (error) {
    failure = error;
    throw error;
} finally {
    clearTimeout(deadline);
    process.removeListener('SIGTERM', interrupt);
    process.removeListener('SIGINT', interrupt);
    await stop();
    const portReleased = await new Promise((resolve, reject) => {
        const socket = createSocketServer();
        socket.once('error', reject);
        socket.listen(port, '127.0.0.1', () => socket.close(error => error ? reject(error) : resolve(true)));
    });
    await writeFile(resolve(output, 'results.json'), JSON.stringify({
        scaleMethod: 'Chromium device-scale emulation at a fixed logical viewport; not native WebView zoom',
        results, failure: failure?.message,
        browserStopped: !browser?.isConnected(), serverStopped: !server.httpServer?.listening, portReleased, port }, null, 2) + '\n');
}
