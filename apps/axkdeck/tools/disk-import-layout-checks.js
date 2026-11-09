window.runDiskImportLayoutChecks = async () => {
    const failures = [];
    const check = (condition, message) => { if (!condition) failures.push(message); };
    const shell = document.querySelector('[role="dialog"]');
    const box = node => {
        const b = node.getBoundingClientRect();
        return { x: b.x, y: b.y, width: b.width, height: b.height, right: b.right, bottom: b.bottom };
    };
    const zoom = shell.getBoundingClientRect().width / shell.offsetWidth;
    const visible = node => getComputedStyle(node).visibility !== 'hidden' && node.getClientRects().length > 0;
    const bounds = box(shell), footer = box(shell.querySelector('.dialog-footer'));
    const body = shell.querySelector('.floppy-content,.disk-import-body');
    const outerScroll = getComputedStyle(body).overflowY === 'auto';
    check(bounds.x >= -1 && bounds.right <= innerWidth + 1, 'Dialog exceeds viewport horizontally');
    check(bounds.y >= -1 && bounds.bottom <= innerHeight + 1, 'Dialog exceeds viewport vertically');
    check(document.documentElement.scrollWidth <= innerWidth + 1, 'Page has horizontal overflow');
    const sources = shell.querySelector('.source-members');
    if (sources) {
        check(box(sources).height / zoom <= 56.5, 'Source chips exceed their bounded two-row area');
        check(sources.scrollWidth <= sources.clientWidth + 1, 'Source chips overflow horizontally');
        for (const chip of sources.querySelectorAll('.source-member')) {
            const c = box(chip), style = getComputedStyle(chip);
            check(Math.abs(c.height / zoom - 26) <= 1, 'Source chip height is unstable');
            check(['Top', 'Right', 'Bottom', 'Left'].every(side => parseFloat(style[`border${side}Width`]) > 0 && style[`border${side}Style`] !== 'none'), 'Source chip border is incomplete');
            const name = chip.querySelector('.member-name'), remove = chip.querySelector('[data-remove-member]');
            if (remove) {
                const r = box(remove), text = box(chip.querySelector('.member-origin') ?? name);
                check(r.right <= c.right + 1 && r.x >= c.x && r.y >= c.y && r.bottom <= c.bottom + 1, 'Source chip remove button is clipped');
                check(r.x - text.right <= 5 * zoom, 'Source chip X is detached from the filename/origin');
            }
            if (sources.clientWidth > 500 && name.scrollWidth < 160) check(c.width < box(sources).width / 2, 'Short source chips stretch across the row');
        }
    }
    const panes = [...shell.querySelectorAll('.import-pane')];
    const narrow = shell.querySelector('.import-review-layout')?.classList.contains('narrow');
    const visiblePanes = panes.filter(visible);
    check(panes.length === 0 || visiblePanes.length === (narrow ? 1 : 2), 'Incorrect visible pane count');
    for (const pane of visiblePanes) {
        const b = box(pane), scroll = pane.querySelector('.import-pane-scroll');
        check((outerScroll ? box(body).bottom : b.bottom) <= footer.y + 1, 'Pane overlaps footer');
        check(scroll.clientHeight >= 70, 'List has less than 70px of usable vertical space');
        check(scroll.scrollWidth <= scroll.clientWidth + 1, 'Pane has horizontal overflow');
        check(scroll.tabIndex === 0, 'Pane cannot be scrolled by keyboard');
        const toolbar = pane.querySelector('.loss-consent-toolbar');
        if (toolbar) {
            check(!scroll.contains(toolbar), 'Bulk loss consent scrolls away with review items');
            check(box(toolbar).bottom <= box(scroll).y + 1, 'Bulk loss consent overlaps the review list');
            check(toolbar.scrollWidth <= toolbar.clientWidth + 1, 'Bulk loss consent overflows horizontally');
            check(getComputedStyle(toolbar.querySelector('label')).fontSize === '11px', 'Safety consent label is not 11px');
        }
    }
    if (panes.length && !narrow) check(box(panes[0]).right <= box(panes[1]).x - 10 * zoom, 'Panes overlap');
    if (narrow) {
        const tabs = [...shell.querySelectorAll('[role="tab"]')].map(box);
        if (body.clientWidth > 350) check(Math.abs(tabs[0].width - tabs[1].width) < 1, 'Narrow tabs are not equal width');
    }
    const settings = shell.querySelector('.disk-import-settings');
    if (settings) {
        const s = box(settings);
        check(s.right <= bounds.right - 8 * zoom, 'Settings overflow dialog');
        for (const control of settings.querySelectorAll('input,button')) {
            if (!visible(control)) continue;
            const b = box(control);
            check(b.x >= s.x - 1 && b.right <= s.right + 1 && b.bottom <= s.bottom + 1, `Settings control overflows: ${control.getAttribute('aria-label') || control.textContent}`);
        }
        const style = getComputedStyle(settings);
        check(style.borderTopStyle !== 'none' && parseFloat(style.borderTopWidth) * zoom >= 0.9, 'Settings border missing');
        const label = settings.querySelector('.destination-label');
        const mode = settings.querySelector('.destination-mode-group');
        if (label && mode) {
            const range = document.createRange(); range.selectNodeContents(label);
            const text = range.getBoundingClientRect(), toggle = box(mode);
            if (text.y < toggle.bottom && text.bottom > toggle.y) check(text.right + 3 * zoom <= toggle.x, 'Destination label crowds the mode toggle');
        }
        const destination = settings.querySelector('.import-destination');
        const capacity = settings.querySelector('.import-capacity');
        if (destination && capacity) {
            const d = box(destination), c = box(capacity);
            if (d.y < c.bottom && d.bottom > c.y) check(d.right + 3 * zoom <= c.x, 'Destination and capacity controls overlap');
        }
    }
    const actions = [...shell.querySelectorAll('.dialog-footer-actions button')].map(box);
    check(actions.every(b => Math.abs(b.y - actions[0].y) < 1 && Math.abs(b.height - actions[0].height) < 1), 'Footer actions are misaligned');
    const scroller = panes[0]?.querySelector('.import-pane-scroll');
    const fullRows = scroller ? [...scroller.querySelectorAll('.floppy-row:not(.excluded),.conversion-object')].filter(row => {
        const r = box(row), s = box(scroller);
        return r.y >= s.y - 1 && r.bottom <= s.bottom + 1;
    }).length : 0;
    if (scroller && shell.classList.contains('floppy-dialog') && shell.offsetWidth >= 1008 && shell.offsetHeight >= 649 && !location.search.includes('recovery')) {
        check(fullRows >= 8, `Only ${fullRows} complete ordinary rows visible`);
    }
    return { failures, bounds, footer, narrow, fullRows, zoom, settings: settings ? box(settings) : null };
};
