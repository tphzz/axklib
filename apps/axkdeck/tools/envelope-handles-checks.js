window.runEnvelopeHandlesChecks = async () => {
    const failures = [], cases = [];
    const check = (value, message) => { if (!value) failures.push(message); };
    const frame = () => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    const host = document.querySelector('[data-envelope-fixture]');
    const state = () => JSON.parse(document.querySelector('[data-envelope-state]').textContent);
    const click = async label => {
        [...document.querySelectorAll('nav[aria-label="Envelope fixture controls"] button')].find(node => node.textContent === label).click();
        await frame();
    };
    const select = async (node, value) => {
        node.focus();
        node.value = value;
        node.dispatchEvent(new Event('change', { bubbles: true }));
        await frame();
    };
    for (let i = 0; i < 100 && !host?.querySelector('.plot-handle'); i++) await new Promise(resolve => setTimeout(resolve, 50));
    check(!!host?.querySelector('.plot-handle'), 'Envelope graph did not load');
    if (!host?.querySelector('.plot-handle')) return { failures };
    for (const mode of ['Sample', 'Bank']) for (const kind of ['aeg', 'feg', 'peg']) {
        await click(mode); await click(kind);
        const label = `${mode} ${kind}`;
        const selector = host.querySelector('select[aria-label="Envelope stage"]');
        check(!!selector, `${label}: missing stage selector`);
        if (!selector) continue;
        const ids = kind === 'aeg' ? ['1', '2', '4'] : ['0', '1', '2', '4'];
        const header = host.querySelector('.graph-readout').getBoundingClientRect();
        const children = [...host.querySelector('.graph-readout').children];
        for (const [index, child] of children.entries()) {
            const box = child.getBoundingClientRect();
            check(box.left >= header.left - 0.5 && box.right <= header.right + 0.5 && box.top >= header.top - 0.5 && box.bottom <= header.bottom + 0.5, `${label}: title/readout/tools escape header`);
            if (index) check(children[index - 1].getBoundingClientRect().right <= box.left + 0.5, `${label}: title/readout/tools overlap`);
        }
        check(JSON.stringify([...selector.options].map(option => option.value)) === JSON.stringify(ids), `${label}: incorrect editable stages`);
        const trace = host.querySelector('[data-trace="envelope"]').getAttribute('d');
        const traceStyle = getComputedStyle(host.querySelector('[data-trace="envelope"]'));
        check(traceStyle.stroke !== 'none' && traceStyle.stroke !== 'rgba(0, 0, 0, 0)', `${label}: envelope trace is not visible`);
        const surfaceStyle = getComputedStyle(host.querySelector('.graph-surface'));
        check(surfaceStyle.backgroundColor !== getComputedStyle(host).backgroundColor && surfaceStyle.backgroundColor !== 'rgba(0, 0, 0, 0)', `${label}: graph surface lacks its dark background`);
        const positions = [...host.querySelectorAll('.plot-handle')].map(node => [node.style.left, node.style.top]);
        const original = JSON.stringify(state().values);
        if (kind !== 'aeg') check(host.querySelector('[data-handle="0"]').style.top === host.querySelector('[data-handle="1"]').style.top, `${label}: equal levels have different vertical coordinates`);
        for (const id of ids) {
            await select(selector, id);
            const handle = host.querySelector(`[data-handle="${id}"]`);
            check(handle.classList.contains('selected') && handle.getAttribute('aria-pressed') === 'true', `${label}: stage ${id} did not select handle`);
            const name = handle.getAttribute('aria-label').split(':')[0];
            check(host.querySelector('.graph-readout output').textContent.startsWith(`${name}:`), `${label}: stage ${id} readout missing`);
            check(!state().dirty && !state().canUndo && JSON.stringify(state().values) === original, `${label}: selecting stage ${id} changed draft or overrides`);
            check(host.querySelector('[data-trace="envelope"]').getAttribute('d') === trace, `${label}: stage selection moved trace`);
            check(JSON.stringify([...host.querySelectorAll('.plot-handle')].map(node => [node.style.left, node.style.top])) === JSON.stringify(positions), `${label}: selection moved handles`);
            handle.focus(); await frame();
            const style = getComputedStyle(handle);
            check(style.borderStyle === 'solid' && parseFloat(style.borderWidth) === 2 && style.borderColor !== 'rgba(0, 0, 0, 0)', `${label}: handle border is not visible`);
            check(style.outlineStyle !== 'none' && parseFloat(style.outlineWidth) >= 2, `${label}: stage ${id} keyboard focus halo missing (focus=${document.activeElement === handle}, visible=${handle.matches(':focus-visible')}, document=${document.hasFocus()}, outline=${style.outline})`);
            check(selector.value === id, `${label}: focus not synchronized to selector`);
            const surface = host.querySelector('.graph-surface').getBoundingClientRect(), box = handle.getBoundingClientRect();
            const scale = box.width / handle.offsetWidth;
            const halo = (parseFloat(style.outlineWidth) + parseFloat(style.outlineOffset)) * scale;
            check(box.left - halo >= surface.left - 0.5 && box.top - halo >= surface.top - 0.5 && box.right + halo <= surface.right + 0.5 && box.bottom + halo <= surface.bottom + 0.5, `${label}: stage ${id} halo clipped at graph boundary`);
        }
        const target = host.querySelector('[data-handle="1"]');
        target.focus(); await frame();
        target.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight', bubbles: true }));
        target.dispatchEvent(new KeyboardEvent('keyup', { key: 'ArrowRight', bubbles: true }));
        await frame();
        check(state().values[`${kind}.attack_rate`] === 126 && state().canUndo, `${label}: keyboard stage edit failed`);
        await click('Undo');
        check(!state().dirty && !state().canUndo && JSON.stringify(state().values) === original, `${label}: undo did not restore values and overrides`);
        if (kind !== 'aeg') {
            await click('Block initial');
            check(selector.querySelector('option[value="0"]').disabled, `${label}: unavailable stage must be disabled`);
        }
        await click('Disabled');
        check(selector.disabled, `${label}: disabled editor stage selector remains active`);
        cases.push({ mode, kind, stages: ids });
    }
    const exact = document.querySelector('[data-overlap-fixture]');
    const exactSelector = exact.querySelector('select');
    const a = exact.querySelector('[data-handle="first"]'), b = exact.querySelector('[data-handle="second"]');
    check(a.style.left === b.style.left && a.style.top === b.style.top, 'Exact overlap fixture does not overlap');
    for (const id of ['first', 'second', 'edge']) {
        await select(exactSelector, id);
        const handle = exact.querySelector(`[data-handle="${id}"]`);
        const rect = handle.getBoundingClientRect();
        check(document.elementFromPoint(rect.x + rect.width / 2, rect.y + rect.height / 2) === handle, `${id}: chosen coincident/boundary handle not hit-testable`);
    }
    check(document.documentElement.scrollWidth <= document.documentElement.clientWidth, 'Envelope controls overflow viewport');
    return { failures, cases };
};
