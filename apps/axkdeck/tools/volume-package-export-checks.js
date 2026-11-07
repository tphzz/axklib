window.runVolumePackageExportChecks = async () => {
    const failures = [];
    const check = (value, message) => { if (!value) failures.push(message); };
    const wait = async (predicate) => {
        for (let i = 0; i < 120; i++) {
            if (predicate()) return;
            await new Promise(resolve => setTimeout(resolve, 25));
        }
        throw new Error('Timed out waiting for volume package export UI');
    };
    await wait(() => window.volumePackageExportFixture && document.querySelector('[role="dialog"]'));
    const fixture = window.volumePackageExportFixture;
    const dialog = () => document.querySelector('[role="dialog"]');
    const button = name => [...dialog().querySelectorAll('button')].find(node =>
        (node.getAttribute('aria-label') || node.textContent).trim() === name);
    const geometry = () => {
        const shell = dialog(), rect = shell.getBoundingClientRect();
        const footer = shell.querySelector('.dialog-footer').getBoundingClientRect();
        check(rect.left >= -1 && rect.right <= innerWidth + 1, 'Dialog fits horizontal viewport');
        check(rect.top >= -1 && rect.bottom <= innerHeight + 1, 'Dialog fits vertical viewport');
        check(footer.top >= rect.top && footer.bottom <= rect.bottom + 1, 'Footer stays visible');
        check(shell.scrollWidth <= shell.clientWidth + 1, 'No horizontal overflow');
        const actions = [...shell.querySelectorAll('.dialog-footer button')];
        const heights = actions.map(node => node.getBoundingClientRect().height);
        check(heights.every(height => Math.abs(height - heights[0]) < 1), 'Footer action heights match');
        check(actions.every(node => ['0px', '0px'].every((zero, i) =>
            getComputedStyle(node)[i ? 'marginBottom' : 'marginTop'] === zero)), 'Footer actions have no vertical margins');
        return { width: rect.width, height: rect.height, footerHeight: footer.height, actionHeights: heights };
    };
    const states = {};
    check(!dialog().textContent.includes('Storage location') && !dialog().textContent.includes('This computer'), 'Native error has no destination choices');
    check(dialog().querySelector('[role="alert"]')?.textContent.includes('Download archive cache'), 'Native error is announced');
    const cancel = button('Cancel'), retry = button('Retry');
    check(Boolean(cancel && retry && !retry.disabled), 'Native error has Cancel then enabled Retry');
    check(cancel.compareDocumentPosition(retry) & Node.DOCUMENT_POSITION_FOLLOWING, 'Cancel precedes Retry');
    const summary = dialog().querySelector('.native-export-summary');
    check(getComputedStyle(summary).fontSize === getComputedStyle(dialog()).getPropertyValue('--dialog-body-font-size').trim(), 'Summary uses shared body typography');
    check(getComputedStyle(summary.querySelector('strong')).fontWeight === '600', 'Source label uses canonical semibold');
    check(summary.tabIndex === 0 && dialog().querySelector('[role="alert"]').tabIndex === 0, 'Overflowing source/error regions are keyboard-reachable');
    states.failed = geometry();
    retry.click();
    await wait(() => dialog().getAttribute('aria-busy') === 'true');
    check(button('Retry').disabled && !button('Cancel').disabled, 'Inspecting disables Retry but allows Cancel');
    await wait(() => Math.abs(Number(getComputedStyle(button('Retry')).opacity) - 0.45) < 0.01);
    check(dialog().querySelector('.dialog-footer-status').textContent === 'Inspecting source', 'Inspecting lifecycle stays in footer');
    states.inspecting = geometry();
    // Canonical one-line states stay stable; narrow layouts must allow accessible error wrapping.
    if (states.failed.width >= 519)
        check(Math.abs(states.failed.height - states.inspecting.height) < 1, 'Canonical error/inspection summary preserves dialog height');
    fixture.show('failed', true);
    await wait(() => dialog().querySelector('[role="alert"]')?.textContent.length > 1000);
    states.long = geometry();
    const scroller = dialog().querySelector('.dialog-results');
    check(scroller.scrollHeight > scroller.clientHeight, 'Long results have a bounded foreground scroller');
    scroller.scrollTop = scroller.scrollHeight;
    check(scroller.scrollTop > 0, 'Foreground results remain scrollable');
    fixture.show('empty');
    await wait(() => dialog().textContent.includes('no non-empty volumes'));
    check(!button('Retry') && !dialog().textContent.includes('Storage location'), 'Empty scope cannot submit');
    states.empty = geometry();
    fixture.show('remote');
    await wait(() => dialog().textContent.includes('Storage location'));
    check(dialog().textContent.includes('This computer'), 'Remote desktop retains both destination choices');
    states.remote = geometry();
    fixture.show('failed');
    await wait(() => Boolean(button('Retry')));
    dialog().dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
    await wait(() => !dialog());
    check(!document.querySelector('button')?.closest('[inert]'), 'Closing restores the background');
    fixture.show('failed');
    await wait(() => Boolean(dialog()));
    await new Promise(resolve => setTimeout(resolve, 200));
    return { failures, states, width: innerWidth, height: innerHeight };
};
