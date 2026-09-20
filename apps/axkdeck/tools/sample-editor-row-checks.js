// Run against the workspace fixture in Chromium and native WebKitGTK.
window.runSampleEditorRegression = async function () {
    const failures = [], measurements = [];
    const assert = (condition, message) => { if (!condition) failures.push(message); };
    const settle = () => new Promise(resolve => setTimeout(resolve, 100));
    const click = async (selector, text) => {
        const node = [...document.querySelectorAll(selector)].find(node => !text || node.textContent.trim() === text);
        if (!node) throw new Error(`Missing ${selector}: ${text}`);
        node.click(); await settle();
    };
    const input = async (label, value) => {
        const node = document.querySelector(`input[aria-label="${label}"]`);
        node.value = String(value); node.dispatchEvent(new Event('input', { bubbles: true })); await settle();
    };
    const page = async (tab, sub) => { await click('[role=tab]', tab); await click('[aria-label="Sample subpages"] button', sub); };
    await settle();
    const rows = [...document.querySelectorAll('[data-collection-list="samples"] .contained-row')];
    assert(rows.length === 3, 'fixture uses real Sample collection rows');
    const toggle = document.querySelector('[aria-label="Editor panel"]');
    assert(toggle.getAttribute('aria-pressed') === 'false', 'editor starts closed');
    await click('[aria-label="Inspect Sample A"]');
    assert(toggle.getAttribute('aria-pressed') === 'true', 'sample selection opens editor');
    const splitter = document.querySelector('[aria-label="Resize editor panel"]');
    const initialSplit = splitter.getAttribute('aria-valuenow');
    splitter.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })); await settle();
    const resizedSplit = splitter.getAttribute('aria-valuenow');
    const availableHeight = document.querySelector('.main-stage').clientHeight - 8;
    assert(availableHeight > 360 ? resizedSplit !== initialSplit : resizedSplit === '50', 'splitter resizes within pane minimums after automatic opening');
    await click('[aria-label="Editor panel"]');
    assert(!document.querySelector('.device-editor'), 'manual close hides editor');
    await click('[aria-label="Inspect Sample A"]');
    assert(toggle.getAttribute('aria-pressed') === 'true', 'same sample selection reopens editor');
    assert(document.querySelector('[aria-label="Resize editor panel"]').getAttribute('aria-valuenow') === resizedSplit, 'reopening preserves splitter');
    await click('[aria-label="Editor panel"]');
    const first = document.querySelector('[aria-label="Inspect Sample A"]');
    first.focus();
    first.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })); await settle();
    assert(toggle.getAttribute('aria-pressed') === 'true', 'keyboard selection opens editor');
    assert(document.activeElement === document.querySelector('[aria-label="Inspect Sample B"]'), 'automatic opening preserves collection focus');
    const rect = node => {
        const r = node.getBoundingClientRect(); return [r.x, r.y, r.width, r.height];
    };
    const geometry = () => rows.map(row => [row, row.querySelector('strong'), row.querySelector('small'), row.querySelector('[aria-label="Stereo Sample"]'), row.querySelector('.contained-playback')].filter(Boolean).map(rect));
    for (const row of [rows[0], rows[2]]) {
        row.querySelector('.contained-identity').click(); await settle();
        const name = row.querySelector('strong').textContent;
        const before = JSON.stringify(geometry());
        const check = (stage, dirty) => {
            const r = row.getBoundingClientRect(), metadata = row.querySelector('small').getBoundingClientRect();
            assert(metadata.top >= r.top && metadata.bottom <= r.bottom, `${name}/${stage}: metadata leaves selection`);
            assert(JSON.stringify(geometry()) === before, `${name}/${stage}: row geometry shifts`);
            const marker = row.querySelector('[aria-label="Unsaved Sample edits"]');
            assert(dirty ? marker && getComputedStyle(marker).visibility !== 'hidden' : !marker || marker.getAttribute('aria-hidden') === 'true', `${name}/${stage}: dirty accessibility state`);
        };
        check('clean', false);
        await page('Map/Out', 'Mix & Key');
        const level = Number(document.querySelector('input[aria-label="Level"]').value);
        await input('Level', level - 1); check('form edit', true);
        await click('[aria-label="Editor panel"]');
        row.querySelector('.contained-identity').click(); await settle();
        assert(Number(document.querySelector('input[aria-label="Level"]')?.value) === level - 1, `${name}: reopening retains page and draft`);
        check('reopen', true);
        await click('[aria-label="Undo Sample edit"]'); check('undo', false);
        await page('Map/Out', 'Level scaling');
        const handle = document.querySelector('.breakpoint-graph button');
        handle.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowUp', bubbles: true }));
        handle.dispatchEvent(new KeyboardEvent('keyup', { key: 'ArrowUp', bubbles: true })); await settle();
        check('graph edit', true);
        await click('.device-editor button', 'Discard'); check('discard', false);
        await page('Map/Out', 'Mix & Key');
        await input('Level', level - 2); check('before save', true);
        await click('.device-editor button', 'Save'); check('saved', false);
        measurements.push({ name, row: rect(row), metadata: rect(row.querySelector('small')) });
    }
    assert(document.querySelector('[aria-label="Write count"]').textContent === '2', 'only two explicit saves submitted');
    await page('Filter', 'Sample EQ');
    await input('EQ width', 6);
    const handle = document.querySelector('[data-handle="frequency-gain"]');
    for (const [event, expected] of [[{ deltaY: -100 }, 6.5], [{ deltaY: -100, shiftKey: true }, 6.6], [{ deltaX: 100, shiftKey: true }, 6.5]]) {
        handle.dispatchEvent(new WheelEvent('wheel', { ...event, bubbles: true, cancelable: true })); await settle();
        assert(Number(document.querySelector('input[aria-label="EQ width"]').value) === expected, `EQ wheel width should be ${expected}`);
    }
    return { failures, measurements };
};
