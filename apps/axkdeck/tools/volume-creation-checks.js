window.runVolumeCreationChecks = async function () {
    const failures = [];
    const check = (value, message) => { if (!value) failures.push(message); };
    const frame = () => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    const wait = async predicate => {
        for (let i = 0; i < 150 && !predicate(); i++) await new Promise(resolve => setTimeout(resolve, 20));
        if (!predicate()) throw new Error('Volume fixture timed out');
        await frame();
    };
    const state = () => JSON.parse(document.querySelector('[data-volume-state]').textContent);
    const button = (root, text) => [...root.querySelectorAll('button')].find(node => node.textContent.trim() === text);
    const dialog = name => document.querySelector(`[role="dialog"][aria-label="${name}"]`);
    const choose = async (label, value) => {
        const node = document.querySelector(`[aria-label="${label}"]`);
        node.value = value; node.dispatchEvent(new Event('change', { bubbles: true })); await frame();
    };
    const open = async () => {
        button(document, 'New volume').click(); await wait(() => dialog('Add volume'));
        const parent = dialog('Add volume');
        const input = parent.querySelector('input');
        input.value = 'bar'; input.dispatchEvent(new Event('input', { bubbles: true })); await frame();
        return parent;
    };
    const geometry = root => {
        const bounds = root.getBoundingClientRect();
        check(bounds.left >= 0 && bounds.right <= innerWidth + 1 && bounds.top >= 0 && bounds.bottom <= innerHeight + 1, 'Volume modal escaped viewport');
        const actions = [...root.querySelectorAll('.dialog-footer-actions button')];
        check(actions.length === 2, 'Shared volume footer actions missing');
        check(Math.abs(actions[0].getBoundingClientRect().height - actions[1].getBoundingClientRect().height) < 0.5, 'Volume footer heights differ');
        check(actions.every(node => getComputedStyle(node).marginTop === '0px' && getComputedStyle(node).marginBottom === '0px'), 'Volume footer vertical margins differ');
    };
    await wait(() => document.querySelector('[data-volume-state]'));
    const pane = document.querySelector('[data-background-pane]');
    pane.scrollTop = 100;
    const width = pane.getBoundingClientRect().width;
    const scroll = pane.scrollTop;
    for (const target of ['A3000', 'A4000_A5000']) {
        await choose('Preferred A-Series generation', target);
        const before = state();
        button(document, 'Delete bar').click();
        await wait(() => dialog('Delete volume') && !button(dialog('Delete volume'), 'Delete permanently').disabled);
        button(dialog('Delete volume'), 'Delete permanently').click();
        await wait(() => !dialog('Delete volume') && !state().exists);
        const parent = await open(); geometry(parent);
        button(parent, 'Add').click(); button(parent, 'Add').click();
        await wait(() => !dialog('Add volume') && state().exists);
        check(!dialog('Sampler load capacity'), 'Empty creation opened a capacity confirmation');
        check(state().writes === before.writes + 2 && state().refreshes === before.refreshes + 2, 'Delete/recreate wrote or refreshed more than once');
        check(state().checks === before.checks + 2 && state().lastTarget === target, 'Empty creation skipped preferred-target preflight');
        check(state().selected === 'bar' && state().revision === before.revision + 2, 'Created bar was not refreshed and selected');
        check(pane.scrollTop === scroll && pane.getBoundingClientRect().width === width, 'Automatic creation changed background scroll geometry');
    }
    const writes = state().writes;
    await choose('Scenario', 'ERROR');
    const failed = await open(); button(failed, 'Add').click();
    await wait(() => failed.querySelector('[role="alert"]'));
    check(state().writes === writes && !button(failed, 'Add').disabled, 'Inspection error wrote or left editing blocked');
    check(failed.querySelector('input').value === 'bar', 'Inspection failure lost typed name');
    button(failed, 'Cancel').click(); await frame();
    for (const scenario of ['DENIED', 'POPULATED']) {
        await choose('Scenario', scenario);
        const parent = await open(); const before = parent.getBoundingClientRect();
        button(parent, 'Add').click(); await wait(() => dialog('Sampler load capacity'));
        const child = dialog('Sampler load capacity'); geometry(child);
        check(Number(getComputedStyle(child.closest('.dialog-backdrop')).zIndex) > Number(getComputedStyle(parent.closest('.dialog-backdrop')).zIndex), 'Review is behind raised Add volume');
        check(!child.closest('[inert]') && !!parent.closest('[inert]'), 'Top child is inert or parent is interactive');
        check(child.contains(document.activeElement), 'Capacity child did not own focus');
        check(button(child, 'Continue').disabled === (scenario === 'DENIED'), 'Capacity refusal was not enforced');
        const action = button(child, 'Cancel'); const hit = action.getBoundingClientRect();
        check(action.contains(document.elementFromPoint(hit.x + hit.width / 2, hit.y + hit.height / 2)), 'Review Cancel is occluded');
        // Open a third modal and prove its close restores the same pending review.
        button(document, 'Nested utility').click(); await wait(() => dialog('Nested utility'));
        const utility = dialog('Nested utility'); const utilityPane = utility.querySelector('[data-utility-pane]'); utilityPane.scrollTop = 70;
        check(!utility.closest('[inert]') && !!child.closest('[inert]'), 'Third modal did not exclusively own interaction');
        utility.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
        await wait(() => !dialog('Nested utility'));
        check(dialog('Sampler load capacity') === child && !child.closest('[inert]'), 'Third-modal Escape dismissed or disabled the parent review');
        child.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
        await wait(() => !dialog('Sampler load capacity') && !button(parent, 'Add').disabled);
        check(!parent.closest('[inert]') && parent.querySelector('input').value === 'bar', 'Capacity cancellation lost the parent name or interaction');
        check(parent.contains(document.activeElement), 'Capacity cancellation did not return parent focus');
        const after = parent.getBoundingClientRect();
        check(before.width === after.width && before.height === after.height, 'Parent resized after validation/cancel');
        check(state().writes === writes && pane.scrollTop === scroll && pane.getBoundingClientRect().width === width, 'Cancelled review wrote or changed background geometry');
        button(parent, 'Cancel').click(); await frame();
    }
    for (const [scenario, recovery] of [['LOST_WAIT', 'Check status'], ['REFRESH_ERROR', 'Refresh']]) {
        await choose('Scenario', scenario);
        const before = state(); const parent = await open();
        button(parent, 'Add').click();
        await wait(() => button(parent, recovery));
        check(!button(parent, 'Add') && parent.querySelector('input').disabled, 'Uncertain/saved write permits resubmission');
        const dismissal = button(parent, scenario === 'LOST_WAIT' ? 'Cancel' : 'Done');
        check(dismissal.disabled === (scenario === 'LOST_WAIT'), 'Uncertain write permits dismissal');
        button(parent, recovery).click(); button(parent, recovery)?.click();
        await wait(() => !dialog('Add volume'));
        check(state().writes === before.writes + 1 && state().revision === before.revision + 1, 'Recovery repeated the mutation');
        check(state().selected === 'bar', 'Recovered volume was not refreshed and selected');
    }
    // Leave the populated review visible for screenshot inspection and real pointer/keyboard checks.
    await choose('Scenario', 'POPULATED');
    const parent = await open(); button(parent, 'Add').click();
    await wait(() => dialog('Sampler load capacity'));
    return { failures, state: state(), backgroundRestored: pane.scrollTop === scroll };
};
