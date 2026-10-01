window.runCapacityRefreshChecks = async function () {
    const failures = [];
    const assert = (ok, message) => { if (!ok) failures.push(message); };
    const wait = async (condition, message) => {
        const deadline = performance.now() + 7000;
        while (!condition()) {
            if (performance.now() > deadline) throw new Error(message);
            await new Promise(resolve => setTimeout(resolve, 20));
        }
    };
    const button = label => [...document.querySelectorAll('button')].find(node =>
        node.getAttribute('aria-label') === label || node.textContent.trim() === label);
    const panel = () => button('Sampler Capacity');
    const state = () => JSON.parse(document.querySelector('[data-inspections]').textContent);
    const profiles = () => document.querySelectorAll('.capacity-profile');
    const revisionReady = async (revision, id) => {
        await wait(() => state().calls.includes(`${revision}:${id}`) && profiles().length === 2,
            `Capacity did not refresh for ${revision}:${id}`);
        assert(!document.querySelector('[role="alert"]'), 'Unexpected inspection error');
    };
    try {
        await wait(() => document.querySelector('[data-tree-id="volume-1"]'), 'Initial tree did not load');
        document.querySelector('[data-tree-id="volume-1"]').click();
        await revisionReady(1, 'volume-1');
        assert(panel().getAttribute('aria-expanded') === 'false', 'Initial fit must remain collapsed');
        button('Complete floppy import').click();
        await revisionReady(2, 'volume-2');
        assert(!state().calls.includes('2:volume-1'), 'Refreshed inspector requested an obsolete scope');
        assert(panel().getAttribute('aria-expanded') === 'false', 'Refreshed fit must remain collapsed');
        panel().click();
        assert(document.querySelector('.inspector').textContent.includes('123.1 / 768 KiB'), 'Updated capacity value missing');
        button('Complete package import').click();
        await revisionReady(3, 'volume-3');
        assert(document.querySelector('.inspector h3').textContent === 'New import', 'New destination not shown');
        button('Complete audio import').click();
        await revisionReady(4, 'volume-1');
        button('Delay responses').click();
        button('Complete floppy import').click();
        await wait(() => state().pending === 1, 'Delayed inspection not submitted');
        panel().click();
        assert(document.querySelector('.inspector').textContent.includes('Inspecting capacity...'), 'Loading state missing');
        button('Exceed capacity').click();
        await wait(() => state().pending === 2, 'Second inspection not submitted');
        panel().click(); panel().click();
        button('Release responses').click();
        await revisionReady(6, 'volume-6');
        assert(panel().getAttribute('aria-expanded') === 'false', 'Pending manual collapse was overwritten');
        panel().click();
        assert([...document.querySelectorAll('.capacity-profile-heading span')].every(node => node.textContent === 'Does not fit'),
            'Earlier delayed report replaced current violation');
        button('Resume responses').click();
        button('Exceed capacity').click();
        await revisionReady(7, 'volume-7');
        assert(panel().getAttribute('aria-expanded') === 'true', 'Violation did not auto-expand');
        button('Fail inspection').click();
        await wait(() => document.querySelector('[role="alert"]'), 'Failure not reported');
        panel().click();
        assert(button('Retry capacity inspection'), 'Retry action missing');
        button('Restore inspection').click(); button('Retry capacity inspection').click();
        await revisionReady(8, 'volume-8');
        document.querySelector('.program-row').click();
        await wait(() => !panel(), 'Program selection did not show object inspector');
        button('Complete floppy import').click();
        await wait(() => document.querySelector('[data-tree-id="volume-9"]'), 'Final tree did not refresh');
        assert(!panel(), 'Refresh reopened volume details after Program selection');
    } catch (error) { failures.push(error.message); }
    return { failures, inspections: state() };
};
