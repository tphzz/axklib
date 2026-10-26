import assert from 'node:assert/strict';

export async function openSelect(control) {
    await control.waitFor({ state: 'visible' });
    if (await control.getAttribute('aria-expanded') !== 'true') await control.click();
    const id = await control.getAttribute('aria-controls');
    assert(id, 'Open selector owns its listbox');
    const popup = control.page().locator(`[id=${JSON.stringify(id)}]`);
    await popup.waitFor();
    return popup;
}

export async function selectValue(control, value) {
    const popup = await openSelect(control);
    await popup.locator(`[role="option"][data-value=${JSON.stringify(String(value))}]`).click();
}

export async function controlValue(control) {
    return control.evaluate((element) => element.value);
}
