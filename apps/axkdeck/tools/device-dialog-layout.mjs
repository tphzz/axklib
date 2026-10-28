import { selectValue, openSelect, controlValue } from './select-control.mjs';
import { exerciseCreate } from './create-dialog-check.mjs';
import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { createServer as createSocketServer } from 'node:net';
import { pathToFileURL } from 'node:url';
import { createServer } from 'vite';

const { chromium } = await import(
    process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright'
);
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const availablePort = await new Promise((resolve, reject) => {
    const socket = createSocketServer();
    socket.once('error', reject);
    socket.listen(0, '127.0.0.1', () => {
        const port = socket.address().port;
        socket.close((error) => (error ? reject(error) : resolve(port)));
    });
});
const server = await createServer({ server: { host: '127.0.0.1', port: availablePort, strictPort: true } });
let browser, port;
const results = [];
async function stop() {
    await browser?.close();
    await server.close();
}
const signal = () => void stop();
process.once('SIGTERM', signal);
process.once('SIGINT', signal);
const deadline = setTimeout(signal, 90000);
try {
    await server.listen();
    port = server.httpServer.address().port;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    for (const width of [1366, 640, 390])
        for (const mode of ['image', 'device', 'xml', 'v6', 'v6-invalid', 'readonly', 'denied', 'recovery', 'create-device', 'create-mounted', 'create-unconfirmed', 'create-v6', 'create-xml']) {
            const page = await browser.newPage({ viewport: { width, height: 844 } });
            page.setDefaultTimeout(10000);
            const errors = [];
            page.on('pageerror', (error) => errors.push(error.message));
            await page.goto(`http://127.0.0.1:${port}/tools/layout-fixtures/device-dialog.html?mode=${mode}`);
            if (mode.startsWith('create-')) {
                await exerciseCreate(page, mode, width, output);
                assert.deepEqual(errors, []);
                results.push({ width, mode, passed: true });
                await page.close();
                continue;
            }
            const dialog = page.getByRole('dialog');
            await dialog.waitFor();
            const source = mode !== 'recovery';
            const v6 = mode === 'v6' || mode === 'v6-invalid';
            let footer = source ? dialog.locator('.open-source-panel:not([hidden]) > footer') : dialog.locator('footer');
            let primary = footer.locator('button').last();
            if (source) {
                assert.deepEqual(await dialog.getByRole('tab').allTextContents(), [
                    'Image',
                    'Device',
                    'SCSI2SDv6',
                    'SCSI2SD XML',
                ]);
                assert.equal(await page.getByLabel('Fixture calls').textContent(), '');
                const initial = await dialog.boundingBox();
                await dialog.getByRole('tab', { name: 'Device', exact: true }).waitFor({ state: 'visible' });
                await dialog.getByRole('tab', { name: 'Image', exact: true }).focus();
                await page.keyboard.press(mode === 'xml' ? 'End' : 'ArrowRight');
                if (v6) await page.keyboard.press('ArrowRight');
                assert.equal(
                    await dialog
                        .getByRole('tab', {
                            name: mode === 'xml' ? 'SCSI2SD XML' : v6 ? 'SCSI2SDv6' : 'Device',
                            exact: true,
                        })
                        .getAttribute('aria-selected'),
                    'true',
                );
                if (mode === 'xml') {
                    assert.equal(await page.getByLabel('Fixture calls').textContent(), '');
                    assert(await dialog.getByRole('combobox', { name: 'Device', exact: true }).isDisabled());
                    await dialog.getByLabel('Configuration XML', { exact: true }).setInputFiles({
                        name: 'synthetic.xml',
                        mimeType: 'text/xml',
                        buffer: Buffer.from('<SCSI2SD/>'),
                    });
                }
                await page.getByLabel('Fixture calls').filter({ hasText: 'list ' }).waitFor();
                if (mode === 'image') {
                    await dialog.getByRole('tab', { name: 'Image', exact: true }).click();
                    await dialog.getByRole('option', { name: /Yamaha images/ }).click();
                    await dialog.getByRole('option', { name: /^Disk1\.hds/ }).click();
                    const rows = dialog.locator('.storage-picker-results.source-selection');
                    assert(await rows.evaluate((node) => node.scrollHeight > node.clientHeight));
                    await rows.evaluate((node) => {
                        node.scrollTop = node.scrollHeight;
                    });
                } else {
                    const device = dialog.getByRole('combobox', { name: 'Device', exact: true });
                    assert.equal(await controlValue(device), '');
                    footer = dialog.locator('.open-source-panel:not([hidden]) > footer');
                    primary = footer.getByRole('button', { name: 'Open', exact: true });
                    assert(await primary.isDisabled());
                    await selectValue(device, 'fixture');
                    if (mode === 'v6-invalid') {
                        await dialog
                            .getByRole('status')
                            .filter({ hasText: 'No SCSI2SDv6 configuration found' })
                            .waitFor();
                        assert(await primary.isDisabled());
                        const target = dialog.getByRole('combobox', { name: 'SCSI target', exact: true });
                        assert(await target.isDisabled());
                        assert.equal((await target.textContent()).trim(), 'Select a configured target');
                        await page.screenshot({ path: resolve(output, `${mode}-${width}.png`) });
                        await dialog.getByRole('button', { name: 'Cancel', exact: true }).click();
                        await dialog.waitFor({ state: 'detached' });
                        assert.equal(await page.getByLabel('Fixture calls').textContent(), 'list ');
                        assert.deepEqual(errors, []);
                        results.push({ width, mode, passed: true });
                        await page.close();
                        continue;
                    }
                    if (mode === 'xml' || v6) {
                        assert(await primary.isDisabled());
                        const target = dialog.getByRole('combobox', { name: 'SCSI target', exact: true });
                        await (await openSelect(target)).getByRole('option', { name: /SCSI ID 0/ }).waitFor({ state: 'attached' });
                        assert.equal(await controlValue(target), '');
                        await selectValue(target, '0');
                        if (v6) {
                            const details = dialog.getByRole('definition').filter({ hasText: /^5242880$/ });
                            assert.equal(await details.count(), 1, 'Target end is exclusive and expressed in bytes');
                            assert.equal(
                                await dialog
                                    .getByLabel('Configured target details')
                                    .getByText('512 B', { exact: true })
                                    .count(),
                                1,
                            );
                        }
                    }
                    assert.equal(
                        await dialog
                            .getByRole('button', { name: 'Read & write', exact: true })
                            .getAttribute('aria-pressed'),
                        'true',
                    );
                    assert(await primary.isDisabled(), 'Writing requires explicit exclusive-use acknowledgement');
                    await dialog.getByRole('checkbox').focus();
                    await page.keyboard.press('Space');
                    assert(await primary.isEnabled());
                    await dialog.getByRole('button', { name: 'Refresh devices', exact: true }).click();
                    await page.waitForFunction(
                        () => document.querySelector('[aria-label="Fixture calls"]')?.textContent === 'list list ',
                    );
                    await dialog.getByRole('button', { name: 'Refresh devices', exact: true }).click({ trial: true });
                    assert.equal(await controlValue(device), 'fixture', 'Refresh must retain the selected device');
                    assert(
                        await dialog.getByRole('checkbox').isChecked(),
                        'A matching refresh retains acknowledgement',
                    );
                    if (mode === 'xml' || v6)
                        assert.equal(
                            await controlValue(dialog.getByRole('combobox', { name: 'SCSI target', exact: true })),
                            '0',
                        );
                    const beforeAccessChange = await dialog.boundingBox();
                    await dialog.getByRole('button', { name: 'Read only', exact: true }).click();
                    assert.equal(await dialog.getByRole('checkbox').count(), 0);
                    assert(await primary.isEnabled());
                    await dialog.getByRole('button', { name: 'Read & write', exact: true }).click();
                    assert.equal(await dialog.getByRole('checkbox').isChecked(), false);
                    assert(await primary.isDisabled());
                    assert.deepEqual(
                        await dialog.boundingBox(),
                        beforeAccessChange,
                        'Access changes must not resize the dialog',
                    );
                    if (mode === 'readonly') {
                        await dialog.getByRole('button', { name: 'Read only', exact: true }).click();
                    } else {
                        await dialog.getByRole('checkbox').focus();
                        await page.keyboard.press('Space');
                    }
                }
                const current = await dialog.boundingBox();
                assert.deepEqual(current, initial, 'Tab switching must not resize or recenter the dialog');
                footer = dialog.locator('.open-source-panel:not([hidden]) > footer');
                primary = footer.locator('.dialog-footer-actions button').last();
            } else if (mode === 'recovery') {
                assert(await primary.isDisabled());
                assert.equal(await controlValue(dialog.getByRole('combobox', { name: /^Device/ })), '');
                assert(await dialog.getByRole('button', { name: 'Connect read-only' }).isDisabled());
                await selectValue(dialog.getByRole('combobox', { name: /^Device/ }), 'fixture');
                await dialog.getByRole('button', { name: 'Connect read-only' }).click();
                assert.equal(await controlValue(dialog.getByRole('combobox', { name: /^Target/ })), '');
                await selectValue(dialog.getByRole('combobox', { name: /^Target/ }), 'disk');
            }
            if (!source) {
                const acknowledgement = dialog.getByRole('checkbox');
                assert.equal(await acknowledgement.isChecked(), false);
                await acknowledgement.focus();
                await page.keyboard.press('Space');
                assert.equal(await acknowledgement.isChecked(), true);
                if (mode === 'recovery') {
                    await selectValue(dialog.getByRole('combobox', { name: 'Interrupted operation', exact: true }), 'synthetic-operation-0');
                    assert.equal(await acknowledgement.isChecked(), false, 'Changing the operation clears recovery consent');
                    await acknowledgement.check();
                }
            }
            for (const select of await dialog.locator('select:visible').all()) {
                const theme = await select.evaluate((node) => {
                    const style = getComputedStyle(node);
                    return {
                        shared: node.classList.contains('dialog-field-control'),
                        colorScheme: style.colorScheme,
                        background: style.backgroundColor,
                        borderStyle: style.borderStyle,
                        fontSize: style.fontSize,
                        height: node.getBoundingClientRect().height,
                        expectedHeight: Number.parseFloat(style.getPropertyValue('--density-control')),
                    };
                });
                assert(theme.shared, 'Native dropdowns use shared dialog styling');
                assert.equal(theme.colorScheme, 'dark', 'Native option popups use the dark palette');
                assert.notEqual(theme.background, 'rgb(255, 255, 255)');
                assert.equal(theme.borderStyle, 'solid');
                assert.equal(theme.fontSize, '11px');
                assert.equal(theme.height, theme.expectedHeight);
                if (await select.isEnabled()) {
                    await select.focus();
                    assert(
                        await select.evaluate((node) => {
                                const colorSwatch = document.createElement('span');
                                colorSwatch.style.color = 'var(--color-accent)';
                                node.parentElement.append(colorSwatch);
                                const accent = getComputedStyle(colorSwatch).color;
                                colorSwatch.remove();
                            return document.activeElement === node && getComputedStyle(node).borderColor === accent;
                        }),
                        'Focused dropdowns use the shared accent border',
                    );
                }
            }
            assert(await primary.isEnabled());
            const geometry = await footer.evaluate((node) => {
                const dialog = node.closest('[role="dialog"]');
                const shell = dialog.getBoundingClientRect();
                return {
                    x: shell.x,
                    y: shell.y,
                    right: shell.right,
                    bottom: shell.bottom,
                    footerTop: node.getBoundingClientRect().top,
                    actions: [...node.querySelectorAll('.dialog-footer-actions button')].map((button) => ({
                        height: button.getBoundingClientRect().height,
                        top: button.getBoundingClientRect().top,
                        marginTop: getComputedStyle(button).marginTop,
                        marginBottom: getComputedStyle(button).marginBottom,
                    })),
                };
            });
            assert(geometry.x >= 0 && geometry.right <= width + 1);
            assert(geometry.y >= 0 && geometry.bottom <= 845);
            for (const action of geometry.actions) {
                assert.equal(action.marginTop, '0px');
                assert.equal(action.marginBottom, '0px');
                assert.equal(action.height, geometry.actions[0].height);
                assert.equal(action.top, geometry.actions[0].top);
            }
            if (mode === 'recovery') {
                const rows = dialog.getByLabel('Device operation status');
                assert(await rows.evaluate((node) => node.scrollHeight > node.clientHeight));
                await rows.evaluate((node) => {
                    node.scrollTop = node.scrollHeight;
                });
                assert.equal(
                    await dialog.locator('footer').evaluate((node) => node.getBoundingClientRect().top),
                    geometry.footerTop,
                );
            }
            await page.screenshot({ path: resolve(output, `${mode}-${width}.png`) });
            if (['device', 'xml', 'v6', 'readonly', 'denied'].includes(mode)) {
                await primary.click();
                if (mode === 'denied') {
                    await dialog.getByRole('status').filter({ hasText: 'Permission declined' }).waitFor();
                    assert.equal(await page.getByLabel('Fixture calls').textContent(), 'list list authorize close ');
                    await page.screenshot({ path: resolve(output, `denied-error-${width}.png`) });
                    await dialog.getByRole('button', { name: 'Read only', exact: true }).click();
                    await primary.click();
                }
                await dialog.waitFor({ state: 'detached' });
                assert.equal(
                    await page.getByLabel('Fixture calls').textContent(),
                    mode === 'readonly'
                        ? 'list list open '
                        : mode === 'denied'
                          ? 'list list authorize close open '
                          : 'list list authorize open ',
                );
            } else {
                await dialog.focus();
                await page.keyboard.press('Escape');
                await dialog.waitFor({ state: 'detached' });
                assert.equal(await page.getByLabel('Fixture calls').textContent(), source ? 'list ' : 'close ');
            }
            assert.deepEqual(errors, []);
            results.push({ width, mode, geometry, passed: true });
            await page.close();
        }
} finally {
    clearTimeout(deadline);
    process.removeListener('SIGTERM', signal);
    process.removeListener('SIGINT', signal);
    await stop();
    if (port)
        await new Promise((resolve, reject) => {
            const socket = createSocketServer();
            socket.once('error', reject);
            socket.listen(port, '127.0.0.1', () => socket.close((error) => (error ? reject(error) : resolve())));
        });
    await writeFile(
        resolve(output, 'results.json'),
        JSON.stringify({ port, serverStopped: true, browserStopped: true, portReleased: !!port, results }, null, 2) +
            '\n',
    );
}
