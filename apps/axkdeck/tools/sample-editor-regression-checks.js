// Shared DOM assertions for Chromium and native WebKitGTK page zoom.
window.runSampleEditorRegression = async function () {
    const failures = [], measurements = [];
    const assert = (condition, message) => { if (!condition) failures.push(message); };
    const settle = () => new Promise(resolve => setTimeout(resolve, 100));
    const visible = node => node && node.getBoundingClientRect().width > 0 && node.getBoundingClientRect().height > 0;
    const click = async (selector, text) => {
        const node = [...document.querySelectorAll(selector)].find(node => node.textContent.trim() === text);
        if (!node) throw new Error(`Missing ${selector}: ${text}`);
        node.click(); await settle();
    };
    const input = async (name, value) => {
        const node = document.querySelector(`input[aria-label="${name}"]`);
        node.value = value;
        node.dispatchEvent(new Event('input', { bubbles: true }));
        await settle();
    };
    const page = async (tab, subpage) => {
        await click('[role=tab]', tab);
        if (subpage) await click('[aria-label="Sample subpages"] button', subpage);
        const panel = document.querySelector('.sample-panel');
        assert(panel.scrollWidth <= panel.clientWidth + 1, `${tab}/${subpage}: horizontal panel overflow`);
        for (const field of panel.querySelectorAll('.parameter-field')) {
            if (!visible(field)) continue;
            const bounds = field.getBoundingClientRect();
            for (const control of field.querySelectorAll('.editor-number,.choice-control,.parameter-unavailable')) {
                if (!visible(control)) continue;
                const box = control.getBoundingClientRect();
                assert(box.left >= bounds.left - 1 && box.right <= bounds.right + 1, `${tab}/${subpage}: ${field.textContent.trim()} leaves field bounds`);
            }
        }
    };
    const geometry = () => ['.sample-transport', '.wave-surface'].map(selector => {
        const node = document.querySelector(selector), box = node.getBoundingClientRect();
        return { x: box.x, y: box.y, width: box.width, height: box.height };
    });
    await settle();
    const editorToggle = document.querySelector('[aria-label="Editor panel"]');
    if (!document.querySelector('.device-editor') && editorToggle) { editorToggle.click(); await settle(); }
    const initial = geometry();
    document.querySelector('.audition-button').click();
    for (let i = 0; i < 30 && !document.querySelector('.audition-button').textContent.includes('Stop'); i++) await settle();
    assert(document.querySelector('.audition-button').textContent.includes('Stop'), 'audition reaches playing state');
    assert(JSON.stringify(geometry()) === JSON.stringify(initial), 'starting audition resizes waveform/footer');
    document.querySelector('.audition-button').click(); await settle();
    assert(JSON.stringify(geometry()) === JSON.stringify(initial), 'stopping audition resizes waveform/footer');
    measurements.push({ transport: geometry() });
    assert(!document.querySelector('.sample-transport [aria-label="Loop mode"]'), 'Loop mode is not an audition preference');
    assert(document.querySelector('.waveform-page [aria-label="Loop mode"]'), 'Waveform owns saved Loop mode');
    await page('Trim/Loop', 'Sample Info');
    const metadata = document.querySelector('.source-metadata');
    assert(metadata?.textContent.includes('Source:'), 'source duration is labelled in header');
    assert(!document.querySelector('.settings-summary'), 'source duration reserves a footer row');
    const headerBox = metadata?.closest('.editor-toolbar').getBoundingClientRect();
    const metadataBox = metadata?.getBoundingClientRect();
    assert(metadataBox && metadataBox.top >= headerBox.top && metadataBox.bottom <= headerBox.bottom, 'source metadata leaves header');
    await page('Map/Out', 'Mix & Key');
    await page('Map/Out', 'Pitch');
    const tabs = () => [...document.querySelectorAll('.navigation [role=tab]')].map(node => node.getBoundingClientRect().x);
    const cleanTabs = JSON.stringify(tabs());
    await input('Coarse tune', '3');
    assert(JSON.stringify(tabs()) === cleanTabs, 'dirty marker moves tabs');
    document.querySelector('[aria-label="Undo Sample edit"]').click(); await settle();
    assert(JSON.stringify(tabs()) === cleanTabs, 'undo moves tabs');
    if (location.search.includes('short-stereo')) {
        const help = document.querySelector('.attribute-help-label[aria-label="Portamento type"]');
        help.focus(); await settle();
        assert(document.querySelector('[role=tooltip]')?.textContent.includes('full parameter layout'), 'short-layout conversion is explained on focus');
        help.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })); await settle();
        assert(!document.querySelector('[role=tooltip]'), 'conversion help closes with Escape');
    }
    await page('Map/Out', 'Expansion & Velocity');
    if (location.search.includes('short-stereo')) {
        assert(!document.querySelector('input[aria-label="Detune"]').disabled, 'true stereo supports scalar detune editing');
        assert(!document.querySelector('input[aria-label="Low crossfade"]').disabled, 'short-layout crossfade is editable through conversion');
    }
    await page('Map/Out', 'Level scaling');
    const scaling = document.querySelector('.breakpoint-graph button');
    scaling.focus(); scaling.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowUp', bubbles: true }));
    scaling.dispatchEvent(new KeyboardEvent('keyup', { key: 'ArrowUp', bubbles: true })); await settle();
    const readout = document.querySelector('.graph-readout').getBoundingClientRect();
    assert(readout.bottom <= document.querySelector('.graph-surface').getBoundingClientRect().top, 'scaling readout overlaps plot');
    await page('EG', 'Amplitude');
    const attack = document.querySelector('[data-handle="1"]');
    attack.focus(); attack.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowLeft', shiftKey: true, bubbles: true })); await settle();
    const path = () => document.querySelector('[data-trace="envelope"]').getAttribute('d');
    const during = path();
    attack.dispatchEvent(new KeyboardEvent('keyup', { key: 'ArrowLeft', bubbles: true })); await settle();
    assert(path() === during, 'envelope refits after release');
    await page('EG', 'Filter'); await page('EG', 'Amplitude');
    assert(path() === during, 'envelope viewport changes after page switch');
    document.querySelector('[aria-label="Fit envelope to width"]').click(); await settle();
    assert(path() !== during, 'explicit Fit changes envelope viewport');
    for (const label of ['Attack rate', 'Decay rate', 'Release rate']) await input(label, '127');
    document.querySelector('[aria-label="Fit envelope to width"]').click(); await settle();
    assert(parseFloat(document.querySelector('[data-handle="1"]').style.left) < 1, 'maximum attack has artificial padding');
    await page('EG', 'Pitch');
    for (const label of ['Attack rate', 'Decay rate', 'Release rate']) await input(label, '127');
    document.querySelector('[aria-label="Fit envelope to width"]').click(); await settle();
    await input('Attack rate', '126');
    assert(!visible(document.querySelector('[data-handle="4"]')), 'offscreen envelope endpoint leaks past plot');
    document.querySelector('[aria-label="Fit envelope to width"]').click(); await settle();
    assert(visible(document.querySelector('[data-handle="4"]')), 'Fit fails to restore envelope endpoint');
    await page('Filter', 'Sample EQ');
    assert(document.querySelectorAll('[data-handle]').length === 1, 'EQ has one frequency/gain handle');
    const position = () => document.querySelector('[data-handle]').style.left;
    const before = position();
    for (const gain of ['12', '0', '-12']) { await input('EQ gain (dB)', gain); assert(position() === before, `EQ frequency moves at gain ${gain}`); }
    const eqHandle = document.querySelector('[data-handle]');
    const widthInput = document.querySelector('input[aria-label="EQ width"]');
    const widthBefore = Number(widthInput.value);
    eqHandle.dispatchEvent(new WheelEvent('wheel', { deltaY: -1, bubbles: true, cancelable: true }));
    await settle();
    assert(Number(widthInput.value) > widthBefore, 'EQ wheel changes width');
    await input('EQ frequency selection', '10'); await input('EQ gain (dB)', '-4'); await input('EQ width', '1');
    const eqPath = document.querySelector('[data-trace="eq"]').getAttribute('d');
    const ys = [...eqPath.matchAll(/[ML]([^,]+),([^ ]+)/g)].map(match => Number(match[2]));
    assert(Math.abs(Math.max(...ys) - 22 / 36 * 200) < 0.1, 'low-frequency EQ response misses parameter gain');
    document.querySelector('[aria-label="Show coefficient response"]').click(); await settle();
    assert(document.querySelector('[data-trace="coefficients"]'), 'coefficient overlay unavailable');
    assert(document.querySelector('[data-trace="eq"]').getAttribute('d') === eqPath, 'overlay moves editing curve');
    await page('LFO');
    const panel = document.querySelector('.graph-panel');
    const groups = document.querySelector('.parameter-group-switch');
    assert(visible(groups) === (panel.clientWidth < 900), 'graph stacking and group navigation disagree');
    if (visible(groups)) await click('.parameter-group-switch button', 'Depth');
    for (const label of ['Pitch depth', 'Cutoff depth', 'Amplitude depth']) assert(visible(document.querySelector(`input[aria-label="${label}"]`)), `${label} is not reachable`);
    await input('Pitch depth', '70');
    assert(document.querySelector('[data-trace="pitch"],.modulation'), 'LFO trace is rendered');
    await page('MIDI/CTRL', 'MIDI Set');
    assert(document.querySelector('input[aria-label="Pitch bend range"]'), 'MIDI Set owns Pitch bend');
    assert(document.querySelector('input[aria-label="Velocity Offset"]'), 'MIDI Set owns Velocity Offset');
    await page('MIDI/CTRL', 'Control');
    assert(document.querySelectorAll('tbody tr').length === 6, 'MIDI controls are not six rows');
    assert(document.querySelectorAll('[role=combobox]').length === 18, 'MIDI selectors are not all searchable');
    const controller = document.querySelector('[role=combobox]');
    controller.scrollIntoView({ block: 'nearest' }); await settle();
    controller.click(); await settle();
    const upwards = document.querySelector('[role=listbox]').getBoundingClientRect().bottom <= controller.getBoundingClientRect().top;
    for (const query of ['071', 'no matches here', '', '071']) {
        controller.value = query;
        controller.dispatchEvent(new Event('input', { bubbles: true })); await settle();
        const anchor = controller.getBoundingClientRect(), popup = document.querySelector('[role=listbox]').getBoundingClientRect();
        const gap = upwards ? anchor.top - popup.bottom : popup.top - anchor.bottom;
        assert(Math.abs(gap - 4) <= 1, `filtered popup loses anchor: ${gap}`);
        assert(popup.top >= 0 && popup.bottom <= innerHeight + 1, 'filtered popup leaves viewport');
    }
    controller.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })); await settle();
    assert(!document.querySelector('[role=listbox]'), 'filtered popup fails to dismiss');
    const samplePanel = document.querySelector('.sample-panel');
    if (document.querySelector('.device-editor').clientWidth >= 900 && samplePanel.clientHeight >= 235)
        assert(samplePanel.scrollHeight <= samplePanel.clientHeight + 1, 'six Control rows overflow a desktop editor');
    assert(document.documentElement.scrollWidth <= document.documentElement.clientWidth + 1, 'horizontal document overflow');
    assert(document.querySelector('[aria-label="Write count"]').textContent === '0', 'editing submitted an unexpected write');
    measurements.push({ viewport: [innerWidth, innerHeight], editor: document.querySelector('.device-editor').clientWidth });
    return { failures, measurements };
};
