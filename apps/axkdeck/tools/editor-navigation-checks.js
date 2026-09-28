window.runEditorNavigationChecks = async () => {
    const failures = [];
    const geometries = [];
    const surfaces = [];
    const check = (value, message) => { if (!value) failures.push(message); };
    const frame = () => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    const controls = document.querySelector('nav[aria-label="Fixture controls"]');
    const click = async (label) => {
        [...controls.querySelectorAll('button')].find(button => button.textContent.trim() === label).click();
        await frame();
    };
    const boxes = () => [...document.querySelectorAll('[data-editor-fixture]')].map(host => {
        const header = host.querySelector('header');
        const tabs = header?.querySelector('[role="tablist"]');
        const selected = tabs?.querySelector('[aria-selected="true"]');
        const style = selected && getComputedStyle(selected);
        const rect = header?.getBoundingClientRect();
        return { kind: host.dataset.editorFixture, header, tabs, selected, style, rect };
    });
    for (let i = 0; i < 100 && boxes().some(value => !value.tabs); i++) await new Promise(resolve => setTimeout(resolve, 50));
    await frame();
    const initial = boxes();
    check(initial.every(value => value.tabs), 'All three real editors must load');
    if (initial.some(value => !value.tabs)) return { failures };
    const initialY = initial.map(value => value.rect.y);
    const initialTabs = initial.map(value => [...value.tabs.querySelectorAll('[role="tab"]')].map(tab => {
        const rect = tab.getBoundingClientRect();
        return [rect.x, rect.y, rect.width, rect.height];
    }));
    function inspect(label) {
        const current = boxes();
        for (const [index, value] of current.entries()) {
            const { kind, header, tabs, selected, style, rect } = value;
            const ratio = rect.width / header.offsetWidth;
            check(Math.abs(parseFloat(getComputedStyle(header).height) - 32) <= 0.01, `${label}: ${kind} primary header must be 32px`);
            check(style.fontSize === '11px', `${label}: ${kind} tab text must be 11px`);
            check(style.paddingLeft === '8px' && style.paddingRight === '8px', `${label}: ${kind} tab padding must be 8px`);
            check(Math.abs(rect.y - initialY[index]) <= 1, `${label}: ${kind} header moved`);
            for (const [tabIndex, tab] of [...tabs.querySelectorAll('[role="tab"]')].entries()) {
                const r = tab.getBoundingClientRect();
                check([r.x, r.y, r.width, r.height].every((value, axis) => Math.abs(value - initialTabs[index][tabIndex][axis]) <= 1), `${label}: ${kind} tab ${tab.textContent.trim()} moved`);
            }
            check(header.querySelectorAll('.format-badge').length === 0, `${label}: redundant ${kind} format badge`);
            const actionButtons = [...header.querySelectorAll('button')].filter(button => button.getAttribute('role') !== 'tab');
            for (const button of actionButtons) {
                const b = button.getBoundingClientRect();
                check(b.top >= rect.top && b.bottom <= rect.bottom + 0.5, `${label}: ${kind} action ${button.textContent.trim() || button.getAttribute('aria-label')} wrapped`);
                check(Math.abs(parseFloat(getComputedStyle(button).height) - 26) <= 0.01, `${label}: ${kind} action height must be 26px`);
                check(b.right <= rect.right + 0.5 && b.left >= rect.left, `${label}: ${kind} action clipped`);
            }
            if (actionButtons.length) {
                const tabRect = tabs.getBoundingClientRect();
                const first = actionButtons[0].getBoundingClientRect();
                check(tabRect.right <= first.left + 1, `${label}: ${kind} tabs overlap actions`);
                check(tabRect.width > 30 * ratio, `${label}: ${kind} tab strip has no usable space`);
            }
            const second = header.parentElement.querySelector('.sample-pages');
            if (kind !== 'program') check(second && Math.abs(parseFloat(getComputedStyle(second).height) - 28) <= 0.01, `${label}: ${kind} secondary row must be 28px`);
            const firstStyle = current[0].style;
            for (const key of ['fontSize', 'color', 'backgroundColor', 'borderBottomColor', 'borderBottomWidth'])
                check(style[key] === firstStyle[key], `${label}: ${kind} selected-tab ${key} differs from Program`);
            geometries.push({ label, kind, width: header.clientWidth, height: rect.height / ratio, tabWidth: tabs.clientWidth, overflow: tabs.scrollWidth > tabs.clientWidth });
            check(selected.getAttribute('tabindex') === '0', `${label}: ${kind} active tab is not keyboard reachable`);
        }
    }
    inspect('clean');
    function effectiveBackground(node) {
        for (let current = node; current; current = current.parentElement) {
            const color = getComputedStyle(current).backgroundColor;
            if (color !== 'transparent' && color !== 'rgba(0, 0, 0, 0)') return color;
        }
        return 'transparent';
    }
    function inspectSurfaces(label) {
        const hosts = [...document.querySelectorAll('[data-editor-fixture]')];
        const expected = getComputedStyle(hosts[1].querySelector('.device-editor')).backgroundColor;
        check(expected !== 'transparent' && expected !== 'rgba(0, 0, 0, 0)', `${label}: sample panel needs an opaque surface`);
        for (const host of hosts) {
            const kind = host.dataset.editorFixture;
            const nodes = kind === 'program'
                ? host.querySelectorAll('.editor-panel, .collection-toolbar, .editor-body, .assignment-table, .assignment-header, .assignment-table > button, .empty-copy, .editor-canvas')
                : host.querySelectorAll('.device-editor, .sample-panel');
            check(nodes.length > 0, `${label}: ${kind} content surfaces missing`);
            for (const node of nodes) {
                const actual = effectiveBackground(node);
                check(actual === expected, `${label}: ${kind} ${node.className} background ${actual} differs from sample panel ${expected}`);
                surfaces.push({ label, kind, className: node.className, background: actual });
            }
        }
    }
    inspectSurfaces('empty assignments');
    await click('Toggle assignments');
    check(document.querySelectorAll('.assignment-table > button').length === 1, 'Populated assignment fixture missing');
    inspectSurfaces('populated assignments');
    await click('Toggle assignments');
    const programTabs = boxes()[0].tabs;
    for (const button of [...programTabs.querySelectorAll('[role="tab"]')].slice(1)) {
        button.click();
        await frame();
        inspectSurfaces(`Program ${button.textContent.trim()}`);
    }
    programTabs.querySelector('[role="tab"]').click();
    await frame();
    for (const label of ['Comparison', 'Dirty', 'Saving', 'Refresh recovery', 'Status recovery', 'Dirty', 'Change objects']) {
        await click(label);
        inspect(label);
    }
    for (const { kind, tabs } of boxes()) {
        const buttons = [...tabs.querySelectorAll('[role="tab"]')];
        for (const [key, target] of [['End', buttons.at(-1)], ['Home', buttons[0]], ['ArrowRight', buttons[1]], ['ArrowLeft', buttons[0]]]) {
            const active = tabs.querySelector('[aria-selected="true"]');
            active.focus();
            active.dispatchEvent(new KeyboardEvent('keydown', { key, bubbles: true }));
            await frame();
            check(document.activeElement === target, `${kind}: ${key} did not focus target tab`);
            check(target.getAttribute('aria-selected') === 'true', `${kind}: ${key} did not activate target tab`);
            const t = target.getBoundingClientRect();
            const area = tabs.getBoundingClientRect();
            check(t.left >= area.left - 1 && t.right <= area.right + 1, `${kind}: ${key} leaves selected tab outside visible strip`);
        }
        if (kind === 'program') continue;
        buttons.find(button => button.textContent.trim() === 'Map/Out').click();
        await frame();
        const row = tabs.closest('[data-editor-fixture]').querySelector('.sample-pages');
        const pages = [...row.querySelectorAll('button')];
        pages[0].focus();
        pages[0].dispatchEvent(new KeyboardEvent('keydown', { key: 'End', bubbles: true }));
        await frame();
        check(document.activeElement === pages.at(-1), `${kind}: secondary End did not focus final page`);
        check(pages.at(-1).getAttribute('aria-pressed') === 'true', `${kind}: secondary End did not activate final page`);
        const last = pages.at(-1).getBoundingClientRect();
        const area = row.querySelector('nav').getBoundingClientRect();
        check(last.left >= area.left - 1 && last.right <= area.right + 1, `${kind}: secondary selected page is clipped`);
        check(Math.abs(parseFloat(getComputedStyle(row).height) - 28) <= 0.01, `${kind}: secondary overflow changed row height`);
    }
    check(document.documentElement.scrollWidth <= document.documentElement.clientWidth, 'Horizontal viewport overflow');
    return { failures, geometries, surfaces };
};
