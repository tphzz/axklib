window.runWorkspacePreferencesChecks = async () => {
    const failures = [];
    const check = (condition, message) => {
        if (!condition) failures.push(message);
    };
    const wait = async (predicate) => {
        for (let index = 0; index < 160; index++) {
            if (predicate()) return;
            await new Promise((resolve) => setTimeout(resolve, 25));
        }
        throw new Error('Workspace Preferences did not reach the expected state');
    };
    const trigger = document.querySelector('button[aria-label="Preferences"]');
    const dialog = () => document.querySelector('[role="dialog"][aria-label="Preferences"]');
    const button = (name) =>
        [...dialog().querySelectorAll('button')].find(
            (node) => (node.getAttribute('aria-label') || node.textContent).trim() === name,
        );
    const key = (node, value) =>
        node.dispatchEvent(
            new KeyboardEvent('keydown', {
                key: value,
                bubbles: true,
                cancelable: true,
            }),
        );
    const open = async () => {
        trigger.focus();
        trigger.click();
        await wait(() => dialog()?.contains(document.activeElement));
    };
    await wait(() => trigger && !trigger.disabled);
    await open();
    const shell = dialog();
    const bounds = shell.getBoundingClientRect();
    const backdrop = shell.closest('.dialog-backdrop').getBoundingClientRect();
    check(!shell.closest('.app-header'), 'Dialog must not be contained by the filtered app header');
    check(
        bounds.top >= 0 && bounds.bottom <= innerHeight && bounds.left >= 0 && bounds.right <= innerWidth,
        'Dialog must fit entirely in the viewport',
    );
    check(
        Math.abs(bounds.top + bounds.height / 2 - innerHeight / 2) < 2,
        'Dialog must be vertically centered in the viewport',
    );
    check(
        Math.abs(bounds.left + bounds.width / 2 - innerWidth / 2) < 2,
        'Dialog must be horizontally centered in the viewport',
    );
    check(
        Math.abs(backdrop.top) < 1 &&
            Math.abs(backdrop.left) < 1 &&
            Math.abs(backdrop.height - innerHeight) < 2 &&
            Math.abs(backdrop.width - innerWidth) < 2,
        'Backdrop must cover the entire viewport',
    );
    check(
        Boolean(document.querySelector('.app-shell').closest('[inert]')),
        'The entire workspace must be inert while Preferences is open',
    );
    button('Save').focus();
    key(document.activeElement, 'Tab');
    check(document.activeElement === button('Close'), 'Tab must wrap inside Preferences');
    button('a4k/a5k').click();
    key(shell, 'Escape');
    await wait(() => !dialog());
    check(document.activeElement === trigger, 'Escape must restore focus to Preferences');
    check(document.querySelector('output').textContent === 'A3000', 'Escape must discard draft');
    check(!document.querySelector('.app-shell').closest('[inert]'), 'Workspace must stop being inert on close');
    await open();
    button('a4k/a5k').click();
    button('Save').click();
    await wait(() => !dialog());
    check(document.querySelector('output').textContent === 'A4000_A5000', 'Save must publish the preference');
    check(document.activeElement === trigger, 'Save must restore focus to Preferences');
    await open();
    return {
        failures,
        viewport: { width: innerWidth, height: innerHeight },
        dialog: bounds.toJSON(),
        backdrop: backdrop.toJSON(),
    };
};
