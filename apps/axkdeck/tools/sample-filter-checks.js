// Shared rendered checks for Chromium and native WebKitGTK.
window.runSampleEditorRegression = async function () {
    const failures = [], measurements = [];
    const assert = (condition, message) => { if (!condition) failures.push(message); };
    const settle = () => new Promise(resolve => setTimeout(resolve, 70));
    const click = async (selector, text) => {
        const node = [...document.querySelectorAll(selector)].find(node => !text || node.textContent.trim() === text);
        if (!node) throw new Error(`Missing ${selector}: ${text}`);
        node.click(); await settle();
    };
    const input = async (label, value) => {
        const node = document.querySelector(`input[aria-label="${label}"]`);
        node.value = String(value); node.dispatchEvent(new Event('input', { bubbles: true })); await settle();
    };
    const key = async (node, key, altKey = false) => {
        for (const type of ['keydown', 'keyup']) node.dispatchEvent(new KeyboardEvent(type, { key, altKey, bubbles: true }));
        await settle();
    };
    await click('[aria-label="Inspect Sample A"]');
    await click('[role=tab]', 'Filter');
    await click('.sample-pages button', 'Filter');
    await input('Cutoff', 44); await input('Q / Width', 4); await input('Gain', 8);
    const selectType = async type => {
        await click('button[aria-label="Filter type"][aria-haspopup="listbox"]');
        document.querySelectorAll('[role="listbox"][aria-label="Filter type"] [role="option"]')[type].click();
        await settle();
    };
    for (let type = 0; type <= 16; type++) {
        await selectType(type);
        const handles = [...document.querySelectorAll('[data-handle]:not([data-handle="gain"])')];
        assert(handles.length === (type === 0 ? 0 : type < 10 ? 1 : 2), `type ${type}: cutoff handle count`);
        const pairs = document.querySelector('[data-trace="filter"]').getAttribute('d').split(/[ML]/).slice(1).map(pair => pair.split(',').map(Number));
        for (const handle of handles.filter(handle => !handle.hidden)) {
            const x = parseFloat(handle.style.left) * 10, y = parseFloat(handle.style.top) * 2;
            const next = Math.min(pairs.length - 1, Math.ceil(x / 2.5));
            const [lx, ly] = pairs[Math.max(0, next - 1)], [rx, ry] = pairs[next];
            const expected = rx === lx ? ry : ly + (ry - ly) * (x - lx) / (rx - lx);
            assert(Math.abs(y - expected) < 0.15, `type ${type}: handle is not on response`);
            const bounds = document.querySelector('.graph-frame').getBoundingClientRect();
            const r = handle.getBoundingClientRect();
            assert(r.left >= bounds.left && r.right <= bounds.right && r.top >= bounds.top && r.bottom <= bounds.bottom, `type ${type}: handle containment`);
        }
        measurements.push({ type, handles: handles.length });
    }
    await selectType(6);
    await input('Q / Width', 4);
    const handle = document.querySelector('[data-handle="cutoff-q"]');
    for (const [shiftKey, expected] of [[false, 7], [true, 8]]) {
        handle.dispatchEvent(new WheelEvent('wheel', { deltaY: -100, shiftKey, bubbles: true, cancelable: true })); await settle();
        assert(Number(document.querySelector('input[aria-label="Q / Width"]').value) === expected, `Q wheel step ${expected}`);
    }
    await click('[aria-label="Undo Sample edit"]');
    assert(Number(document.querySelector('input[aria-label="Q / Width"]').value) === 4, 'Q wheel group undoes once');
    await key(handle, 'ArrowRight');
    assert(Number(document.querySelector('input[aria-label="Cutoff"]').value) === 45, 'horizontal key edits only cutoff');
    assert(Number(document.querySelector('input[aria-label="Gain"]').value) === 8, 'cutoff gesture preserves gain');
    const gainHandle = document.querySelector('[data-handle="gain"]');
    gainHandle.focus(); await settle();
    assert(document.querySelector('[role="tooltip"]')?.textContent.includes('Shift'), 'gain focus explains fine adjustment');
    await key(gainHandle, 'ArrowUp');
    assert(Number(document.querySelector('input[aria-label="Gain"]').value) === 9, 'gain handle edits gain');
    assert(Number(document.querySelector('input[aria-label="Cutoff"]').value) === 45, 'gain preserves cutoff');
    await click('[aria-label="Undo Sample edit"]');
    assert(Number(document.querySelector('input[aria-label="Gain"]').value) === 8, 'gain keyboard gesture undoes once');
    await input('Cutoff', 44);
    await click('.device-editor button', 'Save');
    assert(document.querySelector('[data-handle="cutoff-q"]') === handle, 'Save retains filter graph DOM');
    await click('[role=tab]', 'EG');
    await click('.sample-pages button', 'Amplitude');
    await input('Attack rate', 90);
    await click('[aria-label="Zoom envelope out"]');
    const graph = document.querySelector('[data-trace="envelope"]');
    const curve = graph.getAttribute('d');
    const panel = document.querySelector('.sample-panel');
    const forms = document.querySelector('.graph-controls');
    forms.scrollTop = 65;
    const scroll = forms.scrollTop;
    await click('.device-editor button', 'Save');
    assert(document.querySelector('[data-trace="envelope"]') === graph && graph.getAttribute('d') === curve, 'Save retains envelope DOM and zoom');
    assert(document.querySelector('.sample-panel') === panel, 'Save retains active subpage');
    assert(forms.scrollTop === scroll, 'Save retains form scroll');
    assert(!document.querySelector('[aria-label="Unsaved Sample edits"]'), 'Save clears collection dirty marker');
    await click('[role=tab]', 'Filter'); await click('.sample-pages button', 'Filter');
    return { failures, measurements };
};
