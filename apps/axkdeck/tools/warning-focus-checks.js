window.runWarningFocusChecks = async function () {
    const failures = [];
    const check = (value, message) => { if (!value) failures.push(message); };
    for (let i = 0; i < 80 && !document.querySelector('#experimental-warning-title'); i++) await new Promise(resolve => setTimeout(resolve, 50));
    const title = document.querySelector('#experimental-warning-title');
    const button = document.querySelector('.experimental-warning-dialog .primary-button');
    check(!!title && !!button, 'Real warning is mounted');
    if (!title || !button) return { failures };
    check(title.tabIndex === -1, 'Reading title is not a new Tab stop');
    check(title.hasAttribute('data-dialog-initial-focus') && !button.hasAttribute('data-dialog-initial-focus'), 'Only the title requests initial focus');
    check(getComputedStyle(title).outlineStyle === 'none', 'Only the static title hides its outline');
    check(document.activeElement === title, 'Initial focus is the title, not the action');
    const css = getComputedStyle(button);
    const outlined = css.outlineStyle !== 'none' && parseFloat(css.outlineWidth) > 0;
    check(!outlined, 'Initial action has no focus ring');
    check(css.height === '30px', 'Existing command height is unchanged');
    check(document.querySelector('output').textContent === '0', 'Opening does not acknowledge');
    return { failures, activeElement: document.activeElement?.tagName, actionOutline: css.outline, actionHeight: css.height };
};
