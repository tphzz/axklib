window.runBankFormatChecks = async () => {
    const failures = [];
    const check = (value, message) => { if (!value) failures.push(message); };
    const wait = async (predicate) => {
        for (let i = 0; i < 120; i++) { if (predicate()) return; await new Promise(resolve => setTimeout(resolve, 25)); }
        throw new Error('Timed out waiting for bank format UI');
    };
    const button = (name, parent = document) => [...parent.querySelectorAll('button')].find(node =>
        (node.getAttribute('aria-label') || node.textContent).trim() === name);
    const writes = () => Number(document.querySelector('[aria-label="Conversion writes"]').textContent);
    const section = document.querySelector('[data-inspector-section="stored-format"]');
    const heading = button('Stored format');
    check(heading.getAttribute('aria-expanded') === 'false', 'Stored format starts collapsed');
    heading.click();
    await wait(() => heading.getAttribute('aria-expanded') === 'true');
    check(section.textContent.includes('1 a3k, 1 a4k/a5k'), 'Mixed member formats remain separate');
    const blocked = new URLSearchParams(location.search).has('pending');
    const initial = section.querySelector('.format-badge')?.textContent.trim();
    check(['a3k', 'a4k/a5k'].includes(initial), 'Bank has a recognized stored-format badge');
    for (let pass = 0; pass < (blocked ? 1 : 2); pass++) {
        const row = button('Inspect Bank');
        row.focus();
        row.dispatchEvent(new KeyboardEvent('keydown', { key: 'F10', shiftKey: true, bubbles: true }));
        await wait(() => document.querySelector('[role="menuitem"]'));
        const action = [...document.querySelectorAll('[role="menuitem"]')].find(node => node.textContent.includes('Convert to'));
        check(action?.textContent.includes('sample bank format'), 'Bank menu names the target and object kind');
        action.click();
        await wait(() => document.querySelector('[role="dialog"]'));
        const dialog = document.querySelector('[role="dialog"]');
        check(dialog.textContent.includes('Member Samples and Wave Data are not converted or edited'), 'Member preservation is explicit');
        const cancel = button('Cancel', dialog), convert = button('Convert', dialog);
        check(!dialog.querySelector('input[type="checkbox"]'), 'No redundant acknowledgement checkbox');
        check(Math.abs(cancel.getBoundingClientRect().height - convert.getBoundingClientRect().height) < 1, 'Footer action heights match');
        check(dialog.getBoundingClientRect().width <= innerWidth, 'Dialog fits viewport');
        check(dialog.scrollHeight <= dialog.clientHeight + 1, 'Dialog content stays within shell');
        if (blocked) {
            check(convert.disabled && !cancel.disabled, 'Pending operations block conversion but allow dismissal');
            check(dialog.textContent.includes('Pending bank parameter'), 'Pending blocker remains visible');
            dialog.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
        } else {
            convert.click();
            if (pass === 0) {
                await wait(() => button('Refresh', dialog));
                check(cancel.disabled && writes() === 1, 'Committed conversion is recovery-only');
                button('Refresh', dialog).click();
            }
        }
        await wait(() => !document.querySelector('[role="dialog"]'));
        check(writes() === (blocked ? 0 : pass + 1), 'Refresh must not repeat a write');
        check(section.textContent.includes('1 a3k, 1 a4k/a5k'), 'Bank conversion never changes member formats');
    }
    check(section.querySelector('.format-badge')?.textContent.trim() === initial, 'Round trip restores own bank badge');
    return { failures, writes: writes(), width: innerWidth, blocked };
};
