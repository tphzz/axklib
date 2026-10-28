import assert from 'node:assert/strict';
import { resolve } from 'node:path';
import { selectValue } from './select-control.mjs';

export async function exerciseCreate(page, mode, width, output) {
    const dialog = page.getByRole('dialog', { name: 'Create', exact: true });
    await dialog.waitFor();
    assert.deepEqual(await dialog.getByRole('tab').allTextContents(), ['Image', 'Device', 'SCSI2SDv6', 'SCSI2SD XML']);
    assert.equal(await page.getByLabel('Fixture calls').textContent(), '');
    assert.equal(await dialog.getByRole('tab', { name: 'Image', exact: true }).getAttribute('aria-selected'), 'true');
    const initialBounds = await dialog.boundingBox();
    const tab = mode === 'create-xml' ? 'SCSI2SD XML' : mode === 'create-v6' ? 'SCSI2SDv6' : 'Device';
    await dialog.getByRole('tab', { name: tab, exact: true }).click();
    assert.deepEqual(await dialog.boundingBox(), initialBounds, 'Create destination tabs retain their frame');
    if (mode === 'create-xml') {
        assert.equal(await page.getByLabel('Fixture calls').textContent(), '', 'XML validation precedes enumeration');
        await dialog.getByLabel('Configuration XML').setInputFiles({ name: 'synthetic.xml', mimeType: 'text/xml', buffer: Buffer.from('<SCSI2SD/>') });
    }
    await selectValue(dialog.getByRole('combobox', { name: 'Device', exact: true }), 'card');
    if (tab !== 'Device') await selectValue(dialog.getByRole('combobox', { name: 'SCSI target' }), '0');
    await dialog.getByLabel('Formatting scope').waitFor();
    const review = dialog.getByRole('button', { name: 'Review', exact: true });
    if (mode === 'create-mounted') {
        assert(await review.isDisabled());
        assert((await dialog.textContent()).includes('/media/Synthetic card'));
    } else {
        await review.waitFor();
        assert(await review.isEnabled());
    }
    const bounds = await dialog.boundingBox();
    assert.deepEqual(bounds, initialBounds, 'Inspection and device selection must not recenter Create');
    assert(bounds.x >= 0 && bounds.x + bounds.width <= width + 1);
    assert(await dialog.evaluate((node) => node.scrollWidth <= node.clientWidth + 1));
    const rows = dialog.locator('.contents-scroll');
    assert(await rows.evaluate((node) => node.scrollHeight > node.clientHeight));
    await page.screenshot({ path: resolve(output, `${mode}-${width}.png`) });
    if (mode === 'create-mounted') { await dialog.getByRole('button', { name: 'Cancel', exact: true }).click(); return; }
    await review.click();
    const confirmation = page.getByRole('dialog', { name: 'Erase device and format Yamaha SFS' });
    await confirmation.waitFor();
    const cancel = confirmation.getByRole('button', { name: 'Cancel', exact: true });
    assert(await cancel.evaluate((node) => node === document.activeElement));
    const erase = confirmation.getByRole('button', { name: 'Erase And Format' });
    assert(await erase.isDisabled());
    await confirmation.getByRole('checkbox', { name: /No sampler/ }).check();
    await confirmation.getByRole('checkbox', { name: /I have verified/ }).check();
    const typed = confirmation.getByRole('textbox', { name: 'Device erase confirmation' });
    await typed.fill('ERASE wrong device');
    assert(await erase.isDisabled());
    await typed.fill(`ERASE /dev/synthetic${tab !== 'Device' ? ' SCSI 0' : ''}`);
    await typed.press('Enter');
    assert(!(await page.getByLabel('Fixture calls').textContent()).includes('format '));
    assert(await erase.isEnabled());
    const geometry = await confirmation.locator('footer button').evaluateAll((buttons) => buttons.map((node) => ({ height: node.getBoundingClientRect().height, top: getComputedStyle(node).marginTop, bottom: getComputedStyle(node).marginBottom })));
    assert(geometry.every((item) => Math.abs(item.height - geometry[0].height) < 1 && item.top === '0px' && item.bottom === '0px'));
    assert(await confirmation.evaluate((node) => node.scrollWidth <= node.clientWidth + 1));
    await page.screenshot({ path: resolve(output, `${mode}-confirmation-${width}.png`) });
    if (mode === 'create-unconfirmed') {
        await erase.click();
        const check = dialog.getByRole('button', { name: 'Check status', exact: true });
        await check.waitFor();
        assert(await dialog.getByRole('button', { name: 'Close Create' }).isDisabled());
        assert.equal(await dialog.getByRole('button', { name: 'Review', exact: true }).count(), 0);
        await check.click();
        await dialog.getByRole('button', { name: 'Cancel', exact: true }).click();
    } else {
        await cancel.click();
        assert.deepEqual(await dialog.boundingBox(), bounds, 'Nested modal must restore the review geometry');
        await dialog.getByRole('button', { name: 'Cancel', exact: true }).click();
    }
    assert.equal((await page.getByLabel('Fixture calls').textContent()).split('format ').length - 1, mode === 'create-unconfirmed' ? 1 : 0);
    await dialog.waitFor({ state: 'detached' });
}
