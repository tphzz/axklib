window.runLogViewerChecks = async function () {
    const failures = [];
    const check = (condition, message) => { if (!condition) failures.push(message); };
    const wait = async (predicate) => {
        for (let i = 0; i < 100; i++) { if (predicate()) return; await new Promise((r) => setTimeout(r, 50)); }
        throw new Error(`Log viewer check timed out: ${predicate}`);
    };
    await wait(() => document.querySelectorAll('.log-record').length > 0);
    const byLabel = (label) => document.querySelector(`[aria-label="${label}"]`);
    const switches = [...document.querySelectorAll('[role="switch"]')];
    switches.find((button) => button.textContent.includes('Word wrap')).click();
    await new Promise((r) => requestAnimationFrame(r));
    const viewport = { scrollWidth: document.documentElement.scrollWidth, width: innerWidth };
    check(viewport.scrollWidth <= viewport.width + 1, `Horizontal viewport overflow: ${JSON.stringify(viewport)}`);
    const text = document.querySelector('.log-text');
    byLabel('Log entries').focus();
    const range = document.createRange(); range.selectNodeContents(text);
    getSelection().removeAllRanges(); getSelection().addRange(range);
    document.dispatchEvent(new Event('selectionchange'));
    const selection = getSelection().toString();
    check(selection.length > 0, 'No text could be selected');
    window.logFixture.append();
    await wait(() => document.querySelector('.log-new'));
    check(getSelection().toString() === selection, 'Live update replaced selection');
    check(switches.find((button) => button.textContent.includes('Follow latest')).getAttribute('aria-checked') === 'false', 'Selection did not pause follow');
    byLabel('Clear view').click();
    await wait(() => document.querySelector('.log-empty'));
    window.logFixture.append(3);
    await wait(() => document.querySelectorAll('.log-record').length === 3);
    byLabel('Save logs').click();
    await wait(() => document.querySelector('[role="menuitem"]'));
    document.querySelector('[role="menuitem"]').click();
    await wait(() => window.logFixture.saves.length === 1);
    check(window.logFixture.saves[0].since === 251, 'Save did not retain clear boundary');
    byLabel('Show retained history').click();
    await wait(() => document.querySelectorAll('.log-record').length > 3);
    check(document.querySelector('.log-footer').getBoundingClientRect().bottom <= innerHeight + 1, 'Footer outside viewport');
    return { failures, entries: document.querySelectorAll('.log-record').length, selectionPreserved: true, viewport };
};
