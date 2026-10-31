import assert from 'node:assert/strict';
import { resolve } from 'node:path';
import { selectValue } from './select-control.mjs';

export async function checkSourceTabTransitions(browser, base, output) {
    const results = [];
    for (const leaveFocus of [false, true]) {
        const page = await browser.newPage({ viewport: { width: 640, height: 844 } });
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        page.setDefaultTimeout(10000);
        try {
            await page.goto(`${base}/tools/layout-fixtures/device-dialog.html?mode=v6&delayed-close`);
            const dialog = page.getByRole('dialog', { name: 'Open', exact: true });
            const tabs = dialog.getByRole('tablist', { name: 'Open source' });
            const source = tabs.getByRole('tab', { name: 'SCSI2SDv6', exact: true });
            await source.click();
            await selectValue(dialog.getByRole('combobox', { name: 'Device', exact: true }), 'fixture');
            await dialog.getByRole('combobox', { name: 'SCSI target', exact: true }).waitFor({ state: 'visible' });
            await page.waitForFunction(() => document.querySelector('[aria-label="SCSI target"]')?.getAttribute('aria-disabled') !== 'true');
            await source.focus();
            await page.keyboard.press('Home');
            await page.waitForFunction(() => [...document.querySelectorAll('[role="tab"]')].every(node => node.disabled));
            await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
            const busyFocus = await page.evaluate(() => document.activeElement?.tagName);
            if (leaveFocus) await dialog.getByRole('heading', { name: 'Open', exact: true }).click();
            const image = tabs.getByRole('tab', { name: 'Image', exact: true });
            await page.waitForFunction(() => document.getElementById('open-tab-Image')?.getAttribute('aria-selected') === 'true');
            if (leaveFocus) {
                await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
                assert.equal(await image.evaluate(node => document.activeElement === node), false, 'Pointer departure must prevent delayed focus restoration');
            } else {
                await page.waitForFunction(() => document.activeElement === document.getElementById('open-tab-Image'));
            }
            const geometry = await image.evaluate(node => {
                const style = getComputedStyle(node), line = getComputedStyle(node, '::after');
                return { height: node.getBoundingClientRect().height, fontSize: style.fontSize, radius: style.borderRadius, left: line.left, right: line.right, lineHeight: line.height, color: line.backgroundColor };
            });
            assert.deepEqual(geometry, { height: 32, fontSize: '11px', radius: '0px', left: '0px', right: '0px', lineHeight: '2px', color: 'rgb(104, 151, 187)' });
            assert.deepEqual(errors, []);
            await page.screenshot({ path: resolve(output, `source-tab-transition-${leaveFocus ? 'departed' : 'retained'}.png`) });
            results.push({ leaveFocus, busyFocus, geometry, passed: true });
            await dialog.getByRole('button', { name: 'Cancel', exact: true }).click();
            await dialog.waitFor({ state: 'detached' });
        } finally {
            await page.close();
        }
    }
    return results;
}
