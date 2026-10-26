window.runUiControlsChecks = async function () {
    const failures = [], measurements = [];
    const check = (condition, message) => { if (!condition) failures.push(message); };
    const settle = () => new Promise(resolve => setTimeout(resolve, 50));
    const labelled = label => document.querySelector(`[aria-label=${JSON.stringify(label)}]`);
    const key = async (node, value) => { node.dispatchEvent(new KeyboardEvent('keydown', { key: value, bubbles: true, cancelable: true })); await settle(); };
    const click = async node => { if (!node) throw new Error('Missing test control'); node.scrollIntoView({ block: 'nearest' }); node.click(); await settle(); };
    const button = text => [...document.querySelectorAll('button')].find(node => node.textContent.trim() === text);
    for (let i = 0; i < 40 && !labelled('Output'); i++) await settle();
    for (const name of ['Image', 'Output', 'Controller', 'Destination partition', 'Destination volume', 'Import mode', 'Level']) {
        const control = labelled(name), style = getComputedStyle(control);
        check(style.color === 'rgb(192, 200, 207)', `${name}: shared value color`);
        check(style.fontSize === '11px' && style.fontWeight === '400', `${name}: shared 11px regular typography`);
        check(style.backgroundColor === 'rgb(11, 13, 15)', `${name}: shared field background`);
        await click(control);
        const popup = document.getElementById(control.getAttribute('aria-controls'));
        if (!popup) throw new Error(`${name}: missing popup`);
        const bounds = popup.getBoundingClientRect(), popupStyle = getComputedStyle(popup);
        check(popupStyle.backgroundColor === 'rgb(16, 21, 22)', `${name}: opaque shared popup background`);
        check(popupStyle.color === style.color && popupStyle.fontSize === style.fontSize, `${name}: matching popup typography`);
        check(bounds.left >= -1 && bounds.right <= innerWidth + 1 && bounds.top >= -1 && bounds.bottom <= innerHeight + 1, `${name}: viewport-clamped popup`);
        const active = popup.querySelector('.active');
        if (active) {
            const activeStyle = getComputedStyle(active);
            check(activeStyle.backgroundColor === 'rgb(41, 73, 88)', `${name}: blue active background`);
            check(activeStyle.color === 'rgb(192, 200, 207)', `${name}: readable active text`);
        }
        measurements.push({ name, color: style.color, font: style.fontSize, weight: style.fontWeight, popup: popupStyle.backgroundColor, bounds: bounds.toJSON() });
        await key(control, 'Escape');
        check(!document.getElementById(control.getAttribute('aria-controls')), `${name}: Escape closes its popup`);
    }
    const empty = labelled('Empty device list');
    await click(empty);
    check(labelled('Empty device list').getAttribute('aria-activedescendant') === null, 'Empty list has no active option');
    check(document.querySelector('.select-empty').textContent === 'No devices available', 'Empty state is non-selectable');
    await key(empty, 'Enter'); await key(empty, 'Escape');
    check(labelled('Selection changes').textContent === '0', 'Browsing and cancelling never changes a value');

    await click(button('Open control dialog'));
    const dialog = labelled('Control test dialog'), modalChoice = labelled('Modal choice');
    await click(modalChoice);
    check(dialog.contains(document.querySelector('.select-popup')), 'Modal owns its popup');
    check(document.activeElement === modalChoice, 'DOM focus stays on modal combobox');
    await key(modalChoice, 'ArrowDown'); await key(modalChoice, 'Escape');
    check(dialog.isConnected && !document.querySelector('.select-popup'), 'First Escape closes only the dropdown');
    await click(modalChoice);
    await key(modalChoice, 'Tab');
    check(!document.querySelector('.select-popup'), 'Tab cancels without committing');
    await click(modalChoice);
    await click(button('Open nested dialog'));
    const nested = labelled('Nested control dialog');
    check(nested && modalChoice.closest('[inert]'), 'Nested modal makes parent inert');
    check(!document.querySelector('.select-popup'), 'Nested modal dismisses the parent popup');
    await key(nested, 'Escape');
    check(!labelled('Nested control dialog') && !modalChoice.closest('[inert]'), 'Closing nested dialog restores the parent');
    await click(button('Close control dialog'));

    await click(button('Rename fixture volume'));
    const rename = labelled('Volume name');
    check(document.activeElement === rename && rename.selectionStart === rename.value.length && rename.selectionEnd === rename.value.length, 'Rename initially focuses the end caret');
    rename.setRangeText('X', rename.selectionStart, rename.selectionEnd, 'end');
    rename.dispatchEvent(new Event('input', { bubbles: true })); await settle();
    check(rename.value === 'New VolumeX', 'Typing appends instead of replacing the name');
    rename.setSelectionRange(2, 2); await settle();
    check(rename.selectionStart === 2 && rename.selectionEnd === 2, 'Validation does not reset the caret');
    await key(rename, 'Escape');
    check(!labelled('Volume name'), 'Rename Escape dismisses normally');
    check(labelled('Selection changes').textContent === '0', 'No fixture operation was submitted');
    await click(labelled('Output'));
    return { failures, measurements };
};
